#include "ffi_native.h"

#include "ffi_cdecl.h"
#include "ffi_object.h"
#include "lsp_wire.h"
#include "module.h"
#include "parser.h"
#include "pkg_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <thread>
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#ifdef cdecl
#undef cdecl
#endif
#else
#include <dlfcn.h>
#include <fcntl.h>
#if defined(__linux__) && defined(__GLIBC__)
#include <link.h>
#endif
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace native_ffi {
namespace {

namespace fs = std::filesystem;
constexpr size_t kMaxFileBytes = 16 * 1024 * 1024;
constexpr std::string_view kContractMagic = "BRATFFI1 ";
constexpr std::string_view kContractEnd = "BRATFFI1-END";

static std::string trim_sv(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

static bool read_file_bytes(const std::string &path, std::string &out, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0 || static_cast<size_t>(sz) > kMaxFileBytes) {
        err = "file too large: " + path;
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return true;
}

static std::string norm_lf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            out.push_back('\n');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string find_bin_in_path(std::string_view bin) {
    const char *path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return {};
#ifdef _WIN32
    constexpr char sep_ch = ';';
#else
    constexpr char sep_ch = ':';
#endif
    std::string_view pv(path_env);
    size_t pos = 0;
    while (pos <= pv.size()) {
        size_t sep = pv.find(sep_ch, pos);
        std::string_view dir = (sep == std::string_view::npos) ? pv.substr(pos) : pv.substr(pos, sep - pos);
        if (!dir.empty()) {
#ifdef _WIN32
            if (fs::path(bin).extension().empty()) {
                fs::path cand_exe = fs::path(dir) / (std::string(bin) + ".exe");
                std::error_code ec_exe;
                if (fs::is_regular_file(cand_exe, ec_exe)) return cand_exe.string();
            }
#endif
            fs::path cand = fs::path(dir) / std::string(bin);
            std::error_code ec;
            if (fs::is_regular_file(cand, ec)) return cand.string();
        }
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return {};
}

// Collect `extern struct` / `extern union` / `extern packed struct`, `extern fn`, `pub` wrappers,
// and literal `lib_open("...")` / `lib_sym(h, "sym", "sig")` dynamic plugin declarations.
struct BratFfiSurface {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> structs;
    std::set<std::string> packed;
    std::set<std::string> unions;
    std::vector<ExternDecl> fns;
    bool has_dynamic_plugin_call = false;
    bool has_unresolvable_plugin_call = false;
};

static std::string plugin_norm_slot(std::string_view raw) {
    std::string s;
    for (char c : raw) if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (s == "ownptr" || s == "moveptr") return "own ptr";
    if (s == "outptr") return "out ptr";
    if (s == "&ptr" || s == "refptr") return "&ptr";
    if (s.rfind("fn(", 0) == 0) {
        for (;;) {
            bool hit = false;
            for (const char *suf : {"thread(same)", "thread(any)", "reenter(yes)", "reenter(no)"}) {
                size_t sl = std::strlen(suf);
                if (s.size() >= sl && s.compare(s.size() - sl, sl, suf) == 0) {
                    s.resize(s.size() - sl);
                    hit = true;
                    break;
                }
            }
            if (!hit) break;
        }
    }
    return s;
}

static bool plugin_parse_sig(std::string_view raw_text, std::string &ret_out, std::vector<std::string> &params_out) {
    std::string raw = trim_sv(raw_text);
    size_t open_i = raw.find('(');
    if (open_i == 0 || open_i == std::string::npos || raw.back() != ')') return false;
    int depth = 0;
    for (size_t i = open_i; i < raw.size(); ++i) {
        if (raw[i] == '(') ++depth;
        else if (raw[i] == ')') {
            --depth;
            if (depth == 0 && i != raw.size() - 1) return false;
        }
    }
    if (depth != 0) return false;
    ret_out = plugin_norm_slot(std::string_view(raw).substr(0, open_i));
    std::string_view rest = std::string_view(raw).substr(open_i + 1, raw.size() - open_i - 2);
    params_out.clear();
    size_t start = 0;
    depth = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '(') ++depth;
        else if (rest[i] == ')') --depth;
        else if (rest[i] == ',' && depth == 0) {
            std::string piece = plugin_norm_slot(rest.substr(start, i - start));
            if (!piece.empty()) params_out.push_back(piece);
            start = i + 1;
        }
    }
    std::string tail = plugin_norm_slot(rest.substr(start));
    if (!tail.empty()) params_out.push_back(tail);
    return !ret_out.empty();
}

static void scan_plugin_stmts(const std::vector<StmtP> &stmts,
                              std::map<std::string, std::optional<std::string>> &handles,
                              std::map<std::string, std::pair<std::optional<std::string>, std::string>> &syms,
                              const std::set<std::string> &explicit_names,
                              std::set<std::string> &seen_keys,
                              BratFfiSurface &surf);

static void scan_plugin_expr(const Expr *e,
                             const std::map<std::string, std::optional<std::string>> &handles,
                             const std::map<std::string, std::pair<std::optional<std::string>, std::string>> &syms,
                             const std::set<std::string> &explicit_names,
                             std::set<std::string> &seen_keys,
                             BratFfiSurface &surf) {
    if (!e) return;
    if (e->k == EK::Call) {
        if (e->a == "lib_open" || e->a == "lib_sym" || e->a == "lib_close" || e->a == "call")
            surf.has_dynamic_plugin_call = true;
        if (e->a == "call") {
            bool resolved_call = false;
            if (e->kids.size() >= 2 &&
                e->kids[0] && e->kids[0]->k == EK::Var && syms.count(e->kids[0]->a) &&
                e->kids[1] && (e->kids[1]->k == EK::Str || e->kids[1]->k == EK::RStr)) {
                const auto &[lib_opt, sname] = syms.at(e->kids[0]->a);
                std::string ret_t;
                std::vector<std::string> params;
                if (plugin_parse_sig(e->kids[1]->a, ret_t, params)) {
                    resolved_call = true;
                    if (!explicit_names.count(sname)) {
                        std::string key = sname + ":" + ret_t;
                        for (const auto &p : params) key += "," + p;
                        if (seen_keys.insert(key).second) {
                            auto slot_ann = [](const std::string &sl) -> std::string {
                                if (sl == "own ptr" || sl == "&ptr" || sl == "out ptr") return "ptr";
                                return sl;
                            };
                            ExternDecl d;
                            d.lib = lib_opt;
                            d.name = sname;
                            if (ret_t != "none") d.ret = slot_ann(ret_t);
                            d.owned = (ret_t == "own ptr");
                            d.effects.push_back("__plugin__");
                            for (size_t i = 0; i < params.size(); ++i) {
                                Param p;
                                p.name = "a" + std::to_string(i);
                                p.mode = (params[i] == "own ptr") ? Mode::Move
                                       : ((params[i] == "out ptr") ? Mode::Mut : Mode::Ref);
                                p.ann = slot_ann(params[i]);
                                d.params.push_back(std::move(p));
                            }
                            surf.fns.push_back(std::move(d));
                        }
                    }
                }
            }
            if (!resolved_call) surf.has_unresolvable_plugin_call = true;
        }
    }
    for (const auto &k : e->kids) scan_plugin_expr(k.get(), handles, syms, explicit_names, seen_keys, surf);
    if (e->fn) {
        auto sub_h = handles;
        auto sub_s = syms;
        scan_plugin_stmts(e->fn->body, sub_h, sub_s, explicit_names, seen_keys, surf);
    }
}

static void scan_plugin_stmts(const std::vector<StmtP> &stmts,
                              std::map<std::string, std::optional<std::string>> &handles,
                              std::map<std::string, std::pair<std::optional<std::string>, std::string>> &syms,
                              const std::set<std::string> &explicit_names,
                              std::set<std::string> &seen_keys,
                              BratFfiSurface &surf) {
    for (const auto &sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if ((s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::Set) && !s.a.empty()) {
            const Expr *rhs = s.e1.get();
            if (s.k == SK::Set) {
                handles.erase(s.a);
                syms.erase(s.a);
            }
            if (rhs && rhs->k == EK::Call) {
                if (rhs->a == "lib_open" && rhs->kids.size() == 1) {
                    surf.has_dynamic_plugin_call = true;
                    const Expr *arg = rhs->kids[0].get();
                    if (arg && (arg->k == EK::Str || arg->k == EK::RStr)) handles[s.a] = arg->a;
                    else { handles[s.a] = std::nullopt; surf.has_unresolvable_plugin_call = true; }
                } else if (rhs->a == "lib_sym" && rhs->kids.size() == 2) {
                    surf.has_dynamic_plugin_call = true;
                    const Expr *harg = rhs->kids[0].get();
                    const Expr *sarg = rhs->kids[1].get();
                    if (harg && harg->k == EK::Var && handles.count(harg->a) &&
                        sarg && (sarg->k == EK::Str || sarg->k == EK::RStr)) {
                        syms[s.a] = {handles.at(harg->a), sarg->a};
                    } else {
                        surf.has_unresolvable_plugin_call = true;
                    }
                }
            }
        }
        scan_plugin_expr(s.e1.get(), handles, syms, explicit_names, seen_keys, surf);
        scan_plugin_expr(s.e2.get(), handles, syms, explicit_names, seen_keys, surf);
        scan_plugin_expr(s.e3.get(), handles, syms, explicit_names, seen_keys, surf);
        scan_plugin_stmts(s.body, handles, syms, explicit_names, seen_keys, surf);
        scan_plugin_stmts(s.body2, handles, syms, explicit_names, seen_keys, surf);
        for (const Stmt *el = s.els.get(); el != nullptr; el = el->els.get()) {
            scan_plugin_expr(el->e1.get(), handles, syms, explicit_names, seen_keys, surf);
            scan_plugin_expr(el->e2.get(), handles, syms, explicit_names, seen_keys, surf);
            scan_plugin_expr(el->e3.get(), handles, syms, explicit_names, seen_keys, surf);
            scan_plugin_stmts(el->body, handles, syms, explicit_names, seen_keys, surf);
            scan_plugin_stmts(el->body2, handles, syms, explicit_names, seen_keys, surf);
        }
        for (const auto &fn : s.fns) {
            auto sub_h = handles;
            auto sub_s = syms;
            scan_plugin_stmts(fn.body, sub_h, sub_s, explicit_names, seen_keys, surf);
        }
    }
}

static ExternDecl clone_extern_decl(const ExternDecl &src) {
    ExternDecl dst;
    dst.lib = src.lib;
    dst.name = src.name;
    dst.ret = src.ret;
    dst.varargs = src.varargs;
    dst.owned = src.owned;
    dst.checked = src.checked;
    dst.effects = src.effects;
    dst.contracts = src.contracts;
    for (const auto &p : src.params) {
        Param cp;
        cp.name = p.name;
        cp.mode = p.mode;
        cp.ann = p.ann;
        dst.params.push_back(std::move(cp));
    }
    return dst;
}

static void collect_top_decls(const std::vector<StmtP> &stmts, BratFfiSurface &surf) {
    for (const auto &sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (s.k == SK::ExternStruct) {
            std::vector<std::pair<std::string, std::string>> fields;
            for (const auto &f : s.fields) fields.emplace_back(f.name, f.ann.value_or("i64"));
            surf.structs[s.a] = std::move(fields);
            if (s.opt1 && *s.opt1 == "union") surf.unions.insert(s.a);
            else if (s.opt1 && *s.opt1 == "packed") surf.packed.insert(s.a);
        } else if (s.k == SK::Extern) {
            surf.fns.push_back(clone_extern_decl(s.ext));
        } else if (s.k == SK::Pub) {
            collect_top_decls(s.body, surf);
        }
    }
}

static BratFfiSurface extract_surface(const std::vector<StmtP> &body) {
    BratFfiSurface surf;
    collect_top_decls(body, surf);
    std::set<std::string> explicit_names;
    for (const auto &d : surf.fns) explicit_names.insert(d.name);
    std::map<std::string, std::optional<std::string>> handles;
    std::map<std::string, std::pair<std::optional<std::string>, std::string>> syms;
    std::set<std::string> seen_keys;
    scan_plugin_stmts(body, handles, syms, explicit_names, seen_keys, surf);
    return surf;
}

bool same_brat_ffi_scalar(std::string_view a, std::string_view b, const TargetModel &tm) {
    if (a == b) return true;
    if ((a == "ptr" || a == "str") && (b == "ptr" || b == "str")) return false;
    auto is64 = [](std::string_view x) { return x == "i64" || x == "u64" || x == "usize"; };
    if (is64(a) && is64(b)) return brat_abi_width(a, tm) == brat_abi_width(b, tm);
    auto isnarrow = [](std::string_view x) {
        return x == "u8" || x == "u16" || x == "i8" || x == "i16" || x == "i32" || x == "u32";
    };
    if (isnarrow(a) && isnarrow(b)) {
        auto wa = brat_abi_width(a, tm), wb = brat_abi_width(b, tm);
        return wa.has_value() && wa == wb;
    }
    return false;
}

static std::string size_note_str(std::string_view t, const TargetModel &tm) {
    auto w = brat_abi_width(t, tm);
    if (w && t != "str" && t != "ptr" && t.rfind("ptr[", 0) != 0 && t.rfind("fn(", 0) != 0)
        return std::to_string(*w) + " bytes";
    if (t == "usize" || t == "str" || t == "ptr")
        return std::to_string(tm.ptr) + " bytes";
    return "unknown width";
}

static bool is_fixed_scalar(std::string_view x) {
    return x == "i8" || x == "u8" || x == "i16" || x == "u16" ||
           x == "i32" || x == "u32" || x == "i64" || x == "u64" ||
           x == "usize" || x == "f32" || x == "f64" || x == "bool";
}

struct CheckReport {
    int errors = 0;
    int warnings = 0;
    size_t proven = 0;
    std::vector<std::string> lines;
    std::vector<std::string> notes;

    void err(const std::string &msg) {
        ++errors;
        lines.push_back("  error: " + msg);
    }
    void fail(const std::string &sym, const std::string &why) {
        err(sym + ": " + why);
    }
};

static bool same_callback_sig(std::string_view brat_ann,
                              std::string_view ctype,
                              const CDecl &cdecl,
                              const std::set<std::string> &struct_ok,
                              const TargetModel &tm,
                              std::string &why_out);

static bool same_type_check(std::string_view btype_raw,
                            std::string_view ctype_raw,
                            const CDecl &cdecl,
                            const std::set<std::string> &struct_ok,
                            const TargetModel &tm) {
    std::string btype = trim_sv(btype_raw);
    auto rc = resolve_c_type(&cdecl, ctype_raw, tm);

    if (auto barr = parse_array_field_type(btype)) {
        if (rc.kind != "array" || rc.arr_count != barr->second) return false;
        if (rc.arr_elem_k == "struct") return barr->first == rc.arr_elem_e && struct_ok.count(barr->first) > 0;
        if (rc.arr_elem_k == "ptr") {
            std::string pe = element_of_ptr(barr->first);
            return (barr->first == "ptr" || !pe.empty()) && pointee_ok(pe, rc.arr_elem_e, struct_ok, tm);
        }
        return same_brat_ffi_scalar(barr->first, rc.arr_elem_k, tm);
    }

    if (btype.rfind("fn(", 0) == 0 || rc.kind == "fn") {
        std::string why;
        return same_callback_sig(btype, ctype_raw, cdecl, struct_ok, tm, why);
    }

    std::string elem = element_of_ptr(btype);
    if (!elem.empty()) {
        if (rc.kind == "str") return elem == "u8" || elem == "i8";
        return rc.kind == "ptr" && pointee_ok(elem, rc.extra, struct_ok, tm);
    }
    if (btype == "ptr" && rc.kind == "str") return true;
    if (btype == "ptr" && rc.kind == "ptr") {
        if (is_fixed_scalar(rc.extra) || struct_ok.count(rc.extra)) return false;
        return true;
    }
    if (btype == "str" && rc.kind == "ptr") return false;
    if (struct_ok.count(btype))
        return rc.kind == "struct" && rc.extra == btype && cdecl.structs.count(rc.extra) && !cdecl.structs.at(rc.extra).empty();
    if (rc.kind == "struct" || rc.kind == "array" || rc.kind == "array_unsized" ||
        rc.kind == "typedef" || rc.kind == "long_double" || rc.kind == "invalid")
        return false;
    return same_brat_ffi_scalar(btype, rc.kind, tm);
}

static std::vector<std::string> ident_words_of(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
            cur.push_back(ch);
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

static bool spelled_plain_char(std::string_view ctype) {
    bool has_char = false;
    for (const auto &w : ident_words_of(ctype)) {
        if (w == "char") has_char = true;
        if (w == "signed" || w == "unsigned" || w == "int8_t" || w == "uint8_t") return false;
    }
    return has_char;
}

// A callback slot is stricter than a top-level parameter: the callee, not BRAT, chooses what arrives. `str` is a
// `const char *`, `ptr` is a writable `void *` (the user_data slot), and `ptr[T]` is a writable `T *` that is
// neither a record nor a plain `char *`. Empty result: not a pointer slot, the scalar and record rules apply.
static std::optional<bool> callback_pointer_match(const std::string &btype,
                                                  std::string_view ctype,
                                                  const CDecl &cdecl,
                                                  const std::set<std::string> &struct_ok,
                                                  const TargetModel &tm) {
    auto rc = resolve_c_type(&cdecl, ctype, tm);
    std::string elem = element_of_ptr(btype);
    if (btype == "str") return rc.kind == "str";
    if (btype == "ptr") return rc.kind == "ptr" && rc.opaque && !is_const_pointee(ctype, &cdecl);
    if (elem.empty()) return std::nullopt;
    if (rc.kind != "ptr" || is_const_pointee(ctype, &cdecl) || struct_ok.count(elem)) return false;
    if ((elem == "u8" || elem == "i8") && spelled_plain_char(ctype)) return false;
    return pointee_ok(elem, rc.extra, struct_ok, tm);
}

static bool same_callback_slot(const std::string &btype,
                               std::string_view ctype,
                               const CDecl &cdecl,
                               const std::set<std::string> &struct_ok,
                               const TargetModel &tm) {
    if (auto pointer = callback_pointer_match(btype, ctype, cdecl, struct_ok, tm)) return *pointer;
    return same_type_check(btype, ctype, cdecl, struct_ok, tm);
}

static std::string callback_slot_why(const std::string &slot,
                                     const std::string &btype,
                                     std::string_view ctype,
                                     const CDecl &cdecl,
                                     const TargetModel &tm) {
    const bool pointer_slot = btype == "ptr" || !element_of_ptr(btype).empty();
    if (pointer_slot && is_const_pointee(ctype, &cdecl) && resolve_c_type(&cdecl, ctype, tm).kind != "str")
        return slot + " is `" + btype + "` (writable in BRAT) where C passes `" + trim_sv(ctype) + "` (const)";
    return slot + ": BRAT has `" + btype + "`, C has `" + trim_sv(ctype) + "`";
}

static bool same_callback_sig(std::string_view brat_ann,
                              std::string_view ctype,
                              const CDecl &cdecl,
                              const std::set<std::string> &struct_ok,
                              const TargetModel &tm,
                              std::string &why_out) {
    std::string bret, cret;
    std::vector<std::string> bparams, cparams;
    if (!parse_brat_callback(brat_ann, bret, bparams)) {
        why_out = "not a BRAT callback type `" + std::string(brat_ann) + "`";
        return false;
    }
    if (!parse_c_callback(ctype, &cdecl, cret, cparams)) {
        why_out = "header parameter `" + trim_sv(ctype) + "` is not a C function pointer";
        return false;
    }
    if (bparams.size() != cparams.size()) {
        why_out = "callback arity: BRAT `" + std::string(brat_ann) + "` takes " +
                  std::to_string(bparams.size()) + " argument(s), C `" + trim_sv(ctype) +
                  "` takes " + std::to_string(cparams.size());
        return false;
    }
    for (size_t i = 0; i < bparams.size(); ++i) {
        if (!same_callback_slot(bparams[i], cparams[i], cdecl, struct_ok, tm)) {
            why_out = callback_slot_why("callback argument " + std::to_string(i + 1), bparams[i], cparams[i], cdecl, tm);
            return false;
        }
    }
    if (!same_callback_slot(bret, cret, cdecl, struct_ok, tm)) {
        why_out = callback_slot_why("callback return", bret, cret, cdecl, tm);
        return false;
    }
    return true;
}

static CheckReport check_declarations(const BratFfiSurface &surf,
                                      const CDecl &cdecl,
                                      std::string_view header_name,
                                      const TargetModel &tm) {
    CheckReport rep;
    std::set<std::string> brat_struct_names;
    for (const auto &[sn, _] : surf.structs) brat_struct_names.insert(sn);
    auto b_layouts = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);
    auto c_layouts = compute_c_struct_layout(cdecl, tm);

    for (const auto &[sname, bfields] : surf.structs) {
        std::string rk_lbl = surf.unions.count(sname) ? "extern union" : "extern struct";
        if (cdecl.bitfields.count(sname)) {
            std::string bf_desc;
            const auto &bfs = cdecl.bitfields.at(sname);
            for (size_t i = 0; i < bfs.size(); ++i) {
                if (i) bf_desc += ", ";
                bf_desc += (bfs[i].name.empty() ? "<pad>" : bfs[i].name) + ":" + bfs[i].width;
            }
            rep.err(rk_lbl + " " + sname + ": " + std::string(header_name) + " declares C bitfield(s) (" + bf_desc +
                    "); bitfields are not byte-addressed and cannot be declared in BRAT");
            continue;
        }
        bool any_b_arr = false;
        for (const auto &[_, bt] : bfields) if (parse_array_field_type(bt)) any_b_arr = true;
        if (cdecl.arrays.count(sname) && !any_b_arr) {
            rep.err(rk_lbl + " " + sname + ": inline array fields in " + std::string(header_name) +
                    " are not representable as scalar/pointer fields");
            continue;
        }
        bool b_un = surf.unions.count(sname) > 0;
        bool c_un = cdecl.unions.count(sname) > 0;
        if (c_un && !b_un) {
            rep.err("extern struct " + sname + ": " + std::string(header_name) +
                    " declares a union, BRAT declares a struct (write `extern union`)");
            continue;
        }
        if (b_un && cdecl.structs.count(sname) && !c_un) {
            rep.err("extern union " + sname + ": BRAT declares a union, " + std::string(header_name) + " declares a struct");
            continue;
        }
        bool b_pk = surf.packed.count(sname) > 0;
        bool c_pk = cdecl.packed.count(sname) > 0;
        if (c_pk && !b_pk) {
            rep.err("extern struct " + sname + ": " + std::string(header_name) +
                    " declares it packed, BRAT does not (write `extern packed struct`)");
            continue;
        }
        if (b_pk && cdecl.structs.count(sname) && !c_pk) {
            rep.err("extern struct " + sname + ": BRAT declares it packed, " + std::string(header_name) + " does not");
            continue;
        }
        if (!cdecl.structs.count(sname)) {
            rep.err("extern struct " + sname + ": not declared in " + std::string(header_name));
            continue;
        }
        const auto &cfields = cdecl.structs.at(sname);
        if (cfields.empty()) {
            rep.err("extern struct " + sname + ": opaque in " + std::string(header_name));
            continue;
        }
        if (bfields.size() != cfields.size()) {
            rep.err("extern struct " + sname + ": BRAT declares " + std::to_string(bfields.size()) +
                    " field(s), " + std::string(header_name) + " has " + std::to_string(cfields.size()));
            continue;
        }
        bool bad = false;
        for (size_t i = 0; i < bfields.size(); ++i) {
            const auto &[bf, bt] = bfields[i];
            const auto &[cf, ct] = cfields[i];
            if (bf != cf) {
                rep.err("extern struct " + sname + ": field " + std::to_string(i + 1) +
                        " is `" + bf + "` in BRAT, `" + cf + "` in " + std::string(header_name));
                bad = true;
                continue;
            }
            auto rc = resolve_c_type(&cdecl, ct, tm);
            auto barr = parse_array_field_type(bt);
            if (rc.kind == "array" || barr.has_value()) {
                if (rc.kind != "array" || !barr.has_value()) {
                    rep.err("extern struct " + sname + "." + bf + ": inline array fields mismatch (`" + bt +
                            "` in BRAT vs `" + trim_sv(ct) + "` in " + std::string(header_name) + ")");
                    bad = true;
                } else if (!same_type_check(bt, ct, cdecl, brat_struct_names, tm)) {
                    rep.err("extern struct " + sname + "." + bf + ": BRAT declares `" + bt +
                            "`, " + std::string(header_name) + " has `" + trim_sv(ct) + "`");
                    bad = true;
                }
                continue;
            }
            if (rc.kind == "fn" || bt.rfind("fn(", 0) == 0) {
                std::string why;
                if (!same_callback_sig(bt, ct, cdecl, brat_struct_names, tm, why)) {
                    rep.err("extern struct " + sname + "." + bf + ": " + why);
                    bad = true;
                }
                continue;
            }
            if (rc.kind == "ptr" && bt == "ptr" && is_fixed_scalar(rc.extra)) {
                rep.err("extern struct " + sname + "." + bf + ": erased `ptr` is incompatible with scalar `" +
                        rc.extra + " *`; use `ptr[T]`");
                bad = true;
            } else if (!same_type_check(bt, ct, cdecl, brat_struct_names, tm)) {
                rep.err("extern struct " + sname + "." + bf + ": BRAT declares `" + bt + "` (" +
                        size_note_str(bt, tm) + "), " + std::string(header_name) + " has `" + trim_sv(ct) + "`");
                bad = true;
            }
        }
        if (!bad && b_layouts.count(sname) && c_layouts.count(sname)) {
            const auto &bl = b_layouts.at(sname);
            const auto &cl = c_layouts.at(sname);
            int pc = cdecl.pack_align.count(sname) ? cdecl.pack_align.at(sname) : 0;
            std::string pnote = (pc > 1) ? (" (#pragma pack(" + std::to_string(pc) + "))") : "";
            std::map<std::string, int64_t> coff_map(cl.slots.begin(), cl.slots.end());
            for (const auto &[fnm, boff] : bl.slots) {
                if (coff_map.count(fnm) && coff_map[fnm] != boff) {
                    rep.err("extern struct " + sname + "." + fnm + ": offset is " + std::to_string(boff) +
                            " in BRAT, " + std::to_string(coff_map[fnm]) + " in " + std::string(header_name) + pnote);
                    bad = true;
                }
            }
            if (!bad && (bl.size != cl.size || bl.align != cl.align)) {
                rep.err("extern struct " + sname + ": layout (" + std::to_string(bl.size) + "B, align " +
                        std::to_string(bl.align) + ") differs from " + std::string(header_name) + " (" +
                        std::to_string(cl.size) + "B, align " + std::to_string(cl.align) + ")" + pnote);
                bad = true;
            }
        }
        if (!bad) ++rep.proven;
    }

    std::map<std::string, bool> rep_memo;
    std::set<std::string> rep_visiting;
    static const std::set<std::string_view> kRepKw = {
        "fn", "let", "mut", "ref", "move", "if", "else", "while", "for", "in", "return",
        "match", "struct", "enum", "impl", "trait", "import", "pub", "use", "const",
        "extern", "export", "packed", "spawn", "scope", "arena", "unsafe", "defer",
        "try", "catch", "break", "continue", "par", "as", "true", "false", "none",
        "and", "or", "not", "is",
    };
    static const std::set<std::string_view> kRepScalars = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "bool", "str", "ptr"
    };
    static const std::set<std::string_view> kRepPrim = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "bool"
    };
    auto rep_ident_ok = [&](std::string_view nm) -> bool {
        if (nm.empty() || !(std::isalpha(static_cast<unsigned char>(nm[0])) || nm[0] == '_')) return false;
        for (char ch : nm)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return false;
        return !kRepKw.count(nm);
    };
    std::function<bool(const std::string &)> c_struct_representable = [&](const std::string &sn) -> bool {
        if (auto it = rep_memo.find(sn); it != rep_memo.end()) return it->second;
        if (rep_visiting.count(sn)) return false;
        if (sn.empty() || !rep_ident_ok(sn) || !cdecl.structs.count(sn) || cdecl.structs.at(sn).empty())
            return rep_memo[sn] = false;
        if (cdecl.anon.count(sn) || cdecl.bitfields.count(sn))
            return rep_memo[sn] = false;
        if (cdecl.arrays.count(sn) && !brat_struct_names.count(sn))
            return rep_memo[sn] = false;
        if (cdecl.pack_align.count(sn) && cdecl.pack_align.at(sn) > 1 && !cdecl.unions.count(sn)) {
            if (!c_layouts.count(sn)) return rep_memo[sn] = false;
            CDecl nat_cd = cdecl;
            nat_cd.pack_align.erase(sn);
            auto nat_lays = compute_c_struct_layout(nat_cd, tm);
            if (nat_lays.count(sn)) {
                const auto &cl = c_layouts.at(sn);
                const auto &nl = nat_lays.at(sn);
                if (cl.size != nl.size || cl.align != nl.align || cl.slots != nl.slots)
                    return rep_memo[sn] = false;
            }
        }
        rep_visiting.insert(sn);
        auto cb_slot_ok = [&](std::string_view slot_ct, bool allow_void) -> bool {
            auto sr = resolve_c_type(&cdecl, slot_ct, tm);
            if (sr.kind == "none") return allow_void;
            if (sr.kind == "struct") return c_struct_representable(sr.extra);
            if (sr.kind == "ptr") {
                if (is_const_pointee(slot_ct, &cdecl)) return false;
                if (sr.opaque) return true;
                if (!kRepPrim.count(sr.extra)) return false;
                if ((sr.extra == "u8" || sr.extra == "i8") && spelled_plain_char(slot_ct)) return false;
                return true;
            }
            return kRepScalars.count(sr.kind) > 0;
        };
        bool fields_ok = true;
        for (const auto &[fnm, fct] : cdecl.structs.at(sn)) {
            if (!rep_ident_ok(fnm)) { fields_ok = false; break; }
            auto fr = resolve_c_type(&cdecl, fct, tm);
            if (fr.kind == "struct") {
                if (fr.extra == sn || !c_struct_representable(fr.extra)) { fields_ok = false; break; }
            } else if (fr.kind == "array") {
                if (fr.arr_elem_k == "struct") {
                    if (!c_struct_representable(fr.arr_elem_e)) { fields_ok = false; break; }
                } else if (!kRepScalars.count(fr.arr_elem_k)) {
                    fields_ok = false;
                    break;
                }
            } else if (fr.kind == "fn") {
                std::string cret;
                std::vector<std::string> cparams;
                if (!parse_c_callback(fct, &cdecl, cret, cparams) || !cb_slot_ok(cret, true)) {
                    fields_ok = false;
                    break;
                }
                for (const auto &cp : cparams) {
                    if (!cb_slot_ok(cp, false)) { fields_ok = false; break; }
                }
                if (!fields_ok) break;
            } else if (!kRepScalars.count(fr.kind)) {
                fields_ok = false;
                break;
            }
        }
        rep_visiting.erase(sn);
        return rep_memo[sn] = fields_ok;
    };

    auto drift_set = platform_width_uses(cdecl);

    for (const auto &d : surf.fns) {
        bool is_plugin = false;
        for (const auto &eff : d.effects) if (eff == "__plugin__") is_plugin = true;

        if (!cdecl.fns.count(d.name)) {
            rep.err("extern fn " + d.name + ": not declared in " + std::string(header_name) + "; the ABI is unproven");
            continue;
        }
        const auto &cfn = cdecl.fns.at(d.name);
        bool fn_bad = false;
        if (cdecl.abi_unsupported.count(d.name)) {
            rep.err("extern fn " + d.name + ": unsupported ABI calling convention `" +
                    cdecl.abi_unsupported.at(d.name) + "` in " + std::string(header_name));
            fn_bad = true;
        }
        if (d.params.size() != cfn.params.size()) {
            rep.err("extern fn " + d.name + ": BRAT takes " + std::to_string(d.params.size()) +
                    " argument(s), " + std::string(header_name) + " declares " + std::to_string(cfn.params.size()));
            fn_bad = true;
        }
        size_t nmin = std::min(d.params.size(), cfn.params.size());
        for (size_t i = 0; i < nmin; ++i) {
            std::string bt = d.params[i].ann.value_or("i64");
            const std::string &ct = cfn.params[i];
            std::string where = "parameter " + std::to_string(i + 1) + " `" + d.params[i].name + "`";
            std::string c_cb_r;
            std::vector<std::string> c_cb_p;
            if (bt.rfind("fn(", 0) == 0 || parse_c_callback(ct, &cdecl, c_cb_r, c_cb_p)) {
                if (is_plugin && bt == "ptr") continue;
                std::string why;
                if (!same_callback_sig(bt, ct, cdecl, brat_struct_names, tm, why)) {
                    rep.err("extern fn " + d.name + ": " + where + " " + why);
                    fn_bad = true;
                }
                continue;
            }
            auto rc = resolve_c_type(&cdecl, ct, tm);
            if (rc.kind == "struct") {
                if (bt != rc.extra || !brat_struct_names.count(bt) ||
                    !cdecl.structs.count(rc.extra) || cdecl.structs.at(rc.extra).empty()) {
                    rep.err("extern fn " + d.name + ": " + where + " is declared `" + bt +
                            "`, " + std::string(header_name) + " has `" + rc.extra + "`");
                    fn_bad = true;
                }
            } else if (rc.kind == "ptr" || rc.kind == "str") {
                std::string elem = element_of_ptr(bt);
                std::string base = !elem.empty() ? "ptr" : bt;
                bool is_mut_mode = (d.params[i].mode == Mode::Mut);
                for (const auto &eff : d.effects)
                    if (eff == "writes:" + d.params[i].name) is_mut_mode = true;

                if (rc.kind == "ptr" && base == "str" && !is_const_pointee(ct, &cdecl)) {
                    rep.err("extern fn " + d.name + ": " + where + " is declared `str`, which is read-only, but " +
                            std::string(header_name) + " has `" + trim_sv(ct) + "`: declare a `ptr` for a buffer C may write");
                    fn_bad = true;
                } else if (is_mut_mode && is_const_pointee(ct, &cdecl)) {
                    rep.err("extern fn " + d.name + ": " + where + " is mutable/`out ptr`, which writes through the pointer, but " +
                            std::string(header_name) + " has `" + trim_sv(ct) + "`");
                    fn_bad = true;
                } else if (rc.kind == "str" && base == "ptr" && (elem == "u8" || elem == "i8")) {
                    // Nullable read-only `const char *` parameter spelled `ptr[u8]` or `ptr[i8]`.
                } else if (base != rc.kind && !(base == "str" && rc.kind == "ptr")) {
                    rep.err("extern fn " + d.name + ": " + where + " is declared `" + bt +
                            "`, " + std::string(header_name) + " has `" + trim_sv(ct) + "`");
                    fn_bad = true;
                } else if (!elem.empty() && !pointee_ok(elem, rc.extra, brat_struct_names, tm)) {
                    rep.err("extern fn " + d.name + ": " + where + " is declared `" + bt +
                            "`, " + std::string(header_name) + " has `" + trim_sv(ct) + "`");
                    fn_bad = true;
                } else if (!is_plugin && rc.kind == "ptr" && elem.empty() && is_fixed_scalar(rc.extra)) {
                    rep.err("extern fn " + d.name + ": " + where + " uses erased `ptr`, incompatible with scalar `" +
                            rc.extra + " *`; use `ptr[T]`");
                    fn_bad = true;
                } else if (!is_plugin && rc.kind == "ptr" && elem.empty() && c_struct_representable(rc.extra)) {
                    rep.err("extern fn " + d.name + ": " + where + " uses erased `ptr` for `" + rc.extra +
                            " *`; use `ptr[" + rc.extra + "]`");
                    fn_bad = true;
                }
            } else if (rc.kind == "none") {
                rep.err("extern fn " + d.name + ": " + where + " is `void`, which is not an argument");
                fn_bad = true;
            } else if (!same_brat_ffi_scalar(bt, rc.kind, tm)) {
                rep.err("extern fn " + d.name + ": " + where + " is declared `" + bt + "` (" + size_note_str(bt, tm) +
                        "), " + std::string(header_name) + " has `" + rc.kind + "` (" + size_note_str(rc.kind, tm) + ")");
                fn_bad = true;
            }
        }
        bool bvar = d.varargs.has_value();
        if (bvar && !cfn.variadic) {
            rep.err("extern fn " + d.name + ": BRAT declares a variadic tail, " + std::string(header_name) + " has none");
            fn_bad = true;
        }
        if (cfn.variadic && !bvar) {
            rep.err("extern fn " + d.name + ": " + std::string(header_name) + " is variadic, BRAT declares fixed arguments");
            fn_bad = true;
        }

        std::string bret = d.ret.value_or("none");
        std::string ret_cb_r;
        std::vector<std::string> ret_cb_p;
        if (bret.rfind("fn(", 0) == 0 || parse_c_callback(cfn.ret, &cdecl, ret_cb_r, ret_cb_p)) {
            if (!(is_plugin && bret == "ptr")) {
                std::string why;
                if (!same_callback_sig(bret, cfn.ret, cdecl, brat_struct_names, tm, why)) {
                    rep.err("extern fn " + d.name + ": return value " + why);
                    fn_bad = true;
                }
            }
        } else {
            auto rrc = resolve_c_type(&cdecl, cfn.ret, tm);
            if (bret == "none") {
                if (rrc.kind != "none") {
                    rep.err("extern fn " + d.name + ": BRAT declares no result, " + std::string(header_name) + " returns `" + trim_sv(cfn.ret) + "`");
                    fn_bad = true;
                }
            } else if (rrc.kind == "none") {
                rep.err("extern fn " + d.name + ": " + std::string(header_name) + " returns `void`, BRAT declares `" + bret + "`");
                fn_bad = true;
            } else if (rrc.kind == "struct") {
                if (bret != rrc.extra || !brat_struct_names.count(bret) ||
                    !cdecl.structs.count(rrc.extra) || cdecl.structs.at(rrc.extra).empty()) {
                    rep.err("extern fn " + d.name + ": BRAT returns `" + bret + "`, " + std::string(header_name) + " returns `" + rrc.extra + "`");
                    fn_bad = true;
                }
            } else if (rrc.kind == "ptr" || rrc.kind == "str") {
                std::string relem = element_of_ptr(bret);
                const bool ptr_word_int = (tm.ptr == 4)
                    ? (bret == "i32" || bret == "u32" || bret == "usize")
                    : (bret == "i64" || bret == "u64" || bret == "usize");
                if (rrc.kind == "str" && bret != "str" && bret != "ptr" && !ptr_word_int && relem != "u8" && relem != "i8") {
                    rep.err("extern fn " + d.name + ": BRAT returns `" + bret + "`, " + std::string(header_name) + " returns `" + trim_sv(cfn.ret) + "`");
                    fn_bad = true;
                } else if (!is_plugin && rrc.kind == "ptr" && bret == "ptr" && is_fixed_scalar(rrc.extra)) {
                    rep.err("extern fn " + d.name + ": BRAT returns erased `ptr`, incompatible with scalar `" + rrc.extra + " *`; use `ptr[T]`");
                    fn_bad = true;
                } else if (!is_plugin && rrc.kind == "ptr" && bret == "ptr" && c_struct_representable(rrc.extra)) {
                    rep.err("extern fn " + d.name + ": BRAT returns erased `ptr` for `" + rrc.extra + " *`; use `ptr[" + rrc.extra + "]`");
                    fn_bad = true;
                } else if (rrc.kind == "ptr" && relem.empty() && bret != "ptr" && !ptr_word_int) {
                    rep.err("extern fn " + d.name + ": BRAT returns `" + bret + "`, " + std::string(header_name) + " returns `" + trim_sv(cfn.ret) + "`");
                    fn_bad = true;
                } else if (rrc.kind == "ptr" && !relem.empty() && !pointee_ok(relem, rrc.extra, brat_struct_names, tm)) {
                    rep.err("extern fn " + d.name + ": BRAT returns `" + bret + "`, " + std::string(header_name) + " returns `" + trim_sv(cfn.ret) + "`");
                    fn_bad = true;
                }
            } else if (!same_brat_ffi_scalar(bret, rrc.kind, tm)) {
                rep.err("extern fn " + d.name + ": BRAT returns `" + bret + "` (" + size_note_str(bret, tm) +
                        "), " + std::string(header_name) + " returns `" +
                        (rrc.kind == "typedef" ? trim_sv(cfn.ret) : rrc.kind) + "` (" + size_note_str(rrc.kind, tm) + ")");
                fn_bad = true;
            }
        }

        if (!fn_bad) {
            ++rep.proven;
            if (drift_set.count("fn " + d.name))
                rep.notes.push_back("fn " + d.name + " uses platform-dependent width (`long`/`size_t`) in " + std::string(header_name));
        }
    }
    for (const auto &sn : brat_struct_names) {
        if (drift_set.count("struct " + sn))
            rep.notes.push_back("struct " + sn + " uses platform-dependent width (`long`/`size_t`) in " + std::string(header_name));
    }
    return rep;
}

// Convert a BRAT FFI type to its C probe spelling so `cc -fsyntax-only` checks exact pointer/struct/layout compatibility.
static std::optional<std::string> brat_to_c_probe_type(std::string_view bt,
                                                       const CDecl &cdecl,
                                                       const std::set<std::string> &known_structs) {
    if (bt == "none") return "void";
    if (bt == "bool") return "_Bool";
    if (bt == "i8") return "int8_t";
    if (bt == "u8") return "uint8_t";
    if (bt == "i16") return "int16_t";
    if (bt == "u16") return "uint16_t";
    if (bt == "i32") return "int32_t";
    if (bt == "u32") return "uint32_t";
    if (bt == "i64") return "int64_t";
    if (bt == "u64") return "uint64_t";
    if (bt == "usize") return "size_t";
    if (bt == "f32") return "float";
    if (bt == "f64") return "double";
    if (bt == "str") return "const char *";
    if (bt == "ptr") return "void *";
    if (known_structs.count(std::string(bt))) return cdecl.c_name(std::string(bt));
    return std::nullopt;
}

static bool write_temp_probe_c(const char *prefix, const std::string &body, std::string &path_out) {
#ifdef _WIN32
    static std::atomic<unsigned long> seq{0};
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) return false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        fs::path cand = base / (std::string(prefix) + std::to_string(GetCurrentProcessId()) +
                                "-" + std::to_string(++seq) + ".c");
        HANDLE h = CreateFileW(cand.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        size_t off = 0;
        bool ok = true;
        while (off < body.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(body.size() - off, 65536));
            DWORD wrote = 0;
            if (!WriteFile(h, body.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
                ok = false;
                break;
            }
            off += wrote;
        }
        CloseHandle(h);
        if (!ok) {
            fs::remove(cand, ec);
            return false;
        }
        path_out = cand.string();
        return true;
    }
    return false;
#else
    std::string tpl = (fs::temp_directory_path() / (std::string(prefix) + "XXXXXX.c")).string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), 2);
    if (fd < 0) return false;
    path_out.assign(buf.data());
    size_t off = 0;
    bool ok = true;
    while (off < body.size()) {
        ssize_t w = write(fd, body.data() + off, body.size() - off);
        if (w <= 0) { ok = false; break; }
        off += static_cast<size_t>(w);
    }
    close(fd);
    if (!ok) {
        std::remove(path_out.c_str());
        path_out.clear();
        return false;
    }
    return true;
#endif
}

[[maybe_unused]] static bool run_cmd_capture(const std::vector<std::string> &argv, int &rc_out, std::string &combined_out) {
    combined_out.clear();
    if (argv.empty()) return false;
    pkg_io::ProcOptions opts;
    opts.timeout_ms = pkg_io::cc_timeout_ms();
    opts.search_path = false;
    opts.capture_output = true;
    opts.merge_stderr = true;
    opts.null_stdin = true;
    opts.max_out_total = 4u * 1024u * 1024u;
    pkg_io::ProcResult res = pkg_io::run_proc(argv, opts);
    if (!res.launched) return false;
    if (res.timed_out) {
        combined_out = "C compiler probe timed out";
        rc_out = 124;
        return true;
    }
    combined_out = std::move(res.out);
    rc_out = res.over_limit ? 1 : res.exit_code;
    return true;
}

struct CompilerProbeResult {
    bool checked = false;
    bool ok = true;
    std::string output;
};

static CompilerProbeResult compile_probe(const BratFfiSurface &surf,
                                         const CDecl &cdecl,
                                         const std::string &header_path,
                                         const std::vector<std::string> &include_dirs,
                                         const std::vector<std::string> &defines,
                                         const TargetModel &tm) {
    CompilerProbeResult res;
    if (header_path.empty()) return res;

    std::vector<std::string> cc_argv;
    if (!tm.triple.empty()) {
        std::string zig = find_bin_in_path("zig");
        if (!zig.empty()) cc_argv = {zig, "cc", "-target", tm.triple};
        else {
            std::string clang = find_bin_in_path("clang");
            if (!clang.empty()) cc_argv = {clang, "--target=" + tm.triple};
            else return res;  // cross-target without zig/clang: algebraic check only
        }
    } else {
        for (const char *cand : {"cc", "clang", "gcc"}) {
            std::string p = find_bin_in_path(cand);
            if (!p.empty()) { cc_argv = {p}; break; }
        }
        if (cc_argv.empty()) return res;
    }

    auto b_layouts = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);
    std::set<std::string> known_structs;
    for (const auto &[sn, _] : surf.structs) {
        if (!cdecl.inline_structs.count(sn)) known_structs.insert(sn);
    }

    fs::path abs_hdr = fs::absolute(fs::path(header_path));
    std::string hdr_sp = abs_hdr.string();
    for (char c : hdr_sp) {
        if (c == '\r' || c == '\n') {
            res.checked = true;
            res.ok = false;
            res.output = "invalid newline in header path";
            return res;
        }
    }
    std::string esc_hdr;
    esc_hdr.reserve(hdr_sp.size());
    for (char c : hdr_sp) {
        if (c == '\\' || c == '"') esc_hdr.push_back('\\');
        esc_hdr.push_back(c);
    }
    std::ostringstream src;
    src << "#include <stddef.h>\n#include <stdint.h>\n#include <stdbool.h>\n";
    src << "#include \"" << esc_hdr << "\"\n\n";

    auto emit_leaf_offsets = [&](auto &self,
                                 const std::string &cn,
                                 const std::string &rec,
                                 const std::string &c_prefix,
                                 int64_t base_off) -> void {
        if (!b_layouts.count(rec) || !surf.structs.count(rec)) return;
        const auto &bl = b_layouts.at(rec);
        const auto &bfields = surf.structs.at(rec);
        std::map<std::string, std::string> ftypes(bfields.begin(), bfields.end());
        for (const auto &[fnm, foff] : bl.slots) {
            std::string ft = ftypes[fnm];
            auto ait = cdecl.anon_members.find({rec, fnm});
            if (ait != cdecl.anon_members.end() && surf.structs.count(ait->second)) {
                self(self, cn, ait->second, c_prefix, base_off + foff);
            } else if (cdecl.inline_structs.count(ft) && surf.structs.count(ft)) {
                std::string next_cp = c_prefix.empty() ? fnm : (c_prefix + "." + fnm);
                self(self, cn, ft, next_cp, base_off + foff);
            } else {
                std::string cp = c_prefix.empty() ? fnm : (c_prefix + "." + fnm);
                src << "_Static_assert(offsetof(" << cn << ", " << cp << ") == " << (base_off + foff)
                    << ", \"offsetof " << cn << "." << cp << "\");\n";
            }
        }
    };

    for (const auto &[sn, _] : surf.structs) {
        if (!b_layouts.count(sn) || !cdecl.structs.count(sn)) continue;
        if (cdecl.inline_structs.count(sn)) continue;
        const auto &bl = b_layouts.at(sn);
        std::string cn = cdecl.c_name(sn);
        src << "_Static_assert(sizeof(" << cn << ") == " << bl.size << ", \"sizeof " << sn << "\");\n";
        src << "_Static_assert(_Alignof(" << cn << ") == " << bl.align << ", \"alignof " << sn << "\");\n";
        emit_leaf_offsets(emit_leaf_offsets, cn, sn, "", 0);
    }

    int probe_idx = 0;
    for (const auto &d : surf.fns) {
        if (!cdecl.fns.count(d.name)) continue;
        const auto &cfn = cdecl.fns.at(d.name);
        // Only emit function-pointer assignment when none of the parameters are `ptr` wildcarding a typed C pointer
        bool exact_fnptr = true;
        auto r_sp = brat_to_c_probe_type(d.ret.value_or("none"), cdecl, known_structs);
        if (!r_sp || d.ret.value_or("none") == "ptr") {
            auto cr = resolve_c_type(&cdecl, cfn.ret, tm);
            if (d.ret.value_or("none") == "ptr" && (cr.kind != "ptr" || !cr.extra.empty())) exact_fnptr = false;
        }
        if (!r_sp) exact_fnptr = false;
        std::vector<std::string> p_sps;
        for (size_t i = 0; i < d.params.size() && exact_fnptr; ++i) {
            std::string bt = d.params[i].ann.value_or("i64");
            auto ps = brat_to_c_probe_type(bt, cdecl, known_structs);
            if (!ps) { exact_fnptr = false; break; }
            if (bt == "ptr" && i < cfn.params.size()) {
                auto cp_r = resolve_c_type(&cdecl, cfn.params[i], tm);
                if (cp_r.kind != "ptr" || !cp_r.extra.empty() || is_const_pointee(cfn.params[i], &cdecl))
                    exact_fnptr = false;
            }
            p_sps.push_back(*ps);
        }
        if (exact_fnptr) {
            std::string sig_params;
            if (p_sps.empty() && !d.varargs) sig_params = "void";
            else {
                for (size_t i = 0; i < p_sps.size(); ++i) {
                    if (i) sig_params += ", ";
                    sig_params += p_sps[i];
                }
                if (d.varargs) sig_params += (p_sps.empty() ? "..." : ", ...");
            }
            src << "static " << *r_sp << " (*__brat_probe_" << (probe_idx++) << ")(" << sig_params
                << ") = (" << *r_sp << " (*)(" << sig_params << "))&" << d.name << ";\n";
        } else {
            // Still verify symbol exists as an addressable function/entity in the header
            src << "static const void *__brat_sym_" << (probe_idx++) << " = (const void *)&" << d.name << ";\n";
        }
    }

    std::string tmp_c;
    if (!write_temp_probe_c("brat-ffi-probe-", src.str(), tmp_c)) return res;
    cc_argv.insert(cc_argv.end(), {"-std=gnu11", "-fsyntax-only", "-Werror=incompatible-pointer-types", "-Wno-unused-variable"});
    cc_argv.push_back("-I");
    cc_argv.push_back(abs_hdr.parent_path().string());
    for (const auto &inc : include_dirs) { cc_argv.push_back("-I"); cc_argv.push_back(inc); }
    for (const auto &def : defines) cc_argv.push_back("-D" + def);
    cc_argv.push_back(tmp_c);

    int rc = 0;
    std::string out_txt;
    if (run_cmd_capture(cc_argv, rc, out_txt)) {
        res.checked = true;
        res.ok = (rc == 0);
        for (;;) {
            size_t p = out_txt.find(tmp_c);
            if (p == std::string::npos) break;
            out_txt.replace(p, tmp_c.size(), "ffi-probe.c");
        }
        res.output = trim_sv(out_txt);
    }
    std::error_code rm_ec;
    fs::remove(tmp_c, rm_ec);
    return res;
}

// Binary object (`.o`, `.so`, `.a`, `.dylib`, `.dll`) symbol and DWARF ABI verifier.
static DwarfSlot brat_to_expected_dwarf(std::string_view bt,
                                        const std::map<std::string, RecordLayout> &b_layouts,
                                        const std::set<std::string> &b_unions,
                                        const TargetModel &tm) {
    DwarfSlot s;
    if (bt == "none") { s.category = "void"; s.byte_width = 0; return s; }
    if (bt == "f32") { s.category = "float"; s.byte_width = 4; return s; }
    if (bt == "f64") { s.category = "float"; s.byte_width = 8; return s; }
    if (bt == "ptr" || bt == "str" || bt.rfind("ptr[", 0) == 0) {
        s.category = "pointer";
        s.byte_width = tm.ptr;
        s.name = (bt == "ptr") ? "ptr" : std::string(bt);
        return s;
    }
    if (bt.rfind("fn(", 0) == 0) {
        s.category = "fn";
        s.byte_width = tm.ptr;
        std::string bret;
        std::vector<std::string> bps;
        if (parse_brat_callback(bt, bret, bps)) {
            auto f = std::make_shared<DwarfFnSig>();
            f->ret = std::make_shared<DwarfSlot>(brat_to_expected_dwarf(bret, b_layouts, b_unions, tm));
            for (const auto &p : bps) f->params.push_back(brat_to_expected_dwarf(p, b_layouts, b_unions, tm));
            s.fn = std::move(f);
        }
        return s;
    }
    if (auto arr = parse_array_field_type(bt)) {
        auto sub = brat_to_expected_dwarf(arr->first, b_layouts, b_unions, tm);
        s.category = "array";
        if (sub.byte_width) s.byte_width = *sub.byte_width * arr->second;
        return s;
    }
    if (auto w = brat_abi_width(bt, tm)) {
        s.category = "scalar";
        s.byte_width = *w;
        return s;
    }
    if (b_layouts.count(std::string(bt))) {
        s.category = b_unions.count(std::string(bt)) ? "union" : "struct";
        s.byte_width = b_layouts.at(std::string(bt)).size;
        return s;
    }
    s.category = "unknown";
    return s;
}

static bool dwarf_slot_matches(const DwarfSlot &want, const DwarfSlot &got, std::string &why) {
    if (want.category == "unknown" || got.category == "unknown") return true;
    // Only erased `ptr` accepts an opaque or variadic C function pointer; typed `ptr[T]` and `str` do not.
    if (want.category == "pointer" && got.category == "fn" && want.name == "ptr") return true;
    if (want.category == "fn" && got.category == "fn") {
        if (want.fn && got.fn) {
            if (want.fn->variadic != got.fn->variadic) {
                why = std::string("callback variadic mismatch: BRAT is ") +
                      (want.fn->variadic ? "variadic" : "fixed-arity") + ", DWARF is " +
                      (got.fn->variadic ? "variadic" : "fixed-arity");
                return false;
            }
            if (want.fn->params.size() != got.fn->params.size()) {
                why = "callback arity: BRAT has " + std::to_string(want.fn->params.size()) +
                      ", DWARF has " + std::to_string(got.fn->params.size());
                return false;
            }
            for (size_t i = 0; i < want.fn->params.size(); ++i) {
                if (!dwarf_slot_matches(want.fn->params[i], got.fn->params[i], why)) return false;
            }
            if (want.fn->ret && got.fn->ret && !dwarf_slot_matches(*want.fn->ret, *got.fn->ret, why))
                return false;
        }
        return true;
    }
    if (want.category != got.category) {
        why = "ABI class mismatch: BRAT expects " + want.category + ", binary DWARF has " + got.category;
        return false;
    }
    if (want.byte_width && got.byte_width && *want.byte_width != *got.byte_width) {
        why = "byte width mismatch: BRAT expects " + std::to_string(*want.byte_width) +
              "B, binary DWARF has " + std::to_string(*got.byte_width) + "B";
        return false;
    }
    return true;
}

static CheckReport verify_object_against_surface(const BratFfiSurface &surf,
                                                 const std::string &obj_path,
                                                 const TargetModel &tm) {
    CheckReport rep;
    ObjectInfo info;
    std::string ierr;
    if (!inspect_object_file(obj_path, info, ierr)) {
        rep.fail(fs::path(obj_path).filename().string(), ierr);
        return rep;
    }
    std::string obj_label = fs::path(obj_path).filename().string();
    if (info.ptr_width != 0 && info.ptr_width != tm.ptr) {
        rep.fail(obj_label, "pointer width mismatch: target is " + std::to_string(tm.ptr * 8) +
                 "-bit, object is " + std::to_string(info.ptr_width * 8) + "-bit (" + info.arch + ")");
        return rep;
    }

    auto b_layouts = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);

    for (const auto &[sname, bfields] : surf.structs) {
        auto dit = info.dwarf_structs.find(sname);
        if (dit == info.dwarf_structs.end()) continue;  // stripped or unused struct in TU
        const auto &ds = dit->second;
        bool b_un = surf.unions.count(sname) > 0;
        if (b_un != ds.is_union) {
            rep.fail((b_un ? "union " : "struct ") + sname,
                     std::string("BRAT declares `") + (b_un ? "extern union" : "extern struct") +
                     "`, DWARF in " + obj_label + " has `" + (ds.is_union ? "union" : "struct") + "`");
            continue;
        }
        if (b_layouts.count(sname)) {
            const auto &bl = b_layouts.at(sname);
            if (bl.size != ds.size) {
                rep.fail((b_un ? "union " : "struct ") + sname,
                         "size mismatch: BRAT has " + std::to_string(bl.size) +
                         "B, DWARF in " + obj_label + " has " + std::to_string(ds.size) + "B");
                continue;
            }
            if (ds.members.size() != bfields.size()) {
                rep.fail((b_un ? "union " : "struct ") + sname,
                         "field count mismatch: BRAT has " + std::to_string(bfields.size()) +
                         ", DWARF in " + obj_label + " has " + std::to_string(ds.members.size()));
                continue;
            }
            bool bad = false;
            for (size_t i = 0; i < bfields.size(); ++i) {
                if (bfields[i].first != ds.members[i].name) {
                    rep.fail(sname, "field " + std::to_string(i + 1) + " is `" + bfields[i].first +
                             "` in BRAT, `" + ds.members[i].name + "` in DWARF");
                    bad = true;
                    break;
                }
                if (bl.slots[i].second != ds.members[i].offset) {
                    rep.fail(sname + "." + bfields[i].first,
                             "offset mismatch: BRAT has " + std::to_string(bl.slots[i].second) +
                             ", DWARF has " + std::to_string(ds.members[i].offset));
                    bad = true;
                    break;
                }
                auto want_s = brat_to_expected_dwarf(bfields[i].second, b_layouts, surf.unions, tm);
                std::string why;
                if (!dwarf_slot_matches(want_s, ds.members[i].slot, why)) {
                    rep.fail(sname + "." + bfields[i].first, why);
                    bad = true;
                    break;
                }
            }
            if (!bad) ++rep.proven;
        }
    }

    for (const auto &d : surf.fns) {
        auto sit = info.symbols.find(d.name);
        if (sit == info.symbols.end() || !sit->second.defined || !sit->second.exported) {
            // Check for C++ Itanium/MSVC mangled symbol hint (`_Z...<len><name>...` or `?<name>@...`)
            std::string mangled_hint;
            std::string needle = std::to_string(d.name.size()) + d.name;
            for (const auto &[sym_nm, sym_v] : info.symbols) {
                if (!sym_v.defined || !sym_v.exported) continue;
                if ((sym_nm.rfind("_Z", 0) == 0 && sym_nm.find(needle) != std::string::npos) ||
                    (sym_nm.rfind("?" + d.name + "@", 0) == 0)) {
                    mangled_hint = sym_nm;
                    break;
                }
            }
            if (!mangled_hint.empty()) {
                rep.fail(d.name, "missing unmangled C symbol in " + obj_label +
                         " (found C++ mangled `" + mangled_hint + "` -- add `extern \"C\"`)");
            } else {
                rep.fail(d.name, "symbol not exported by " + obj_label);
            }
            continue;
        }
        if (sit->second.kind == "object") {
            rep.fail(d.name, "symbol in " + obj_label + " is data (`STT_OBJECT`), not a function");
            continue;
        }
        auto fit = info.dwarf_fns.find(d.name);
        if (fit == info.dwarf_fns.end()) {
            ++rep.proven;  // symbol verified; no DWARF debug info present
            continue;
        }
        const auto &dfn = fit->second;
        bool bvar = d.varargs.has_value();
        if (bvar != dfn.variadic) {
            rep.fail(d.name, std::string("variadic mismatch against DWARF in ") + obj_label);
            continue;
        }
        if (d.params.size() != dfn.params.size()) {
            rep.fail(d.name, "arity mismatch: BRAT has " + std::to_string(d.params.size()) +
                     " parameter(s), DWARF in " + obj_label + " has " + std::to_string(dfn.params.size()));
            continue;
        }
        bool bad = false;
        for (size_t i = 0; i < d.params.size(); ++i) {
            auto want_s = brat_to_expected_dwarf(d.params[i].ann.value_or("i64"), b_layouts, surf.unions, tm);
            std::string why;
            if (!dwarf_slot_matches(want_s, dfn.params[i], why)) {
                rep.fail(d.name, "parameter " + std::to_string(i + 1) + " (`" + d.params[i].name + "`): " + why);
                bad = true;
                break;
            }
        }
        if (!bad && dfn.ret) {
            auto want_r = brat_to_expected_dwarf(d.ret.value_or("none"), b_layouts, surf.unions, tm);
            std::string why;
            if (!dwarf_slot_matches(want_r, *dfn.ret, why)) {
                rep.fail(d.name, "return value: " + why);
                bad = true;
            }
        }
        if (!bad) ++rep.proven;
    }
    return rep;
}

// Check exported symbols in `extern "<lib>"` shared libraries via dlopen/dlsym (host) or `inspect_object_file`.
struct LibSymbolStatus {
    std::vector<std::string> libraries;
    std::vector<std::pair<std::string, std::string>> missing;  // (lib, sym_or_reason)
};

static LibSymbolStatus check_library_symbols(const BratFfiSurface &surf, const std::string &base_dir) {
    LibSymbolStatus st;
    std::map<std::string, std::vector<std::string>> by_lib;
    for (const auto &d : surf.fns) {
        if (d.lib && !d.lib->empty()) by_lib[*d.lib].push_back(d.name);
    }
    for (const auto &[lib_raw, syms] : by_lib) {
        st.libraries.push_back(lib_raw);
        std::string resolved = lib_raw;
        std::error_code ec;
        if (!fs::path(lib_raw).is_absolute() && !base_dir.empty()) {
            fs::path cand = fs::path(base_dir) / lib_raw;
            if (fs::is_regular_file(cand, ec)) resolved = cand.string();
        }
        // Never call `dlopen`/`LoadLibrary` on a file path! Static binary symbol-table
        // inspection verifies `.so`/`.dylib`/`.dll`/`.o`/`.a` without running `.init_array`
        // or `DllMain` constructors from untrusted binaries.
        const bool has_path_sep = (lib_raw.find('/') != std::string::npos || lib_raw.find('\\') != std::string::npos);
        if (fs::is_regular_file(resolved, ec)) {
            auto is_static_obj = [](const std::string &p) {
                std::string lo = p;
                for (char &c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return (lo.size() >= 2 && lo.substr(lo.size() - 2) == ".o") ||
                       (lo.size() >= 2 && lo.substr(lo.size() - 2) == ".a") ||
                       (lo.size() >= 4 && lo.substr(lo.size() - 4) == ".obj") ||
                       (lo.size() >= 4 && lo.substr(lo.size() - 4) == ".lib");
            };
            const bool static_obj = is_static_obj(resolved);
            (void)static_obj;
            ObjectInfo oi;
            std::string oerr;
            if (inspect_object_file(resolved, oi, oerr)) {
                for (const auto &s : syms) {
                    auto it = oi.symbols.find(s);
                    bool found = (it != oi.symbols.end() && it->second.defined && it->second.exported);
#ifndef _WIN32
                    if (!found && !static_obj && oi.format != "ar")
                        found = (dlsym(RTLD_DEFAULT, s.c_str()) != nullptr);
#endif
                    if (!found) st.missing.emplace_back(lib_raw, s);
                }
            } else {
                st.missing.emplace_back(lib_raw, "<cannot open library>");
            }
            continue;
        }
        if (has_path_sep) {
            st.missing.emplace_back(lib_raw, "<cannot open library>");
            continue;
        }
#ifdef _WIN32
        if (resolved == "c" || resolved == "m") {
            HMODULE msvc = GetModuleHandleA("msvcrt.dll");
            if (msvc) {
                for (const auto &s : syms) {
                    if (!GetProcAddress(msvc, s.c_str())) st.missing.emplace_back(lib_raw, s);
                }
            }
            continue;
        }
        if (resolved == "pthread" || resolved == "dl") continue;
        std::string dll_path;
        for (const std::string &cand_name : {resolved + ".dll", "lib" + resolved + ".dll",
                                             "lib" + resolved + "-1.dll", resolved + "1.dll"}) {
            std::string p = find_bin_in_path(cand_name);
            if (!p.empty()) { dll_path = std::move(p); break; }
        }
        if (dll_path.empty()) {
            st.missing.emplace_back(lib_raw, "<cannot open library>");
            continue;
        }
        ObjectInfo dll_oi;
        std::string dll_err;
        if (inspect_object_file(dll_path, dll_oi, dll_err)) {
            for (const auto &s : syms) {
                auto it = dll_oi.symbols.find(s);
                if (it == dll_oi.symbols.end() || !it->second.defined || !it->second.exported)
                    st.missing.emplace_back(lib_raw, s);
            }
        } else {
            st.missing.emplace_back(lib_raw, "<cannot open library>");
        }
#else
        if (resolved == "c" || resolved == "m" || resolved == "pthread" || resolved == "dl") {
            for (const auto &s : syms) {
                if (!dlsym(RTLD_DEFAULT, s.c_str())) st.missing.emplace_back(lib_raw, s);
            }
            continue;
        }
#if defined(__APPLE__)
        std::string so_cand = "lib" + resolved + ".dylib";
#else
        std::string so_cand = "lib" + resolved + ".so";
#endif
        void *h = dlopen(so_cand.c_str(), RTLD_LAZY | RTLD_LOCAL);
#if defined(__APPLE__)
        if (!h) {
            std::string so_alt = "lib" + resolved + ".so";
            h = dlopen(so_alt.c_str(), RTLD_LAZY | RTLD_LOCAL);
        }
#endif
        if (!h) h = dlopen(resolved.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!h) {
            st.missing.emplace_back(lib_raw, "<cannot open library>");
            continue;
        }
        for (const auto &s : syms) {
            dlerror();
            void *p = dlsym(h, s.c_str());
            const char *de = dlerror();
            if (!p && de != nullptr) st.missing.emplace_back(lib_raw, s);
        }
        dlclose(h);
#endif
    }
    return st;
}

// Canonical `BRATFFI1` contract representation + drift comparison.
struct ContractFn {
    std::string name;
    std::optional<std::string> lib;
    std::vector<std::string> params;
    std::string ret;
    bool variadic = false;
    bool owned = false;
    bool checked = false;
    std::vector<std::string> effects;
    std::vector<std::string> contracts;
};

struct ContractStruct {
    std::string name;
    bool packed = false;
    bool is_union = false;
    int64_t size = 0;
    int64_t align = 1;
    std::vector<std::tuple<std::string, std::string, int64_t>> fields;
};

struct FfiContract {
    int version = 1;
    std::optional<std::string> triple;
    std::string model;
    int ptr_width = 8;
    int long_width = 8;
    int wide_align = 8;
    std::optional<std::string> proved_sha256;
    std::vector<ContractStruct> structs;
    std::vector<ContractFn> fns;
};

static FfiContract build_contract(const BratFfiSurface &surf,
                                  const TargetModel &tm,
                                  const std::optional<std::string> &proved_sha) {
    FfiContract c;
    if (!tm.triple.empty()) c.triple = tm.triple;
    c.model = tm.model;
    c.ptr_width = tm.ptr;
    c.long_width = tm.lng;
    c.wide_align = tm.wide_align;
    c.proved_sha256 = proved_sha;

    auto layouts = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);
    for (const auto &[sn, fields] : surf.structs) {
        if (!layouts.count(sn)) continue;
        const auto &lay = layouts.at(sn);
        ContractStruct cs;
        cs.name = sn;
        cs.packed = surf.packed.count(sn) > 0;
        cs.is_union = surf.unions.count(sn) > 0;
        cs.size = lay.size;
        cs.align = lay.align;
        for (size_t i = 0; i < fields.size() && i < lay.slots.size(); ++i)
            cs.fields.emplace_back(fields[i].first, fields[i].second, lay.slots[i].second);
        c.structs.push_back(std::move(cs));
    }

    std::vector<const ExternDecl *> sorted_fns;
    sorted_fns.reserve(surf.fns.size());
    for (const auto &fn : surf.fns) sorted_fns.push_back(&fn);
    std::sort(sorted_fns.begin(), sorted_fns.end(), [](const ExternDecl *a, const ExternDecl *b) {
        return a->name < b->name;
    });
    for (const ExternDecl *dp : sorted_fns) {
        const auto &d = *dp;
        ContractFn cf;
        cf.name = d.name;
        cf.lib = d.lib;
        for (const auto &p : d.params) cf.params.push_back(p.ann.value_or("i64"));
        cf.ret = d.ret.value_or("none");
        cf.variadic = d.varargs.has_value();
        cf.owned = d.owned;
        cf.checked = d.checked;
        cf.effects = d.effects;
        cf.contracts = d.contracts;
        c.fns.push_back(std::move(cf));
    }
    return c;
}

static void write_contract_json(lsp_wire::JsonWriter &w, const FfiContract &c) {
    w.begin_obj();
    w.key_lit("version"); w.int64(c.version);
    w.key_lit("target");
    w.begin_obj();
    w.key_lit("triple");
    if (c.triple) w.str(*c.triple); else w.null_val();
    w.key_lit("model"); w.str(c.model);
    w.key_lit("ptr"); w.int64(c.ptr_width);
    w.key_lit("long"); w.int64(c.long_width);
    w.key_lit("wide_align"); w.int64(c.wide_align);
    w.end_obj();
    w.key_lit("proved");
    if (c.proved_sha256) {
        w.begin_obj();
        w.key_lit("sha256"); w.str(*c.proved_sha256);
        w.end_obj();
    } else {
        w.null_val();
    }
    w.key_lit("structs");
    w.begin_arr();
    for (const auto &s : c.structs) {
        w.begin_obj();
        w.key_lit("name"); w.str(s.name);
        w.key_lit("packed"); w.boolean(s.packed);
        if (s.is_union) { w.key_lit("union"); w.boolean(true); }
        w.key_lit("size"); w.int64(s.size);
        w.key_lit("align"); w.int64(s.align);
        w.key_lit("fields");
        w.begin_arr();
        for (const auto &[fn, ft, foff] : s.fields) {
            w.begin_obj();
            w.key_lit("name"); w.str(fn);
            w.key_lit("type"); w.str(ft);
            w.key_lit("offset"); w.int64(foff);
            w.end_obj();
        }
        w.end_arr();
        w.end_obj();
    }
    w.end_arr();
    w.key_lit("fns");
    w.begin_arr();
    for (const auto &f : c.fns) {
        w.begin_obj();
        w.key_lit("name"); w.str(f.name);
        w.key_lit("lib"); if (f.lib) w.str(*f.lib); else w.null_val();
        w.key_lit("params");
        w.begin_arr();
        for (const auto &p : f.params) w.str(p);
        w.end_arr();
        w.key_lit("ret"); w.str(f.ret);
        w.key_lit("variadic"); w.boolean(f.variadic);
        w.key_lit("owned"); w.boolean(f.owned);
        w.key_lit("checked"); w.boolean(f.checked);
        w.key_lit("effects");
        w.begin_arr();
        for (const auto &e : f.effects) w.str(e);
        w.end_arr();
        w.key_lit("contracts");
        w.begin_arr();
        for (const auto &ct : f.contracts) w.str(ct);
        w.end_arr();
        w.end_obj();
    }
    w.end_arr();
    w.end_obj();
}

static bool parse_contract_from_tape(const lsp_wire::LspJsonTape &tape, uint32_t obj, FfiContract &out) {
    if (!tape.is_object(obj)) return false;
    out = FfiContract{};
    out.version = tape.int_or(tape.get(obj, "version"), 1);
    uint32_t tgt = tape.get(obj, "target");
    if (tape.is_object(tgt)) {
        uint32_t tr = tape.get(tgt, "triple");
        if (tape.is_string(tr)) out.triple = std::string(tape.str_view(tr));
        uint32_t md = tape.get(tgt, "model");
        if (tape.is_string(md)) out.model = std::string(tape.str_view(md));
        out.ptr_width = static_cast<int>(tape.int_or(tape.get(tgt, "ptr"), 8));
        out.long_width = static_cast<int>(tape.int_or(tape.get(tgt, "long"), 8));
        out.wide_align = static_cast<int>(tape.int_or(tape.get(tgt, "wide_align"),
                         (out.model == "ilp32" ? 4 : 8)));
    }
    uint32_t prv = tape.get(obj, "proved");
    if (tape.is_object(prv)) {
        uint32_t sh = tape.get(prv, "sha256");
        if (tape.is_string(sh)) out.proved_sha256 = std::string(tape.str_view(sh));
    }
    uint32_t sts = tape.get(obj, "structs");
    if (tape.is_array(sts)) {
        for (uint32_t cur = tape.at(sts).first_child; cur != 0; cur = tape.at(cur).next_sibling) {
            if (!tape.is_object(cur)) continue;
            ContractStruct cs;
            cs.name = std::string(tape.str_view(tape.get(cur, "name")));
            cs.packed = tape.bool_val(tape.get(cur, "packed"), false);
            cs.is_union = tape.bool_val(tape.get(cur, "union"), false);
            cs.size = tape.int_or(tape.get(cur, "size"), 0);
            cs.align = tape.int_or(tape.get(cur, "align"), 1);
            uint32_t fl = tape.get(cur, "fields");
            if (tape.is_array(fl)) {
                for (uint32_t fc = tape.at(fl).first_child; fc != 0; fc = tape.at(fc).next_sibling) {
                    if (!tape.is_object(fc)) continue;
                    cs.fields.emplace_back(
                        std::string(tape.str_view(tape.get(fc, "name"))),
                        std::string(tape.str_view(tape.get(fc, "type"))),
                        tape.int_or(tape.get(fc, "offset"), 0));
                }
            }
            out.structs.push_back(std::move(cs));
        }
    }
    uint32_t fns = tape.get(obj, "fns");
    if (tape.is_array(fns)) {
        for (uint32_t cur = tape.at(fns).first_child; cur != 0; cur = tape.at(cur).next_sibling) {
            if (!tape.is_object(cur)) continue;
            ContractFn cf;
            cf.name = std::string(tape.str_view(tape.get(cur, "name")));
            uint32_t lb = tape.get(cur, "lib");
            if (tape.is_string(lb)) cf.lib = std::string(tape.str_view(lb));
            uint32_t ps = tape.get(cur, "params");
            if (tape.is_array(ps)) {
                for (uint32_t pc = tape.at(ps).first_child; pc != 0; pc = tape.at(pc).next_sibling)
                    if (tape.is_string(pc)) cf.params.emplace_back(tape.str_view(pc));
            }
            cf.ret = std::string(tape.str_view(tape.get(cur, "ret")));
            if (cf.ret.empty()) cf.ret = "none";
            uint32_t va = tape.get(cur, "varargs");
            cf.variadic = tape.bool_val(tape.get(cur, "variadic"), false) || tape.is_string(va);
            cf.owned = tape.bool_val(tape.get(cur, "owned"), false);
            cf.checked = tape.bool_val(tape.get(cur, "checked"), false);
            uint32_t ef = tape.get(cur, "effects");
            if (tape.is_array(ef)) {
                for (uint32_t ec = tape.at(ef).first_child; ec != 0; ec = tape.at(ec).next_sibling)
                    if (tape.is_string(ec)) cf.effects.emplace_back(tape.str_view(ec));
            }
            uint32_t ct = tape.get(cur, "contracts");
            if (tape.is_array(ct)) {
                for (uint32_t cc = tape.at(ct).first_child; cc != 0; cc = tape.at(cc).next_sibling)
                    if (tape.is_string(cc)) cf.contracts.emplace_back(tape.str_view(cc));
            }
            out.fns.push_back(std::move(cf));
        }
    }
    return true;
}

static bool extract_binary_contract_json(const std::string &bin_path, std::string &raw_json_out, FfiContract &ct_out, std::string &err) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in) { err = "cannot open binary: " + bin_path; return false; }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0 || sz > 256 * 1024 * 1024) { err = "binary unreadable: " + bin_path; return false; }
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(sz), '\0');
    if (sz > 0) in.read(bytes.data(), sz);

    size_t pos = 0;
    while ((pos = bytes.find(kContractMagic, pos)) != std::string::npos) {
        size_t jstart = pos + kContractMagic.size();
        size_t jend = bytes.find(kContractEnd, jstart);
        if (jend != std::string::npos && jend - jstart <= 4 * 1024 * 1024) {
            std::string_view slice(bytes.data() + jstart, jend - jstart);
            lsp_wire::LspJsonTape tape;
            if (tape.parse(slice) && parse_contract_from_tape(tape, tape.root(), ct_out)) {
                raw_json_out = std::string(slice);
                return true;
            }
        }
        pos = jstart;
    }
    err = bin_path + " carries no FFI contract";
    return false;
}

static std::string fmt_contract_fn_sig(const ContractFn &f) {
    std::string ps;
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) ps += ", ";
        ps += f.params[i].empty() ? "?" : f.params[i];
    }
    if (f.variadic) ps += (ps.empty() ? "" : ", ") + std::string("..ptr");
    std::string r = f.ret.empty() ? "none" : f.ret;
    return r + "(" + ps + ")";
}

static std::vector<std::string> compute_contract_drift(const FfiContract &was, const FfiContract &now) {
    std::vector<std::string> drift;
    if (was.model != now.model) drift.push_back("target model: recorded " + was.model + ", now " + now.model);
    if (was.ptr_width != now.ptr_width) drift.push_back("target ptr: recorded " + std::to_string(was.ptr_width) + ", now " + std::to_string(now.ptr_width));
    if (was.long_width != now.long_width) drift.push_back("target long: recorded " + std::to_string(was.long_width) + ", now " + std::to_string(now.long_width));

    if (was.proved_sha256 && now.proved_sha256 && *was.proved_sha256 != *now.proved_sha256) {
        drift.push_back("header sha256: recorded " + was.proved_sha256->substr(0, 12) +
                        ", now " + now.proved_sha256->substr(0, 12));
    } else if (was.proved_sha256 && !now.proved_sha256) {
        drift.push_back("this binary was proved against a header; the comparison has none");
    } else if (now.proved_sha256 && !was.proved_sha256) {
        drift.push_back("this binary carries no header proof, the comparison has one");
    }

    std::map<std::string, const ContractFn *> was_f, now_f;
    std::set<std::string> all_f;
    for (const auto &f : was.fns) { was_f[f.name] = &f; all_f.insert(f.name); }
    for (const auto &f : now.fns) { now_f[f.name] = &f; all_f.insert(f.name); }
    for (const auto &nm : all_f) {
        const auto *wf = was_f.count(nm) ? was_f.at(nm) : nullptr;
        const auto *nf = now_f.count(nm) ? now_f.at(nm) : nullptr;
        if (!nf) { drift.push_back("extern fn " + nm + ": recorded " + fmt_contract_fn_sig(*wf) + ", no longer declared"); continue; }
        if (!wf) { drift.push_back("extern fn " + nm + ": newly declared " + fmt_contract_fn_sig(*nf) + ", not in the binary"); continue; }
        if (wf->lib != nf->lib) {
            drift.push_back("extern fn " + nm + ": library " + wf->lib.value_or("none") + ", now " + nf->lib.value_or("none"));
        }
        std::string sa = fmt_contract_fn_sig(*wf), sb = fmt_contract_fn_sig(*nf);
        if (sa != sb) drift.push_back("extern fn " + nm + ": recorded " + sa + ", now " + sb);
    }

    std::map<std::string, const ContractStruct *> was_s, now_s;
    std::set<std::string> all_s;
    for (const auto &s : was.structs) { was_s[s.name] = &s; all_s.insert(s.name); }
    for (const auto &s : now.structs) { now_s[s.name] = &s; all_s.insert(s.name); }
    for (const auto &nm : all_s) {
        const auto *ws = was_s.count(nm) ? was_s.at(nm) : nullptr;
        const auto *ns = now_s.count(nm) ? now_s.at(nm) : nullptr;
        if (!ns) { drift.push_back("extern struct " + nm + ": recorded, no longer declared"); continue; }
        if (!ws) { drift.push_back("extern struct " + nm + ": newly declared, not in the binary"); continue; }
        if (ws->size != ns->size) drift.push_back("extern struct " + nm + ": size recorded " + std::to_string(ws->size) + ", now " + std::to_string(ns->size));
        if (ws->align != ns->align) drift.push_back("extern struct " + nm + ": align recorded " + std::to_string(ws->align) + ", now " + std::to_string(ns->align));
        if (ws->packed != ns->packed) drift.push_back("extern struct " + nm + ": packed recorded " + (ws->packed ? "true" : "false") + ", now " + (ns->packed ? "true" : "false"));
        if (ws->is_union != ns->is_union) drift.push_back("extern struct " + nm + ": union recorded " + (ws->is_union ? "true" : "false") + ", now " + (ns->is_union ? "true" : "false"));

        std::map<std::string, std::pair<std::string, int64_t>> af, bf;
        std::set<std::string> all_fld;
        for (const auto &[fn, ft, fo] : ws->fields) { af[fn] = {ft, fo}; all_fld.insert(fn); }
        for (const auto &[fn, ft, fo] : ns->fields) { bf[fn] = {ft, fo}; all_fld.insert(fn); }
        for (const auto &fn : all_fld) {
            if (!bf.count(fn)) drift.push_back("extern struct " + nm + "." + fn + ": recorded, gone");
            else if (!af.count(fn)) drift.push_back("extern struct " + nm + "." + fn + ": new field");
            else if (af[fn] != bf[fn]) {
                drift.push_back("extern struct " + nm + "." + fn + ": recorded " + af[fn].first +
                                " at " + std::to_string(af[fn].second) + ", now " + bf[fn].first +
                                " at " + std::to_string(bf[fn].second));
            }
        }
    }
    return drift;
}

static bool make_bratbind_json(const std::string &brat_path,
                               const std::string &header_path,
                               const std::vector<std::string> &objects,
                               std::string_view target,
                               bool preprocess,
                               const std::vector<std::string> &include_dirs,
                               const std::vector<std::string> &defines,
                               std::string &json_out,
                               std::string &err) {
    std::string raw_brat;
    if (!read_file_bytes(brat_path, raw_brat, err)) return false;
    std::string norm_brat = norm_lf(raw_brat);
    std::vector<StmtP> body;
    try {
        body = parse(norm_brat).body;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    auto surf = extract_surface(body);
    if (surf.structs.empty() && surf.fns.empty()) {
        err = brat_path + " declares no extern functions or structs";
        return false;
    }

    std::string raw_hdr_disk;
    if (!read_file_bytes(header_path, raw_hdr_disk, err)) return false;
    std::string hdr_file_sha = pkg_io::sha256_hex(raw_hdr_disk);

    std::string hdr_text;
    if (preprocess || !include_dirs.empty() || !defines.empty()) {
        if (!preprocess_header_cc(header_path, include_dirs, defines, target, hdr_text, err)) return false;
    } else {
        if (!read_header_inlined(header_path, hdr_text, err)) return false;
    }
    std::string hdr_text_sha = pkg_io::sha256_hex(hdr_text);

    // Prove across all 3 ABI data models (`lp64`, `llp64`, `ilp32`) unless a single `--target` was requested
    std::vector<TargetModel> models_to_try;
    if (!target.empty()) {
        models_to_try.push_back(make_target_model(target));
    } else {
        models_to_try.push_back(make_target_model("x86_64-unknown-linux-gnu"));
        models_to_try.push_back(make_target_model("x86_64-pc-windows-msvc"));
        models_to_try.push_back(make_target_model("i686-unknown-linux-gnu"));
    }

    std::map<std::string, FfiContract> proved_models;
    std::string first_fail;
    for (const auto &tm : models_to_try) {
        CDecl cd;
        std::string perr;
        if (!parse_c_header(hdr_text, tm, !(preprocess || !include_dirs.empty() || !defines.empty()), cd, perr)) {
            if (first_fail.empty()) first_fail = perr;
            continue;
        }
        auto rep = check_declarations(surf, cd, fs::path(header_path).filename().string(), tm);
        if (rep.errors == 0) {
            TargetModel ct_tm = target.empty() ? TargetModel{"", tm.model, tm.ptr, tm.lng, tm.wide_align} : tm;
            proved_models[tm.model] = build_contract(surf, ct_tm, hdr_file_sha);
        } else if (first_fail.empty()) {
            first_fail = rep.lines.empty() ? "header proof failed" : rep.lines[0];
        }
    }
    if (proved_models.empty()) {
        err = first_fail.empty() ? ("cannot prove " + brat_path + " against " + header_path) : first_fail;
        return false;
    }

    TargetModel host_tm = make_target_model(target);
    std::vector<std::tuple<std::string, std::string, bool>> obj_entries;
    for (const auto &op : objects) {
        std::string obytes;
        if (!read_file_bytes(op, obytes, err)) return false;
        auto orep = verify_object_against_surface(surf, op, host_tm);
        if (orep.errors > 0) {
            err = orep.lines.empty() ? ("object proof failed: " + op) : orep.lines[0];
            return false;
        }
        ObjectInfo oi;
        std::string ierr;
        inspect_object_file(op, oi, ierr);
        obj_entries.emplace_back(fs::path(op).filename().string(), pkg_io::sha256_hex(obytes),
                                 !oi.dwarf_fns.empty() || !oi.dwarf_structs.empty());
    }

    lsp_wire::JsonWriter w;
    w.begin_obj();
    w.key_lit("format"); w.str("bratbind");
    w.key_lit("version"); w.int64(1);
    w.key_lit("source"); w.str(fs::path(brat_path).filename().string());
    w.key_lit("source_sha256"); w.str(pkg_io::sha256_hex(norm_brat));
    w.key_lit("header");
    w.begin_obj();
    w.key_lit("name"); w.str(fs::path(header_path).filename().string());
    w.key_lit("sha256"); w.str(hdr_file_sha);
    w.key_lit("text_sha256"); w.str(hdr_text_sha);
    w.key_lit("text"); w.str(hdr_text);
    w.end_obj();
    w.key_lit("models");
    w.begin_obj();
    for (const auto &[mname, ct] : proved_models) {
        w.key(mname);
        w.begin_obj();
        w.key_lit("contract");
        write_contract_json(w, ct);
        w.end_obj();
    }
    w.end_obj();
    w.key_lit("objects");
    w.begin_arr();
    for (const auto &[onm, osha, hdwarf] : obj_entries) {
        w.begin_obj();
        w.key_lit("name"); w.str(onm);
        w.key_lit("sha256"); w.str(osha);
        w.key_lit("dwarf"); w.boolean(hdwarf);
        w.end_obj();
    }
    w.end_arr();
    w.end_obj();
    json_out = std::string(w.view()) + "\n";
    return true;
}

static std::optional<bool> run_freestanding_cc_check(const BratFfiSurface &surf,
                                                     const CDecl &cdecl,
                                                     std::string_view hdr_text,
                                                     const TargetModel &tm,
                                                     std::vector<std::string> &findings) {
    (void)hdr_text;
    std::vector<std::string> cc_argv;
    std::string zig = find_bin_in_path("zig");
    if (!zig.empty()) {
        cc_argv = {zig, "cc", "-target", tm.triple};
    } else {
        std::string clang = find_bin_in_path("clang");
        if (!clang.empty()) {
            cc_argv = {clang, "--target=" + tm.triple};
        } else if (tm.triple == "x86_64-unknown-linux-gnu" && sizeof(void *) == 8 && sizeof(long) == 8) {
            std::string gcc = find_bin_in_path("cc");
            if (gcc.empty()) gcc = find_bin_in_path("gcc");
            if (!gcc.empty()) cc_argv = {gcc};
        } else if ((tm.triple == "x86_64-pc-windows-msvc" || tm.triple == "x86_64-pc-windows-gnu") &&
                   sizeof(void *) == 8 && sizeof(long) == 4) {
            std::string gcc = find_bin_in_path("gcc");
            if (gcc.empty()) gcc = find_bin_in_path("cc");
            if (!gcc.empty()) cc_argv = {gcc};
        } else if (tm.triple == "i686-unknown-linux-gnu") {
            std::string gcc = find_bin_in_path("gcc");
            if (gcc.empty()) gcc = find_bin_in_path("cc");
            if (!gcc.empty()) cc_argv = {gcc, "-m32"};
        }
    }
    if (cc_argv.empty()) return std::nullopt;

    auto b_layouts = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);
    if (b_layouts.empty()) return true;

    // Strip `#include <...>` so `-ffreestanding -nostdinc` works with built-in or portable fixed-width typedefs.
    std::ostringstream src;
    src << "#ifdef __INT8_TYPE__\n"
        << "typedef __INT8_TYPE__ int8_t;\ntypedef __UINT8_TYPE__ uint8_t;\n"
        << "typedef __INT16_TYPE__ int16_t;\ntypedef __UINT16_TYPE__ uint16_t;\n"
        << "typedef __INT32_TYPE__ int32_t;\ntypedef __UINT32_TYPE__ uint32_t;\n"
        << "typedef __INT64_TYPE__ int64_t;\ntypedef __UINT64_TYPE__ uint64_t;\n"
        << "typedef __SIZE_TYPE__ size_t;\ntypedef __PTRDIFF_TYPE__ ptrdiff_t;\n"
        << "typedef __INTPTR_TYPE__ intptr_t;\ntypedef __UINTPTR_TYPE__ uintptr_t;\n"
        << "#else\n"
        << "typedef signed char int8_t;\ntypedef unsigned char uint8_t;\n"
        << "typedef short int16_t;\ntypedef unsigned short uint16_t;\n"
        << "typedef int int32_t;\ntypedef unsigned int uint32_t;\n"
        << "typedef long long int64_t;\ntypedef unsigned long long uint64_t;\n"
        << "typedef " << (tm.ptr == 4 ? "unsigned int" : "unsigned long long") << " size_t;\n"
        << "typedef " << (tm.ptr == 4 ? "int" : "long long") << " ptrdiff_t;\n"
        << "typedef ptrdiff_t intptr_t;\ntypedef size_t uintptr_t;\n"
        << "#endif\n"
        << "#ifdef __GNUC__\n"
        << "#define offsetof(t, m) __builtin_offsetof(t, m)\n"
        << "#else\n"
        << "#define offsetof(t, m) ((size_t)&(((t *)0)->m))\n"
        << "#endif\n";
    {
        std::istringstream hs{std::string(hdr_text)};
        std::string ln;
        while (std::getline(hs, ln)) {
            std::string t = trim_sv(ln);
            if (!t.empty() && t[0] == '#' && trim_sv(std::string_view(t).substr(1)).rfind("include", 0) == 0)
                continue;
            src << ln << "\n";
        }
    }
    auto emit_abi_leaf_offsets = [&](auto &self,
                                     const std::string &cn,
                                     const std::string &rec,
                                     const std::string &c_prefix,
                                     int64_t base_off) -> void {
        if (!b_layouts.count(rec) || !surf.structs.count(rec)) return;
        const auto &bl = b_layouts.at(rec);
        const auto &bfields = surf.structs.at(rec);
        std::map<std::string, std::string> ftypes(bfields.begin(), bfields.end());
        for (const auto &[fnm, foff] : bl.slots) {
            std::string ft = ftypes[fnm];
            auto ait = cdecl.anon_members.find({rec, fnm});
            if (ait != cdecl.anon_members.end() && surf.structs.count(ait->second)) {
                self(self, cn, ait->second, c_prefix, base_off + foff);
            } else if (cdecl.inline_structs.count(ft) && surf.structs.count(ft)) {
                std::string next_cp = c_prefix.empty() ? fnm : (c_prefix + "." + fnm);
                self(self, cn, ft, next_cp, base_off + foff);
            } else {
                std::string cp = c_prefix.empty() ? fnm : (c_prefix + "." + fnm);
                src << "_Static_assert(offsetof(" << cn << ", " << cp << ") == " << (base_off + foff) << ", \"offsetof\");\n";
            }
        }
    };
    for (const auto &[sn, bl] : b_layouts) {
        if (!cdecl.structs.count(sn) || cdecl.inline_structs.count(sn)) continue;
        std::string cn = cdecl.c_name(sn);
        src << "_Static_assert(sizeof(" << cn << ") == " << bl.size << ", \"sizeof " << sn << "\");\n";
        src << "_Static_assert(_Alignof(" << cn << ") == " << bl.align << ", \"alignof " << sn << "\");\n";
        emit_abi_leaf_offsets(emit_abi_leaf_offsets, cn, sn, "", 0);
    }

    std::string tmp_c;
    if (!write_temp_probe_c("brat-abi-cc-", src.str(), tmp_c)) return std::nullopt;

    cc_argv.insert(cc_argv.end(), {"-std=gnu11", "-ffreestanding", "-nostdinc", "-fsyntax-only", tmp_c});
    int rc = 0;
    std::string out_txt;
    bool ran = run_cmd_capture(cc_argv, rc, out_txt);
    std::error_code rm_ec;
    fs::remove(tmp_c, rm_ec);
    if (!ran) return std::nullopt;
    if (rc != 0) {
        // Distinguish missing cross target / multilib support from genuine `_Static_assert` failure
        if (out_txt.find("_Static_assert") == std::string::npos && out_txt.find("static assertion") == std::string::npos)
            return std::nullopt;
        findings.push_back("  C compiler static_assert failed: " + trim_sv(out_txt));
        return false;
    }
    return true;
}

}  // namespace

AbiMatrixResult abi_matrix(const std::string &source_path,
                           const std::string &header_path,
                           bool preprocess,
                           const std::vector<std::string> &include_dirs,
                           const std::vector<std::string> &defines) {
    AbiMatrixResult res;
    std::string src_text, err;
    if (!read_file_bytes(source_path, src_text, err)) { res.error = err; return res; }
    std::vector<StmtP> body;
    try {
        body = parse(norm_lf(src_text)).body;
    } catch (const std::exception &e) {
        res.error = e.what();
        return res;
    }
    auto surf = extract_surface(body);

    std::string hdr_text;
    bool strict = !(preprocess || !include_dirs.empty() || !defines.empty());
    if (!strict) {
        if (!preprocess_header_cc(header_path, include_dirs, defines, "", hdr_text, err)) { res.error = err; return res; }
    } else {
        if (!read_header_inlined(header_path, hdr_text, err)) { res.error = err; return res; }
    }

    static const std::array<const char *, 5> kTriples = {
        "x86_64-unknown-linux-gnu",
        "aarch64-apple-darwin",
        "x86_64-pc-windows-msvc",
        "i686-unknown-linux-gnu",
        "wasm32-unknown-wasi",
    };

    struct Row {
        TargetModel tm;
        CheckReport rep;
        std::optional<bool> cc_ok;
        std::map<std::string, RecordLayout> b_lay;
        std::map<std::string, RecordLayout> c_lay;
    };
    std::vector<Row> rows;
    bool all_ok = true;

    for (const char *tr : kTriples) {
        TargetModel tm = make_target_model(tr);
        CDecl cd;
        if (!parse_c_header(hdr_text, tm, strict, cd, err)) {
            res.error = err;
            return res;
        }
        auto rep = check_declarations(surf, cd, fs::path(header_path).filename().string(), tm);
        auto b_lay = compute_brat_struct_layout(surf.structs, surf.packed, surf.unions, tm);
        auto c_lay = compute_c_struct_layout(cd, tm);
        std::optional<bool> cc_ok = std::nullopt;
        if (rep.errors == 0) {
            cc_ok = run_freestanding_cc_check(surf, cd, hdr_text, tm, rep.lines);
            if (cc_ok.has_value() && !*cc_ok) ++rep.errors;
        }
        if (rep.errors > 0) all_ok = false;
        rows.push_back({std::move(tm), std::move(rep), cc_ok, std::move(b_lay), std::move(c_lay)});
    }

    lsp_wire::JsonWriter w;
    w.begin_obj();
    w.key_lit("version"); w.int64(1);
    w.key_lit("file"); w.str(source_path);
    w.key_lit("header"); w.str(header_path);
    w.key_lit("ok"); w.boolean(all_ok);
    w.key_lit("targets");
    w.begin_arr();
    for (const auto &r : rows) {
        w.begin_obj();
        w.key_lit("target"); w.str(r.tm.triple);
        w.key_lit("model"); w.str(r.tm.model);
        w.key_lit("ptr_width"); w.int64(r.tm.ptr);
        w.key_lit("long_width"); w.int64(r.tm.lng);
        w.key_lit("wide_align"); w.int64(r.tm.wide_align);
        w.key_lit("errors"); w.int64(r.rep.errors);
        w.key_lit("proven"); w.uint64(r.rep.proven);
        w.key_lit("cc_verified");
        if (r.cc_ok.has_value()) w.boolean(*r.cc_ok); else w.null_val();
        w.key_lit("findings");
        w.begin_arr();
        for (const auto &ln : r.rep.lines) w.str(trim_sv(ln));
        w.end_arr();
        w.key_lit("structs");
        w.begin_obj();
        for (const auto &[sn, _] : surf.structs) {
            w.key(sn);
            w.begin_obj();
            auto emit_l = [&](const char *k, const std::map<std::string, RecordLayout> &mp) {
                w.key(k);
                auto it = mp.find(sn);
                if (it == mp.end()) { w.null_val(); return; }
                w.begin_obj();
                w.key_lit("size"); w.int64(it->second.size);
                w.key_lit("align"); w.int64(it->second.align);
                w.key_lit("offsets");
                w.begin_obj();
                for (const auto &[fn, fo] : it->second.slots) { w.key(fn); w.int64(fo); }
                w.end_obj();
                w.end_obj();
            };
            emit_l("brat", r.b_lay);
            emit_l("c", r.c_lay);
            w.end_obj();
        }
        w.end_obj();
        w.end_obj();
    }
    w.end_arr();

    w.key_lit("differential");
    w.begin_obj();
    for (const auto &[sn, _] : surf.structs) {
        w.key(sn);
        w.begin_obj();
        for (const auto &r : rows) {
            w.key(r.tm.triple);
            w.begin_obj();
            auto emit_l = [&](const char *k, const std::map<std::string, RecordLayout> &mp) {
                w.key(k);
                auto it = mp.find(sn);
                if (it == mp.end()) { w.null_val(); return; }
                w.begin_obj();
                w.key_lit("size"); w.int64(it->second.size);
                w.key_lit("align"); w.int64(it->second.align);
                w.key_lit("offsets");
                w.begin_obj();
                for (const auto &[fn, fo] : it->second.slots) { w.key(fn); w.int64(fo); }
                w.end_obj();
                w.end_obj();
            };
            emit_l("brat", r.b_lay);
            emit_l("c", r.c_lay);
            w.end_obj();
        }
        w.end_obj();
    }
    w.end_obj();
    w.end_obj();

    std::ostringstream txt;
    txt << "ffi abi-matrix: " << fs::path(source_path).filename().string()
        << " vs " << fs::path(header_path).filename().string() << "\n";
    for (const auto &r : rows) {
        txt << "  " << (r.rep.errors == 0 ? "OK  " : "FAIL") << "  " << r.tm.triple
            << " (" << r.tm.model << ", ptr=" << r.tm.ptr << ", long=" << r.tm.lng
            << ", align8=" << r.tm.wide_align << ")";
        if (r.cc_ok.has_value()) txt << (*r.cc_ok ? " [cc verified]" : " [cc FAILED]");
        else txt << " [cc skipped]";
        txt << "\n";
        for (const auto &ln : r.rep.lines) txt << "      " << trim_sv(ln) << "\n";
    }
    for (const auto &[sn, _] : surf.structs) {
        txt << "  struct " << sn << ": ";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) txt << ", ";
            auto arch = rows[i].tm.triple.substr(0, rows[i].tm.triple.find('-'));
            auto it = rows[i].b_lay.find(sn);
            if (it != rows[i].b_lay.end())
                txt << arch << "=" << it->second.size << "B(align " << it->second.align << ")";
            else
                txt << arch << "=?";
        }
        txt << "\n";
    }

    res.status = all_ok ? 0 : 1;
    res.json = std::string(w.view());
    res.text = txt.str();
    return res;
}

bool verify_module_header(const std::vector<StmtP> &body,
                          std::string_view header_text,
                          std::string_view header_name,
                          std::string_view target,
                          std::string &error) {
    TargetModel tm = make_target_model(target);
    CDecl cd;
    if (!parse_c_header(header_text, tm, false, cd, error)) return false;
    auto surf = extract_surface(body);
    auto rep = check_declarations(surf, cd, header_name, tm);
    if (rep.errors > 0) {
        std::ostringstream oss;
        for (size_t i = 0; i < rep.lines.size(); ++i) {
            if (i) oss << "\n";
            oss << rep.lines[i];
        }
        error = oss.str();
        return false;
    }
    return true;
}

bool verify_bratbind_bytes(std::string_view brat_bytes,
                           std::string_view bind_bytes,
                           std::string_view target,
                           const std::map<std::string, std::string> &object_files,
                           std::string_view label,
                           std::string &error) {
    lsp_wire::LspJsonTape tape;
    if (!tape.parse(bind_bytes) || !tape.is_object(tape.root())) {
        error = std::string(label) + ": invalid JSON";
        return false;
    }
    uint32_t root = tape.root();
    if (tape.str_view(tape.get(root, "format")) != "bratbind" ||
        tape.int_or(tape.get(root, "version"), 0) != 1) {
        error = std::string(label) + ": unsupported bratbind format or version";
        return false;
    }

    std::string norm_src = norm_lf(brat_bytes);
    if (tape.str_view(tape.get(root, "source_sha256")) != pkg_io::sha256_hex(norm_src)) {
        error = std::string(label) + ": source SHA-256 mismatch (declarations were modified after proof)";
        return false;
    }

    uint32_t hdr = tape.get(root, "header");
    if (!tape.is_object(hdr) || !tape.is_string(tape.get(hdr, "text")) ||
        !tape.is_string(tape.get(hdr, "text_sha256"))) {
        error = std::string(label) + ": missing embedded header proof";
        return false;
    }
    std::string hdr_text(tape.str_view(tape.get(hdr, "text")));
    if (pkg_io::sha256_hex(hdr_text) != tape.str_view(tape.get(hdr, "text_sha256"))) {
        error = std::string(label) + ": embedded header SHA-256 mismatch";
        return false;
    }
    std::string hdr_name(tape.str_view(tape.get(hdr, "name")));
    if (hdr_name.empty()) hdr_name = "<bratbind-header>";

    TargetModel req_tm = make_target_model(target);
    uint32_t models = tape.get(root, "models");
    if (!tape.is_object(models)) {
        error = std::string(label) + ": contains no proved ABI data models";
        return false;
    }
    uint32_t mnode = tape.get(models, req_tm.model);
    if (!tape.is_object(mnode)) {
        error = std::string(label) + ": target ABI model `" + req_tm.model + "` is not proved in this .bratbind";
        return false;
    }

    std::vector<StmtP> body;
    try {
        body = parse(norm_src).body;
    } catch (const std::exception &e) {
        error = std::string(label) + ": " + e.what();
        return false;
    }
    auto surf = extract_surface(body);
    if (surf.structs.empty() && surf.fns.empty()) {
        error = std::string(label) + ": module contains no extern declarations";
        return false;
    }

    CDecl cd;
    std::string perr;
    if (!parse_c_header(hdr_text, req_tm, false, cd, perr)) {
        error = std::string(label) + ": " + perr;
        return false;
    }
    auto rep = check_declarations(surf, cd, hdr_name, req_tm);
    if (rep.errors > 0) {
        error = std::string(label) + ": header proof failed for `" + req_tm.model + "`:\n" +
                (rep.lines.empty() ? std::string() : rep.lines[0]);
        return false;
    }

    FfiContract rec_ct;
    if (!parse_contract_from_tape(tape, tape.get(mnode, "contract"), rec_ct)) {
        error = std::string(label) + ": missing contract for `" + req_tm.model + "`";
        return false;
    }
    std::optional<std::string> hdr_sha;
    if (tape.is_string(tape.get(hdr, "sha256"))) hdr_sha = std::string(tape.str_view(tape.get(hdr, "sha256")));
    TargetModel ct_tm = rec_ct.triple ? make_target_model(*rec_ct.triple)
                                      : TargetModel{"", req_tm.model, req_tm.ptr, req_tm.lng, req_tm.wide_align};
    FfiContract now_ct = build_contract(surf, ct_tm, hdr_sha);
    auto drift = compute_contract_drift(rec_ct, now_ct);
    if (!drift.empty()) {
        error = std::string(label) + ": contract drift for `" + req_tm.model + "`: " + drift[0];
        return false;
    }

    std::set<std::string> seen_rec_objs;
    uint32_t objs = tape.get(root, "objects");
    if (tape.is_array(objs)) {
        for (uint32_t cur = tape.at(objs).first_child; cur != 0; cur = tape.at(cur).next_sibling) {
            if (!tape.is_object(cur) || !tape.is_string(tape.get(cur, "name"))) {
                error = std::string(label) + ": invalid object entry";
                return false;
            }
            std::string oname(tape.str_view(tape.get(cur, "name")));
            seen_rec_objs.insert(oname);
            auto oit = object_files.find(oname);
            if (oit != object_files.end()) {
                std::string obytes, rerr;
                if (!read_file_bytes(oit->second, obytes, rerr)) { error = rerr; return false; }
                if (pkg_io::sha256_hex(obytes) != tape.str_view(tape.get(cur, "sha256"))) {
                    error = std::string(label) + ": bundled object " + oname + " SHA-256 mismatch";
                    return false;
                }
                auto orep = verify_object_against_surface(surf, oit->second, req_tm);
                if (orep.errors > 0) {
                    error = std::string(label) + ": binary object check failed:\n" + orep.lines[0];
                    return false;
                }
            }
        }
    }
    for (const auto &[oname, opath] : object_files) {
        if (!seen_rec_objs.count(oname)) {
            auto orep = verify_object_against_surface(surf, opath, req_tm);
            if (orep.errors > 0) {
                error = std::string(label) + ": binary object check failed:\n" + orep.lines[0];
                return false;
            }
        }
    }
    return true;
}

bool verify_module_bratbind_file(const std::string &brat_path,
                                 std::string_view brat_src,
                                 std::string_view target,
                                 bool &proved_out,
                                 std::string &error_out) {
    proved_out = false;
    error_out.clear();
    fs::path bp(brat_path);
    fs::path bind_path = bp;
    bind_path.replace_extension(".bratbind");
    std::error_code ec;
    if (!fs::is_regular_file(bind_path, ec)) return true;

    std::string raw_brat(brat_src);
    if (raw_brat.empty() && !read_file_bytes(brat_path, raw_brat, error_out)) return false;
    std::string raw_bind;
    if (!read_file_bytes(bind_path.string(), raw_bind, error_out)) return false;

    std::map<std::string, std::string> sibling_objs;
    fs::path parent = fs::absolute(bp).parent_path();
    for (const auto &ent : fs::directory_iterator(parent, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        std::string ext = ent.path().extension().string();
        if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".so" || ext == ".dylib" || ext == ".dll")
            sibling_objs[ent.path().filename().string()] = ent.path().string();
    }
    if (!verify_bratbind_bytes(raw_brat, raw_bind, target, sibling_objs, bind_path.string(), error_out))
        return false;
    proved_out = true;
    return true;
}

bool has_any_ffi_surface(const std::vector<StmtP> &body) {
    auto surf = extract_surface(body);
    return !surf.structs.empty() || !surf.fns.empty() || surf.has_dynamic_plugin_call;
}

int ffi_gate(const std::string &entry_path,
             const std::vector<std::pair<std::string, std::string>> &module_sources,
             const std::string &header_path,
             const std::vector<std::string> &object_paths,
             const std::string &target,
             const std::string &who,
             bool unsafe_ffi,
             bool quiet) {
    BratFfiSurface unproved_surf;
    bool has_any_ffi = false;
    bool unresolvable_plugin = false;
    fs::path abs_entry = fs::weakly_canonical(fs::path(entry_path));

    for (const auto &[mpath, msrc] : module_sources) {
        std::vector<StmtP> mbody;
        try {
            mbody = parse(norm_lf(msrc)).body;
        } catch (...) {
            continue;
        }
        auto msurf = extract_surface(mbody);
        if (msurf.structs.empty() && msurf.fns.empty() && !msurf.has_dynamic_plugin_call) continue;
        has_any_ffi = true;

        std::error_code ec;
        bool is_entry = (fs::weakly_canonical(fs::path(mpath), ec) == abs_entry) || (mpath == entry_path);
        if (!is_entry || (header_path.empty() && object_paths.empty())) {
            bool proved = false;
            std::string berr;
            if (!verify_module_bratbind_file(mpath, msrc, target, proved, berr)) {
                if (!unsafe_ffi) {
                    std::fprintf(stderr, "%s: FFI proof rejected (%s):\n  %s\n",
                                 who.c_str(), mpath.c_str(), berr.c_str());
                    return 1;
                }
            } else if (proved && !msurf.has_unresolvable_plugin_call) {
                continue;
            }
        }
        for (const auto &[sn, sf] : msurf.structs) unproved_surf.structs[sn] = sf;
        for (const auto &sn : msurf.packed) unproved_surf.packed.insert(sn);
        for (const auto &sn : msurf.unions) unproved_surf.unions.insert(sn);
        for (auto &fn : msurf.fns) unproved_surf.fns.push_back(std::move(fn));
        if (msurf.has_unresolvable_plugin_call) unresolvable_plugin = true;
    }

    if (!has_any_ffi) return 0;
    if (unproved_surf.structs.empty() && unproved_surf.fns.empty() && !unresolvable_plugin) return 0;

    if (unresolvable_plugin && !unsafe_ffi) {
        std::fprintf(stderr,
                     "%s: refusing %s: dynamic `lib_open`/`lib_sym`/`call` target or signature is not a string literal.\n"
                     "  Pass `--unsafe-ffi` if you accept runtime-only FFI without static header verification.\n",
                     who.c_str(), entry_path.c_str());
        return 1;
    }

    if (header_path.empty()) {
        if (!unsafe_ffi) {
            std::string cmd_lbl = (who.rfind("brat ", 0) == 0) ? who : ("brat " + who);
            std::fprintf(stderr,
                         "%s: refusing %s: it calls C and no header was given.\n"
                         "  A prototype that disagrees with the library is undefined behaviour, not a\n"
                         "  warning. Prove it, or say you are not going to:\n"
                         "      %s %s --header FILE     the declarations compared to C\n"
                         "      %s %s --unsafe-ffi      build it anyway, on your head\n",
                         who.c_str(), entry_path.c_str(),
                         cmd_lbl.c_str(), entry_path.c_str(),
                         cmd_lbl.c_str(), entry_path.c_str());
            return 1;
        }
        if (!quiet) {
            std::fprintf(stderr, "%s: %s calls C without a proof (--unsafe-ffi)\n",
                         who.c_str(), entry_path.c_str());
        }
        return 0;
    }

    TargetModel tm = make_target_model(target);
    std::string hdr_text, herr;
    if (!read_header_inlined(header_path, hdr_text, herr)) {
        std::fprintf(stderr, "%s: %s\n", who.c_str(), herr.c_str());
        return 1;
    }
    CDecl cd;
    if (!parse_c_header(hdr_text, tm, true, cd, herr)) {
        // Fallback to `--preprocess` if header uses `#if`/`#ifdef` and `cc` is available
        std::string pp_err;
        if (!preprocess_header_cc(header_path, {}, {}, target, hdr_text, pp_err) ||
            !parse_c_header(hdr_text, tm, false, cd, herr)) {
            std::fprintf(stderr, "%s: %s\n", who.c_str(), herr.c_str());
            return 1;
        }
    }
    auto rep = check_declarations(unproved_surf, cd, fs::path(header_path).filename().string(), tm);
    auto cprobe = (rep.errors == 0) ? compile_probe(unproved_surf, cd, header_path, {}, {}, tm)
                                    : CompilerProbeResult{};
    int obj_errors = 0;
    std::vector<std::string> obj_lines;
    for (const auto &op : object_paths) {
        auto orep = verify_object_against_surface(unproved_surf, op, tm);
        obj_errors += orep.errors;
        for (const auto &ln : orep.lines) obj_lines.push_back(ln);
    }
    for (const auto &ln : rep.lines) std::fprintf(stderr, "%s\n", ln.c_str());
    for (const auto &ln : obj_lines) std::fprintf(stderr, "%s\n", ln.c_str());
    if (!cprobe.output.empty() && cprobe.checked && !cprobe.ok)
        std::fprintf(stderr, "%s\n", cprobe.output.c_str());

    int total_errors = rep.errors + obj_errors + ((cprobe.checked && !cprobe.ok) ? 1 : 0);
    if (total_errors > 0) {
        std::string target_desc = header_path;
        for (size_t i = 0; i < object_paths.size(); ++i)
            target_desc += (i == 0 ? " + " : ", ") + object_paths[i];
        std::fprintf(stderr, "%s: refusing to build %s: %d binding error(s) against %s\n",
                     who.c_str(), entry_path.c_str(), total_errors, target_desc.c_str());
        return 1;
    }
    if (!quiet) {
        std::fprintf(stderr, "%s: %zu binding(s) proven against %s\n",
                     who.c_str(), rep.proven, header_path.c_str());
    }
    return 0;
}

int cmd_ffi(int argc, char **argv) {
    std::string src_file, header, bin_path, target, emit_lib, emit_bind_out;
    bool has_emit = false, has_emit_bind = false, abi_mat = false, json_mode = false, preprocess = false;
    std::vector<std::string> objects, inc_dirs, defines;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--json") { json_mode = true; continue; }
        if (a == "--abi-matrix") { abi_mat = true; continue; }
        if (a == "--preprocess") { preprocess = true; continue; }
        if (a == "--emit") { has_emit = true; continue; }
        if (a.rfind("--emit=", 0) == 0) { has_emit = true; emit_lib = a.substr(7); continue; }
        if (a == "--emit-lib" && i + 1 < argc) { has_emit = true; emit_lib = argv[++i]; continue; }
        if (a.rfind("--emit-lib=", 0) == 0) { has_emit = true; emit_lib = a.substr(11); continue; }
        if (a == "--emit-bind") {
            has_emit_bind = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' && std::string_view(argv[i + 1]).ends_with(".bratbind"))
                emit_bind_out = argv[++i];
            continue;
        }
        if (a.rfind("--emit-bind=", 0) == 0) { has_emit_bind = true; emit_bind_out = a.substr(12); continue; }
        if (a == "--header" && i + 1 < argc) { header = argv[++i]; continue; }
        if (a.rfind("--header=", 0) == 0) { header = a.substr(9); continue; }
        if (a == "--object" && i + 1 < argc) { objects.push_back(argv[++i]); continue; }
        if (a.rfind("--object=", 0) == 0) { objects.push_back(a.substr(9)); continue; }
        if (a == "--bin" && i + 1 < argc) { bin_path = argv[++i]; continue; }
        if (a.rfind("--bin=", 0) == 0) { bin_path = a.substr(6); continue; }
        if (a == "--target" && i + 1 < argc) { target = argv[++i]; continue; }
        if (a.rfind("--target=", 0) == 0) { target = a.substr(9); continue; }
        if (a == "-I" && i + 1 < argc) { inc_dirs.push_back(argv[++i]); continue; }
        if (a.rfind("-I", 0) == 0 && a.size() > 2) { inc_dirs.push_back(a.substr(2)); continue; }
        if (a == "-D" && i + 1 < argc) { defines.push_back(argv[++i]); continue; }
        if (a.rfind("-D", 0) == 0 && a.size() > 2) { defines.push_back(a.substr(2)); continue; }
        if (a == "--help" || a == "-h") {
            std::fprintf(stdout,
                         "usage: brat ffi <file.brat> [--header <h>] [--object <o>] "
                         "[--abi-matrix] [--emit-bind] [--bin <exe>] [--json]\n");
            return 0;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "ffi: unknown option: %s\n", a.c_str());
            return 2;
        }
        if (src_file.empty()) src_file = a;
    }

    // Mode 1: `brat ffi --bin <exe>` (standalone or drift check against source/header)
    if (!bin_path.empty() && src_file.empty()) {
        std::string raw_ct_json, err;
        FfiContract ct;
        if (!extract_binary_contract_json(bin_path, raw_ct_json, ct, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        if (json_mode) {
            lsp_wire::JsonWriter w;
            w.begin_obj();
            w.key_lit("binary"); w.str(bin_path);
            w.key_lit("contract");
            write_contract_json(w, ct);
            w.end_obj();
            std::printf("%s\n", std::string(w.view()).c_str());
        } else {
            std::printf("ffi contract in %s: model=%s ptr=%d long=%d (%zu struct(s), %zu fn(s))\n",
                        bin_path.c_str(), ct.model.c_str(), ct.ptr_width, ct.long_width,
                        ct.structs.size(), ct.fns.size());
            for (const auto &s : ct.structs)
                std::printf("  struct %s: %lldB (align %lld)\n", s.name.c_str(),
                            static_cast<long long>(s.size), static_cast<long long>(s.align));
            for (const auto &f : ct.fns)
                std::printf("  extern fn %s -> %s\n", f.name.c_str(), f.ret.c_str());
        }
        return 0;
    }

    // Mode 2: `brat ffi --emit [--emit-lib LIB] --header H`
    if (has_emit) {
        if (header.empty()) {
            std::fprintf(stderr, "ffi --emit requires --header <file.h>\n");
            return 2;
        }
        TargetModel tm = make_target_model(target);
        std::string hdr_text, err;
        if (preprocess || !inc_dirs.empty() || !defines.empty()) {
            if (!preprocess_header_cc(header, inc_dirs, defines, target, hdr_text, err)) {
                std::fprintf(stderr, "ffi: %s\n", err.c_str());
                return 1;
            }
        } else if (!read_header_inlined(header, hdr_text, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        CDecl cd;
        if (!parse_c_header(hdr_text, tm, false, cd, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        auto [text, _notes] = emit_brat_bindings(cd, header, emit_lib, false, tm);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }

    if (src_file.empty()) {
        std::fprintf(stderr, "usage: brat ffi <file.brat> [--header <h>] [--object <o>] [--abi-matrix] [--emit-bind] [--bin <exe>] [--json]\n");
        return 2;
    }

    // Mode 3: `--abi-matrix`
    if (abi_mat) {
        if (header.empty()) {
            std::fprintf(stderr, "ffi --abi-matrix requires --header <file.h>\n");
            return 2;
        }
        auto res = abi_matrix(src_file, header, preprocess, inc_dirs, defines);
        if (!res.error.empty()) {
            std::fprintf(stderr, "ffi: %s\n", res.error.c_str());
            return 1;
        }
        const std::string &out = json_mode ? res.json : res.text;
        std::fwrite(out.data(), 1, out.size(), stdout);
        if (json_mode) std::fputc('\n', stdout);
        return res.status;
    }

    // Mode 4: `--emit-bind`
    if (has_emit_bind) {
        if (header.empty()) {
            std::fprintf(stderr, "ffi --emit-bind requires --header <file.h>\n");
            return 2;
        }
        std::string bind_json, err;
        if (!make_bratbind_json(src_file, header, objects, target, preprocess, inc_dirs, defines, bind_json, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        if (emit_bind_out.empty()) {
            fs::path p(src_file);
            p.replace_extension(".bratbind");
            emit_bind_out = p.string();
        }
        std::ofstream out(emit_bind_out, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "ffi: cannot write %s\n", emit_bind_out.c_str());
            return 1;
        }
        out << bind_json;
        if (json_mode) std::fwrite(bind_json.data(), 1, bind_json.size(), stdout);
        else std::printf("ffi ok: wrote %s\n", emit_bind_out.c_str());
        return 0;
    }

    // Mode 5: Standard `brat ffi <file.brat> [--header H] [--object O] [--bin B] [--json]`
    std::string src_bytes, err;
    if (!read_file_bytes(src_file, src_bytes, err)) {
        std::fprintf(stderr, "ffi: %s\n", err.c_str());
        return 1;
    }
    std::vector<StmtP> body;
    try {
        body = parse(norm_lf(src_bytes)).body;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "ffi: %s\n", e.what());
        return 1;
    }
    auto surf = extract_surface(body);
    TargetModel tm = make_target_model(target);

    CheckReport total_rep;
    CompilerProbeResult cprobe;
    std::optional<std::string> hdr_sha;

    if (!header.empty()) {
        std::string raw_disk;
        if (!read_file_bytes(header, raw_disk, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        hdr_sha = pkg_io::sha256_hex(raw_disk);

        std::string hdr_text;
        bool strict = !(preprocess || !inc_dirs.empty() || !defines.empty());
        if (!strict) {
            if (!preprocess_header_cc(header, inc_dirs, defines, target, hdr_text, err)) {
                std::fprintf(stderr, "ffi: %s\n", err.c_str());
                return 1;
            }
        } else if (!read_header_inlined(header, hdr_text, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        CDecl cd;
        if (!parse_c_header(hdr_text, tm, strict, cd, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        total_rep = check_declarations(surf, cd, fs::path(header).filename().string(), tm);
        if (total_rep.errors == 0) {
            cprobe = compile_probe(surf, cd, header, inc_dirs, defines, tm);
            if (cprobe.checked && !cprobe.ok) {
                ++total_rep.errors;
                total_rep.lines.push_back("  compiler probe failed:\n" + cprobe.output);
            }
        }
    }

    for (const auto &op : objects) {
        auto orep = verify_object_against_surface(surf, op, tm);
        total_rep.errors += orep.errors;
        if (header.empty()) total_rep.proven += orep.proven;
        for (auto &ln : orep.lines) total_rep.lines.push_back(std::move(ln));
    }

    // Drift check if `--bin` was also given alongside `<file.brat>`
    std::vector<std::string> drift_lines;
    if (!bin_path.empty()) {
        std::string raw_ct_j;
        FfiContract bin_ct;
        if (!extract_binary_contract_json(bin_path, raw_ct_j, bin_ct, err)) {
            std::fprintf(stderr, "ffi: %s\n", err.c_str());
            return 1;
        }
        FfiContract now_ct = build_contract(surf, tm, hdr_sha);
        drift_lines = compute_contract_drift(bin_ct, now_ct);
        for (const auto &dl : drift_lines) {
            ++total_rep.errors;
            total_rep.lines.push_back("  drift: " + dl);
        }
    }

    auto lib_st = check_library_symbols(surf, fs::path(src_file).parent_path().string());
    std::vector<std::pair<std::string, std::string>> missing_entries;
    if (!lib_st.missing.empty() && target.empty() && objects.empty()) {
        for (const auto &[lb, sym] : lib_st.missing) {
            if (sym == "<cannot open library>") {
                // Linker-resolved bare names like `-lz` or `-lsqlite3` are notes unless path contains `/`
                if (lb.find('/') != std::string::npos || lb.find('\\') != std::string::npos) {
                    ++total_rep.errors;
                    missing_entries.emplace_back(lb, "cannot open library `" + lb + "`");
                } else {
                    missing_entries.emplace_back(lb, "library `" + lb + "` (linker resolves at build time)");
                }
            } else {
                ++total_rep.errors;
                missing_entries.emplace_back(sym, lb + " exports no `" + sym + "`");
            }
        }
    }

    int rc = (total_rep.errors == 0) ? 0 : 1;
    if (json_mode) {
        lsp_wire::JsonWriter w;
        w.begin_obj();
        w.key_lit("file"); w.str(src_file);
        w.key_lit("header"); if (!header.empty()) w.str(header); else w.null_val();
        w.key_lit("errors"); w.int64(total_rep.errors);
        w.key_lit("warnings"); w.int64(total_rep.warnings);
        w.key_lit("proven"); w.uint64(total_rep.proven);
        w.key_lit("declarations"); w.uint64(surf.structs.size() + surf.fns.size());
        w.key_lit("libraries");
        w.begin_arr();
        for (const auto &lb : lib_st.libraries) {
            if (lb.find('/') != std::string::npos || lb.find('\\') != std::string::npos)
                w.str(fs::absolute(fs::path(lb)).lexically_normal().string());
            else
                w.str(lb);
        }
        w.end_arr();
        w.key_lit("missing");
        w.begin_arr();
        for (const auto &[sym, reason] : missing_entries) {
            w.begin_obj();
            w.key_lit("symbol"); w.str(sym);
            w.key_lit("reason"); w.str(reason);
            w.end_obj();
        }
        w.end_arr();
        w.key_lit("findings");
        w.begin_arr();
        for (const auto &ln : total_rep.lines) w.str(trim_sv(ln));
        w.end_arr();
        w.key_lit("notes");
        w.begin_arr();
        for (const auto &nt : total_rep.notes) w.str(nt);
        w.end_arr();
        w.key_lit("drift");
        w.begin_arr();
        for (const auto &dl : drift_lines) w.str(dl);
        w.end_arr();
        w.key_lit("compiler");
        w.begin_obj();
        w.key_lit("checked"); w.boolean(cprobe.checked);
        w.key_lit("ok"); w.boolean(cprobe.ok);
        w.key_lit("output");
        w.begin_arr();
        if (!cprobe.output.empty()) {
            std::istringstream css(cprobe.output);
            std::string cln;
            while (std::getline(css, cln)) w.str(cln);
        }
        w.end_arr();
        w.end_obj();
        w.end_obj();
        std::printf("%s\n", std::string(w.view()).c_str());
    } else {
        if (!bin_path.empty()) {
            if (drift_lines.empty() && rc == 0) {
                std::printf("ffi ok: %s contract matches %s\n", bin_path.c_str(), src_file.c_str());
            } else {
                for (const auto &dl : drift_lines) std::printf("  %s\n", dl.c_str());
                for (const auto &ln : total_rep.lines) std::printf("%s\n", ln.c_str());
            }
        } else {
            for (const auto &ln : total_rep.lines) std::printf("%s\n", ln.c_str());
            for (const auto &[sym, reason] : missing_entries) {
                const char *tag = (reason.find("linker resolves") != std::string::npos) ? "note" : "error";
                std::printf("  %s: %s\n", tag, reason.c_str());
            }
            for (const auto &nt : total_rep.notes) std::printf("  note: %s\n", nt.c_str());
            if (!header.empty() || !objects.empty()) {
                std::printf("%s: %zu binding(s) proven, %d error(s), %d warning(s)\n",
                            src_file.c_str(), total_rep.proven, total_rep.errors, total_rep.warnings);
            } else {
                std::printf("%s: %zu binding(s) checked against the library only\n",
                            src_file.c_str(), surf.fns.size());
            }
        }
    }
    return rc;
}

int cmd_bindgen(int argc, char **argv) {
    std::string out_path, target, lang = "brat", lib;
    bool inline_arrays = false, preprocess = false, header_only = false;
    std::vector<std::string> inc_dirs, defines, files;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--inline-arrays") { inline_arrays = true; continue; }
        if (a == "--preprocess") { preprocess = true; continue; }
        if (a == "--header-only") { header_only = true; continue; }
        if (a == "-I" && i + 1 < argc) { inc_dirs.push_back(argv[++i]); continue; }
        if (a.rfind("-I", 0) == 0 && a.size() > 2) { inc_dirs.push_back(a.substr(2)); continue; }
        if (a == "-D" && i + 1 < argc) { defines.push_back(argv[++i]); continue; }
        if (a.rfind("-D", 0) == 0 && a.size() > 2) { defines.push_back(a.substr(2)); continue; }
        if (a == "--lib" && i + 1 < argc) { lib = argv[++i]; continue; }
        if (a.rfind("--lib=", 0) == 0) { lib = a.substr(6); continue; }
        if (a == "-o" && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (a == "--target" && i + 1 < argc) { target = argv[++i]; continue; }
        if (a.rfind("--target=", 0) == 0) { target = a.substr(9); continue; }
        if (a == "--lang" && i + 1 < argc) { lang = argv[++i]; continue; }
        if (a.rfind("--lang=", 0) == 0) { lang = a.substr(7); continue; }
        if (a == "--help" || a == "-h") {
            std::fprintf(stdout,
                         "usage: brat bindgen <header.h | lib.rs> [-o out] [--lib NAME] "
                         "[--preprocess] [--header-only] [-I DIR] [-D DEF] [--inline-arrays] "
                         "[--lang brat|python|go|rust] [--target TRIPLE]\n");
            return 0;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "bindgen: unknown option: %s\n", a.c_str());
            return 2;
        }
        files.push_back(a);
    }

    if (files.size() != 1) {
        std::fprintf(stderr,
                     "usage: brat bindgen <header.h | lib.rs> [-o out] [--lib NAME] "
                     "[--preprocess] [--header-only] [-I DIR] [-D DEF] [--inline-arrays] "
                     "[--lang brat|python|go|rust] [--target TRIPLE]\n");
        return 2;
    }
    if (lang != "brat" && lang != "python" && lang != "go" && lang != "rust") {
        std::fprintf(stderr, "bindgen: unknown --lang %s (expected brat, python, go, rust)\n", lang.c_str());
        return 2;
    }

    const std::string &path = files[0];
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        std::fprintf(stderr, "bindgen: no such file: %s\n", path.c_str());
        return 1;
    }

    if (std::string_view(path).ends_with(".rs")) {
        if (lang != "brat") {
            std::fprintf(stderr, "bindgen: --lang %s wants a C header, not a Rust source\n", lang.c_str());
            return 2;
        }
        std::string rsrc, rerr;
        if (!read_file_bytes(path, rsrc, rerr)) {
            std::fprintf(stderr, "bindgen: %s\n", rerr.c_str());
            return 1;
        }
        std::string text = bindgen_rust_source(rsrc, path, make_target_model(target));
        if (!out_path.empty()) {
            std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::fprintf(stderr, "bindgen: cannot write %s\n", out_path.c_str());
                return 1;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                std::fprintf(stderr, "bindgen: cannot write %s\n", out_path.c_str());
                return 1;
            }
            std::printf("bindgen ok %s\n", out_path.c_str());
        } else {
            std::fwrite(text.data(), 1, text.size(), stdout);
        }
        return 0;
    }

    TargetModel tm = make_target_model(target);
    std::string hdr_text, root_only_text, err;
    if (preprocess || !inc_dirs.empty() || !defines.empty()) {
        if (!preprocess_header_cc(path, inc_dirs, defines, target, hdr_text, err,
                                  header_only ? &root_only_text : nullptr)) {
            std::fprintf(stderr, "bindgen: %s\n", err.c_str());
            return 1;
        }
    } else {
        if (!read_header_inlined(path, hdr_text, err)) {
            std::fprintf(stderr, "bindgen: %s\n", err.c_str());
            return 1;
        }
        if (header_only && !read_file_bytes(path, root_only_text, err)) {
            std::fprintf(stderr, "bindgen: %s\n", err.c_str());
            return 1;
        }
    }

    CDecl cd;
    if (!parse_c_header(hdr_text, tm, false, cd, err)) {
        std::fprintf(stderr, "bindgen: %s\n", err.c_str());
        return 1;
    }

    if (header_only) {
        CDecl root_cd;
        std::string rerr;
        if (parse_c_header(root_only_text, tm, false, root_cd, rerr)) {
            std::set<std::string> keep_fns, keep_consts, keep_structs;
            for (const auto &[fn, _] : root_cd.fns) keep_fns.insert(fn);
            for (const auto &[cn, _] : root_cd.consts) keep_consts.insert(cn);
            for (const auto &[sn, _] : root_cd.structs) keep_structs.insert(sn);

            std::set<std::string> seen_types;
            std::function<void(std::string_view)> mark_type_structs = [&](std::string_view ctype) {
                if (!seen_types.insert(std::string(ctype)).second) return;
                auto r = resolve_c_type(&cd, ctype, tm);
                if (r.kind == "fn") {
                    std::string cb_ret;
                    std::vector<std::string> cb_ps;
                    if (parse_c_callback(ctype, &cd, cb_ret, cb_ps)) {
                        mark_type_structs(cb_ret);
                        for (const auto &cp : cb_ps) mark_type_structs(cp);
                    }
                    return;
                }
                std::string cand;
                if (r.kind == "struct" || r.kind == "ptr") cand = r.extra;
                else if (r.kind == "array" && r.arr_elem_k == "struct") cand = r.arr_elem_e;
                if (!cand.empty() && cd.structs.count(cand) && keep_structs.insert(cand).second) {
                    for (const auto &[_, ft] : cd.structs.at(cand)) mark_type_structs(ft);
                }
            };
            for (auto it = cd.fns.begin(); it != cd.fns.end();) {
                if (!keep_fns.count(it->first)) {
                    it = cd.fns.erase(it);
                } else {
                    mark_type_structs(it->second.ret);
                    for (const auto &pt : it->second.params) mark_type_structs(pt);
                    ++it;
                }
            }
            for (const auto &sn : std::vector<std::string>(keep_structs.begin(), keep_structs.end())) {
                if (cd.structs.count(sn)) {
                    for (const auto &[_, ft] : cd.structs.at(sn)) mark_type_structs(ft);
                }
            }
            for (auto it = cd.structs.begin(); it != cd.structs.end();) {
                if (!keep_structs.count(it->first)) it = cd.structs.erase(it);
                else ++it;
            }
            std::vector<std::pair<std::string, int64_t>> filtered_consts;
            for (const auto &kv : cd.consts) {
                if (keep_consts.count(kv.first)) filtered_consts.push_back(kv);
            }
            cd.consts = std::move(filtered_consts);
        }
    }

    std::vector<std::string> unexported_notes;
    if (!lib.empty() && target.empty() && !cd.fns.empty()) {
        std::string obj_path;
        std::error_code fs_ec;
        std::string lib_s(lib);
        const bool is_std_pseudo = (lib_s == "c" || lib_s == "m" || lib_s == "pthread" || lib_s == "dl");
        if (!is_std_pseudo && !fs::path(lib_s).is_absolute()) {
            fs::path cand = fs::path(std::string(path)).parent_path() / lib_s;
            if (fs::is_regular_file(cand, fs_ec)) obj_path = cand.string();
        }
        if (!is_std_pseudo && obj_path.empty() && fs::is_regular_file(lib_s, fs_ec)) obj_path = lib_s;
#if defined(__linux__) && defined(__GLIBC__)
        if (!is_std_pseudo && obj_path.empty() &&
            lib_s.find('/') == std::string::npos && lib_s.find('\\') == std::string::npos) {
            std::string so_cand = "lib" + lib_s + ".so";
            void *h = dlopen(so_cand.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!h) h = dlopen(lib_s.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (h) {
                struct link_map *lm = nullptr;
                if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name && lm->l_name[0] != '\0') {
                    if (fs::is_regular_file(lm->l_name, fs_ec)) obj_path = lm->l_name;
                }
                dlclose(h);
            }
        }
#endif
        ObjectInfo oi;
        std::string oerr;
        if (!obj_path.empty() && inspect_object_file(obj_path, oi, oerr)) {
            for (auto it = cd.fns.begin(); it != cd.fns.end();) {
                auto sit = oi.symbols.find(it->first);
                bool ok = (sit != oi.symbols.end() && sit->second.defined && sit->second.exported);
                if (!ok) {
                    unexported_notes.push_back("// skipped: " + it->first + ": not exported by " + lib_s + "\n");
                    it = cd.fns.erase(it);
                } else {
                    ++it;
                }
            }
        } else {
            BratFfiSurface probe;
            for (const auto &[fn_name, _] : cd.fns) {
                ExternDecl d;
                d.name = fn_name;
                d.lib = lib_s;
                probe.fns.push_back(std::move(d));
            }
            auto lib_st = check_library_symbols(probe, fs::path(std::string(path)).parent_path().string());
            bool lib_opened = true;
            for (const auto &[_, sym] : lib_st.missing) {
                if (sym == "<cannot open library>") {
                    lib_opened = false;
                    break;
                }
            }
            if (lib_opened && !lib_st.missing.empty()) {
                for (const auto &[_, sym] : lib_st.missing) {
                    if (cd.fns.erase(sym) > 0)
                        unexported_notes.push_back("// skipped: " + sym + ": not exported by " + lib_s + "\n");
                }
            }
        }
    }

    std::string text = (lang == "brat")
        ? emit_brat_bindings(cd, path, lib, inline_arrays, tm).first
        : emit_lang_bindings(cd, path, lang, lib, tm).first;
    if (lang == "brat" && !unexported_notes.empty()) {
        size_t nl = text.find('\n');
        if (nl != std::string::npos) {
            std::string extra;
            for (const auto &n : unexported_notes) extra += n;
            text.insert(nl + 1, extra);
        }
    }

    if (!out_path.empty()) {
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "bindgen: cannot write %s\n", out_path.c_str());
            return 1;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::fprintf(stderr, "bindgen: cannot write %s\n", out_path.c_str());
            return 1;
        }
        size_t n_decls = cd.fns.size() + cd.structs.size();
        std::printf("bindgen ok %s (%zu decl(s))\n", out_path.c_str(), n_decls);
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
    return 0;
}

}  // namespace native_ffi



