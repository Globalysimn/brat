#include "ffi_cdecl.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace native_ffi {
namespace {

namespace fs = std::filesystem;

static std::string trim_s(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

static bool ident0(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
static bool identc(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

static bool bindgen_name_ok(std::string_view name, bool is_fn = false) {
    if (name.empty() || !ident0(name[0])) return false;
    for (size_t i = 1; i < name.size(); ++i)
        if (!identc(name[i])) return false;

    static const std::set<std::string_view> kBratKw = {
        "fn", "let", "mut", "ref", "move", "if", "else", "while", "for", "in", "return",
        "match", "struct", "enum", "impl", "trait", "import", "pub", "use", "const",
        "extern", "export", "packed", "spawn", "scope", "arena", "unsafe", "defer",
        "try", "catch", "break", "continue", "par", "as", "true", "false", "none",
        "and", "or", "not", "is",
    };
    static const std::set<std::string_view> kCKw = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
        "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
        "register", "restrict", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
        "_Bool", "_Complex", "_Imaginary", "_Static_assert", "_Thread_local",
    };
    if (kBratKw.count(name)) return false;
    if (is_fn && kCKw.count(name)) return false;
    return true;
}

static std::vector<std::string> struct_topo_order(const CDecl &cdecl, const TargetModel &tm) {
    std::vector<std::string> order;
    std::set<std::string> seen;
    std::function<void(const std::string &)> visit = [&](const std::string &name) {
        if (seen.count(name) || !cdecl.structs.count(name)) return;
        seen.insert(name);
        for (const auto &[_, ctype] : cdecl.structs.at(name)) {
            auto r = resolve_c_type(&cdecl, ctype, tm);
            if (r.kind == "struct" && !r.extra.empty() && r.extra != name) visit(r.extra);
            else if (r.kind == "array" && r.arr_elem_k == "struct" && !r.arr_elem_e.empty() && r.arr_elem_e != name)
                visit(r.arr_elem_e);
        }
        order.push_back(name);
    };
    for (const auto &[nm, _] : cdecl.structs) visit(nm);
    return order;
}

static std::optional<std::string> brat_scalar_of(std::string_view k) {
    static const std::set<std::string_view> kDirect = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
        "usize", "f32", "f64", "bool", "str", "ptr", "none"
    };
    if (kDirect.count(k)) return std::string(k);
    return std::nullopt;
}

static bool cb_spelled_plain_char(std::string_view ctype) {
    bool has_char = false;
    size_t i = 0;
    while (i < ctype.size()) {
        if (ident0(ctype[i])) {
            size_t j = i + 1;
            while (j < ctype.size() && identc(ctype[j])) ++j;
            std::string_view w = ctype.substr(i, j - i);
            if (w == "char") has_char = true;
            if (w == "signed" || w == "unsigned" || w == "int8_t" || w == "uint8_t") return false;
            i = j;
        } else {
            ++i;
        }
    }
    return has_char;
}

static std::optional<std::string> cb_ptr_to_brat(const CDecl &cdecl,
                                                 std::string_view slot_ctype,
                                                 const ResolvedCType &rc) {
    if (is_const_pointee(slot_ctype, &cdecl)) return std::nullopt;
    if (rc.opaque) return std::string("ptr");
    static const std::set<std::string_view> kPrim = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "bool"
    };
    if (!kPrim.count(rc.extra)) return std::nullopt;
    if ((rc.extra == "u8" || rc.extra == "i8") && cb_spelled_plain_char(slot_ctype))
        return std::nullopt;
    return "ptr[" + rc.extra + "]";
}

static std::optional<std::string> c_cb_to_brat(const CDecl &cdecl,
                                               std::string_view ctype,
                                               const std::set<std::string> &used_structs,
                                               const TargetModel &tm) {
    std::string cret;
    std::vector<std::string> cparams;
    if (!parse_c_callback(ctype, &cdecl, cret, cparams)) return std::nullopt;
    auto rr = resolve_c_type(&cdecl, cret, tm);
    std::string bret;
    if (rr.kind == "none") {
        bret.clear();
    } else if (rr.kind == "struct") {
        if (!used_structs.count(rr.extra)) return std::nullopt;
        bret = rr.extra;
    } else if (rr.kind == "ptr") {
        auto bp = cb_ptr_to_brat(cdecl, cret, rr);
        if (!bp) return std::nullopt;
        bret = *bp;
    } else if (auto bs = brat_scalar_of(rr.kind)) {
        bret = *bs;
    } else {
        return std::nullopt;
    }

    std::vector<std::string> bparams;
    for (const auto &cp : cparams) {
        auto pr = resolve_c_type(&cdecl, cp, tm);
        if (pr.kind == "none") return std::nullopt;
        if (pr.kind == "struct") {
            if (!used_structs.count(pr.extra)) return std::nullopt;
            bparams.push_back(pr.extra);
        } else if (pr.kind == "ptr") {
            auto bp = cb_ptr_to_brat(cdecl, cp, pr);
            if (!bp) return std::nullopt;
            bparams.push_back(*bp);
        } else if (auto bs = brat_scalar_of(pr.kind)) {
            bparams.push_back(*bs);
        } else {
            return std::nullopt;
        }
    }
    std::string head = "fn(";
    for (size_t i = 0; i < bparams.size(); ++i) {
        if (i) head += ", ";
        head += bparams[i];
    }
    head += ")";
    return bret.empty() ? head : (head + " -> " + bret);
}

static std::optional<std::string> lang_scalar_type(std::string_view kind, std::string_view extra, std::string_view lang) {
    struct Row { const char *py; const char *go; const char *rs; };
    static const std::map<std::string_view, Row> kMap = {
        {"i8", {"c_int8", "int8_t", "i8"}},
        {"u8", {"c_uint8", "uint8_t", "u8"}},
        {"i16", {"c_int16", "int16_t", "i16"}},
        {"u16", {"c_uint16", "uint16_t", "u16"}},
        {"i32", {"c_int32", "int32_t", "i32"}},
        {"u32", {"c_uint32", "uint32_t", "u32"}},
        {"i64", {"c_int64", "int64_t", "i64"}},
        {"u64", {"c_uint64", "uint64_t", "u64"}},
        {"usize", {"c_size_t", "size_t", "usize"}},
        {"bool", {"c_bool", "bool", "bool"}},
        {"f32", {"c_float", "float", "f32"}},
        {"f64", {"c_double", "double", "f64"}},
        {"str", {"c_char_p", "char*", "*const c_char"}},
        {"ptr", {"c_void_p", "void*", "*mut c_void"}},
    };
    auto pick = [&](const Row &r) -> std::string {
        if (lang == "python") return r.py;
        if (lang == "go") return r.go;
        return r.rs;
    };
    auto it = kMap.find(kind);
    if (it != kMap.end()) return pick(it->second);
    if (kind == "ptr" && !extra.empty()) return pick(kMap.at("ptr"));
    if (kind == "struct" && !extra.empty()) return std::string(extra);
    return std::nullopt;
}

}  // namespace

std::pair<std::string, std::vector<std::string>> emit_brat_bindings(
    const CDecl &cdecl_in,
    std::string_view header_name,
    std::string_view lib,
    bool inline_arrays,
    const TargetModel &tm) {
    CDecl cdecl = cdecl_in;
    std::vector<std::string> out;
    std::vector<std::string> notes;
    auto skip = [&](std::string s) { notes.push_back(std::move(s)); };

    std::string base_hdr = fs::path(std::string(header_name)).filename().string();
    out.push_back("// generated by brat bindgen from " + base_hdr);
    if (!lib.empty()) out.push_back("// library: " + std::string(lib));

    std::vector<std::string> const_lines;
    for (const auto &[cname, cval] : cdecl.consts) {
        if (bindgen_name_ok(cname)) const_lines.push_back("pub const " + cname + " = " + std::to_string(cval));
    }
    if (!const_lines.empty()) {
        for (auto &l : const_lines) out.push_back(std::move(l));
        out.emplace_back();
    }

    std::set<std::string> used;
    std::set<std::string> pointer_structs;
    for (const auto &[nm, fields] : cdecl.structs) {
        if (!fields.empty() && !cdecl.anon.count(nm) && !cdecl.bitfields.count(nm) &&
            (inline_arrays || !cdecl.arrays.count(nm)) && bindgen_name_ok(nm))
            pointer_structs.insert(nm);
    }

    auto brat_ptr = [&](std::string_view extra) -> std::string {
        static const std::set<std::string_view> kPrim = {
            "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "bool"
        };
        if (kPrim.count(extra) || pointer_structs.count(std::string(extra)))
            return "ptr[" + std::string(extra) + "]";
        return "ptr";
    };

    auto c_layouts = compute_c_struct_layout(cdecl, tm);
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> emitted_so_far;
    std::vector<std::string> emitted_order;
    std::vector<std::string> struct_lines;

    for (const auto &name : struct_topo_order(cdecl, tm)) {
        const auto &fields = cdecl.structs.at(name);
        if (!bindgen_name_ok(name)) {
            skip(name + ": a C struct name the BRAT parser reads as a keyword");
            continue;
        }
        if (fields.empty()) {
            skip("struct " + name + " has no body in the header: declared as `ptr`");
            continue;
        }
        if (cdecl.arrays.count(name) && !inline_arrays) {
            skip("struct " + name + " has inline array fields: not representable as an extern struct");
            continue;
        }
        if (cdecl.bitfields.count(name)) {
            std::string bf_desc;
            const auto &bfs = cdecl.bitfields.at(name);
            for (size_t i = 0; i < bfs.size(); ++i) {
                if (i) bf_desc += ", ";
                bf_desc += (bfs[i].name.empty() ? "<pad>" : bfs[i].name) + ":" + bfs[i].width;
            }
            skip("struct " + name + " has C bitfield(s) (" + bf_desc + "): not byte-addressed, declared as `ptr`");
            continue;
        }
        if (cdecl.anon.count(name)) {
            if (cdecl.arrays.count(name)) {
                skip("struct " + name + " has inline array fields: not representable as an extern struct");
            } else {
                skip("struct " + name + " holds an anonymous union or struct: no BRAT field name for it, so the record is not declared");
            }
            continue;
        }

        std::vector<std::pair<std::string, std::string>> members;
        bool ok = true;
        for (const auto &[fname, ctype] : fields) {
            auto r = resolve_c_type(&cdecl, ctype, tm);
            if (r.kind == "struct" && r.extra == name) {
                skip("struct " + name + " contains itself: declared as `ptr`");
                ok = false;
                break;
            }
            if (!bindgen_name_ok(fname)) {
                skip(name + "." + fname + ": a C member name the BRAT parser reads as a keyword");
                ok = false;
                break;
            }
            std::string b;
            if (r.kind == "array") {
                std::string eb;
                if (r.arr_elem_k == "struct" && used.count(r.arr_elem_e)) eb = r.arr_elem_e;
                else if (auto bs = brat_scalar_of(r.arr_elem_k); bs && *bs != "none") eb = *bs;
                if (eb.empty()) {
                    skip("struct " + name + "." + fname + ": array `" + ctype + "` has no BRAT spelling");
                    ok = false;
                    break;
                }
                b = eb + "[" + std::to_string(r.arr_count) + "]";
            } else if (r.kind == "fn") {
                auto cb_b = c_cb_to_brat(cdecl, ctype, used, tm);
                if (!cb_b) {
                    skip("struct " + name + "." + fname + ": callback `" + trim_s(ctype) + "` has no BRAT spelling");
                    ok = false;
                    break;
                }
                b = *cb_b;
            } else if (r.kind == "ptr") {
                b = brat_ptr(r.extra);
            } else if (r.kind == "struct") {
                if (used.count(r.extra)) b = r.extra;
                else {
                    skip("struct " + name + "." + fname + ": `" + ctype + "` has no BRAT spelling");
                    ok = false;
                    break;
                }
            } else if (auto bs = brat_scalar_of(r.kind); bs && *bs != "none") {
                b = *bs;
            } else {
                skip("struct " + name + "." + fname + ": `" + ctype + "` has no BRAT spelling");
                ok = false;
                break;
            }
            members.emplace_back(fname, b);
        }

        if (!ok || members.empty()) {
            pointer_structs.erase(name);
            continue;
        }

        if (ok && !members.empty()) {
            if (cdecl.pack_align.count(name) && cdecl.pack_align.at(name) > 1 && !cdecl.unions.count(name)) {
                auto probe_map = emitted_so_far;
                probe_map[name] = members;
                auto b_lays = compute_brat_struct_layout(probe_map, cdecl.packed, cdecl.unions, tm);
                if (c_layouts.count(name) && b_lays.count(name)) {
                    const auto &cl = c_layouts.at(name);
                    const auto &bl = b_lays.at(name);
                    if (cl.size != bl.size || cl.align != bl.align || cl.slots != bl.slots) {
                        pointer_structs.erase(name);
                        cdecl.anon.insert(name);
                        skip("struct " + name + ": #pragma pack(" + std::to_string(cdecl.pack_align.at(name)) +
                             ") layout (" + std::to_string(cl.size) + "B, align " + std::to_string(cl.align) +
                             ") differs from natural alignment");
                        continue;
                    }
                }
            }
            emitted_so_far[name] = members;
            emitted_order.push_back(name);
            used.insert(name);
        }
    }
    pointer_structs = used;
    static const std::set<std::string_view> kPrimPtr = {
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "usize", "f32", "f64", "bool"
    };
    for (const auto &name : emitted_order) {
        auto &members = emitted_so_far.at(name);
        std::string kw = cdecl.unions.count(name) ? "extern union"
                       : (cdecl.packed.count(name) ? "extern packed struct" : "extern struct");
        std::string mjoined;
        for (size_t i = 0; i < members.size(); ++i) {
            auto &mty = members[i].second;
            if (mty.size() > 5 && mty.rfind("ptr[", 0) == 0 && mty.back() == ']') {
                std::string inner = mty.substr(4, mty.size() - 5);
                if (!used.count(inner) && !kPrimPtr.count(inner)) mty = "ptr";
            }
            if (i) mjoined += ", ";
            mjoined += members[i].first + ": " + mty;
        }
        struct_lines.push_back(kw + " " + name + " { " + mjoined + " }");
    }
    for (auto &sl : struct_lines) out.push_back(std::move(sl));
    out.emplace_back();

    std::vector<std::string> fn_lines;
    for (const auto &[name, fn] : cdecl.fns) {
        if (!bindgen_name_ok(name, true)) {
            skip(name + ": a C function name the BRAT parser reads as a keyword");
            continue;
        }
        if (cdecl.abi_unsupported.count(name)) {
            skip(name + ": non-standard ABI (" + cdecl.abi_unsupported.at(name) + ")");
            continue;
        }
        auto rr = resolve_c_type(&cdecl, fn.ret, tm);
        std::string ret;
        if (rr.kind == "struct") {
            if (!used.count(rr.extra)) {
                skip(name + ": returns `" + rr.extra + "`, whose body is opaque");
                continue;
            }
            ret = rr.extra;
        } else if (rr.kind == "ptr") {
            ret = brat_ptr(rr.extra);
        } else if (auto bs = brat_scalar_of(rr.kind)) {
            ret = *bs;
        } else {
            skip(name + ": result `" + trim_s(fn.ret) + "` has no BRAT spelling");
            continue;
        }

        std::vector<std::string> args;
        std::set<std::string> seen_pnames;
        bool bad = false;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            const auto &cp = fn.params[i];
            auto pr = resolve_c_type(&cdecl, cp, tm);
            std::string b;
            if (pr.kind == "none") {
                skip(name + ": a `void` parameter");
                bad = true;
                break;
            } else if (pr.kind == "fn") {
                auto cb_b = c_cb_to_brat(cdecl, cp, used, tm);
                if (!cb_b) {
                    skip(name + ": callback parameter `" + trim_s(cp) + "` has no BRAT spelling");
                    bad = true;
                    break;
                }
                b = *cb_b;
            } else if (pr.kind == "struct") {
                if (!used.count(pr.extra)) {
                    skip(name + ": parameter " + std::to_string(i + 1) + " is `" + pr.extra + "`, whose body is opaque");
                    bad = true;
                    break;
                }
                b = pr.extra;
            } else if (pr.kind == "ptr") {
                b = brat_ptr(pr.extra);
            } else if (auto bs = brat_scalar_of(pr.kind)) {
                b = *bs;
            } else {
                skip(name + ": parameter `" + trim_s(cp) + "` has no BRAT spelling");
                bad = true;
                break;
            }
            std::string pn = (i < fn.param_names.size()) ? fn.param_names[i] : "";
            if (!bindgen_name_ok(pn, false) || seen_pnames.count(pn))
                pn = "a" + std::to_string(i);
            seen_pnames.insert(pn);
            args.push_back(pn + ": " + b);
        }
        if (bad) continue;
        if (fn.variadic) args.push_back("..ptr");
        std::string ajoined;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) ajoined += ", ";
            ajoined += args[i];
        }
        fn_lines.push_back("    extern fn " + name + "(" + ajoined + ") -> " + ret + ";");
    }

    if (!fn_lines.empty()) {
        out.push_back(lib.empty() ? "extern {" : ("extern \"" + std::string(lib) + "\" {"));
        for (auto &fl : fn_lines) out.push_back(std::move(fl));
        out.push_back("}");
    }

    std::string text = out[0] + "\n";
    for (const auto &why : notes) text += "// skipped: " + why + "\n";
    for (size_t i = 1; i < out.size(); ++i) text += out[i] + "\n";
    return {text, notes};
}

std::pair<std::string, std::vector<std::string>> emit_lang_bindings(
    const CDecl &cdecl,
    std::string_view header_name,
    std::string_view lang,
    std::string_view /*lib*/,
    const TargetModel &tm) {
    struct LangStruct {
        std::string name;
        std::vector<std::pair<std::string, std::string>> rows;
        bool packed = false;
    };
    std::vector<std::string> notes;
    std::vector<LangStruct> structs;
    std::set<std::string> ok_structs;

    for (const auto &name : struct_topo_order(cdecl, tm)) {
        const auto &fields = cdecl.structs.at(name);
        if (fields.empty() || cdecl.anon.count(name) || cdecl.bitfields.count(name) || cdecl.unions.count(name)) {
            notes.push_back("struct " + name + " has no representable body: skipped");
            continue;
        }
        std::vector<std::pair<std::string, std::string>> rows;
        bool bad = false;
        for (const auto &[fname, ctype] : fields) {
            auto r = resolve_c_type(&cdecl, ctype, tm);
            if (r.kind == "struct" && cdecl.structs.count(r.extra) && cdecl.structs.at(r.extra).empty()) {
                rows.emplace_back(fname, *lang_scalar_type("ptr", {}, lang));
                continue;
            }
            if (r.kind == "struct" && !ok_structs.count(r.extra)) {
                notes.push_back(name + "." + fname + ": `" + trim_s(ctype) + "` has no " + std::string(lang) + " spelling");
                bad = true;
                break;
            }
            auto lt = lang_scalar_type(r.kind, r.extra, lang);
            if (!lt) {
                notes.push_back(name + "." + fname + ": `" + trim_s(ctype) + "` has no " + std::string(lang) + " spelling");
                bad = true;
                break;
            }
            rows.emplace_back(fname, *lt);
        }
        if (!bad && !rows.empty()) {
            structs.push_back({name, std::move(rows), cdecl.packed.count(name) > 0});
            ok_structs.insert(name);
        }
    }

    std::string base_hdr = fs::path(std::string(header_name)).filename().string();
    std::vector<std::string> out;

    if (lang == "python") {
        out = {
            "\"\"\"Python bindings for " + base_hdr + ".",
            "Generated by brat bindgen from the same C view `brat ffi --header` proves.",
            "\"\"\"",
            "import ctypes",
            "",
            "lib = None",
            "",
            "def load(path):",
            "    global lib",
            "    lib = ctypes.CDLL(path)",
            "    _bind(lib)",
            "    return lib",
            ""
        };
        for (const auto &st : structs) {
            out.push_back("class " + st.name + "(ctypes.Structure):");
            if (st.packed) out.push_back("    _pack_ = 1");
            std::string fstr;
            for (size_t i = 0; i < st.rows.size(); ++i) {
                if (i) fstr += ", ";
                const auto &[fn, ft] = st.rows[i];
                std::string t_expr = ok_structs.count(ft) ? ft : ("ctypes." + ft);
                fstr += "(\"" + fn + "\", " + t_expr + ")";
            }
            out.push_back("    _fields_ = [" + fstr + "]");
            out.emplace_back();
        }
        std::vector<std::string> fns;
        for (const auto &[name, fn] : cdecl.fns) {
            auto rr = resolve_c_type(&cdecl, fn.ret, tm);
            std::optional<std::string> rt = (rr.kind == "none") ? std::optional<std::string>("None")
                                                                : lang_scalar_type(rr.kind, rr.extra, "python");
            if (rr.kind == "struct" && !ok_structs.count(rr.extra)) rt = std::nullopt;
            if (!rt) {
                notes.push_back(name + ": result `" + trim_s(fn.ret) + "` has no python spelling");
                continue;
            }
            std::vector<std::string> args;
            bool bad = false;
            for (const auto &cp : fn.params) {
                auto pr = resolve_c_type(&cdecl, cp, tm);
                if (pr.kind == "none") { bad = true; break; }
                auto pt = lang_scalar_type(pr.kind, pr.extra, "python");
                if (pr.kind == "struct" && !ok_structs.count(pr.extra)) pt = std::nullopt;
                if (!pt) {
                    notes.push_back(name + ": parameter `" + trim_s(cp) + "` has no python spelling");
                    bad = true;
                    break;
                }
                args.push_back(ok_structs.count(*pt) ? *pt : ("ctypes." + *pt));
            }
            if (bad) continue;
            fns.push_back("    f = lib." + name);
            fns.push_back("    f.restype = " + (*rt == "None" ? std::string("None")
                                                              : (ok_structs.count(*rt) ? *rt : ("ctypes." + *rt))));
            std::string aj;
            for (size_t i = 0; i < args.size(); ++i) { if (i) aj += ", "; aj += args[i]; }
            fns.push_back("    f.argtypes = [" + aj + "]");
        }
        if (!fns.empty()) {
            out.push_back("def _bind(lib):");
            for (auto &l : fns) out.push_back(std::move(l));
            out.emplace_back();
        }
        std::string text = out[0] + "\n";
        for (auto it = notes.rbegin(); it != notes.rend(); ++it) text += "# skipped: " + *it + "\n";
        for (size_t i = 1; i < out.size(); ++i) text += out[i] + "\n";
        return {text, notes};
    }

    if (lang == "rust") {
        out = {
            "// Rust bindings for " + base_hdr,
            "// Generated by brat bindgen from the same C view `brat ffi --header` proves.",
            "#![allow(non_camel_case_types, non_snake_case, dead_code)]",
            "",
            "use std::os::raw::{c_char, c_void};",
            ""
        };
        for (const auto &st : structs) {
            out.push_back(st.packed ? "#[repr(C, packed)]" : "#[repr(C)]");
            out.push_back("pub struct " + st.name + " {");
            for (const auto &[fn, ft] : st.rows) out.push_back("    pub " + fn + ": " + ft + ",");
            out.push_back("}");
            out.emplace_back();
        }
        out.push_back("extern \"C\" {");
        for (const auto &[name, fn] : cdecl.fns) {
            auto rr = resolve_c_type(&cdecl, fn.ret, tm);
            std::optional<std::string> rt = (rr.kind == "none") ? std::optional<std::string>("()")
                                                                : lang_scalar_type(rr.kind, rr.extra, "rust");
            if (rr.kind == "struct" && !ok_structs.count(rr.extra)) rt = std::nullopt;
            if (!rt) {
                notes.push_back(name + ": result `" + trim_s(fn.ret) + "` has no rust spelling");
                continue;
            }
            std::vector<std::string> args;
            bool bad = false;
            for (size_t i = 0; i < fn.params.size(); ++i) {
                const auto &cp = fn.params[i];
                auto pr = resolve_c_type(&cdecl, cp, tm);
                auto pt = (pr.kind == "none") ? std::nullopt : lang_scalar_type(pr.kind, pr.extra, "rust");
                if (pr.kind == "struct" && !ok_structs.count(pr.extra)) pt = std::nullopt;
                if (!pt) {
                    notes.push_back(name + ": parameter `" + trim_s(cp) + "` has no rust spelling");
                    bad = true;
                    break;
                }
                args.push_back("a" + std::to_string(i) + ": " + *pt);
            }
            if (bad) continue;
            if (fn.variadic) args.push_back("...");
            std::string aj;
            for (size_t i = 0; i < args.size(); ++i) { if (i) aj += ", "; aj += args[i]; }
            out.push_back("    pub fn " + name + "(" + aj + ") -> " + *rt + ";");
        }
        out.push_back("}");
        out.emplace_back();
        std::string text = out[0] + "\n";
        for (auto it = notes.rbegin(); it != notes.rend(); ++it) text += "// skipped: " + *it + "\n";
        for (size_t i = 1; i < out.size(); ++i) text += out[i] + "\n";
        return {text, notes};
    }

    // Go (cgo)
    auto go_type = [&](const std::string &ct) -> std::string {
        if (ok_structs.count(ct)) return ct;
        if (ct == "char*") return "*C.char";
        if (ct == "void*") return "unsafe.Pointer";
        return "C." + ct;
    };
    auto esc_c_inc = [](std::string_view p) {
        std::string s;
        for (char c : p) {
            if (c == '\r' || c == '\n') continue;
            if (c == '\\' || c == '"') s.push_back('\\');
            if (c == '*' && !s.empty() && s.back() == '/') s.push_back(' ');
            if (c == '/' && !s.empty() && s.back() == '*') s.push_back(' ');
            s.push_back(c);
        }
        return s;
    };
    out = {
        "// Go bindings for " + base_hdr,
        "// Generated by brat bindgen from the same C view `brat ffi --header` proves.",
        "",
        "package bindings",
        "",
        "/*",
        "#include <stdlib.h>",
        "#include <stdbool.h>",
        "#include \"" + esc_c_inc(header_name) + "\"",
        "*/",
        "import \"C\""
    };
    size_t import_c_idx = out.size() - 1;
    out.emplace_back();
    out.push_back("type (");
    for (const auto &st : structs) {
        std::string c_sp = (cdecl.tags.count(st.name) && !cdecl.aliases.count(st.name))
            ? ("C.struct_" + st.name) : ("C." + st.name);
        out.push_back("\t" + st.name + " = " + c_sp);
    }
    out.push_back(")");
    out.emplace_back();

    bool needs_unsafe = false;
    for (const auto &[name, fn] : cdecl.fns) {
        auto rr = resolve_c_type(&cdecl, fn.ret, tm);
        std::optional<std::string> rt = (rr.kind == "none") ? std::nullopt : lang_scalar_type(rr.kind, rr.extra, "go");
        if (rr.kind == "struct" && !ok_structs.count(rr.extra)) rt = std::nullopt;
        if (rr.kind != "none" && !rt) {
            notes.push_back(name + ": result `" + trim_s(fn.ret) + "` has no go spelling");
            continue;
        }
        std::vector<std::string> args;
        bool bad = false;
        bool uses_unsafe = (rt && *rt == "void*");
        for (size_t i = 0; i < fn.params.size(); ++i) {
            const auto &cp = fn.params[i];
            auto pr = resolve_c_type(&cdecl, cp, tm);
            auto pt = (pr.kind == "none") ? std::nullopt : lang_scalar_type(pr.kind, pr.extra, "go");
            if (pr.kind == "struct" && !ok_structs.count(pr.extra)) pt = std::nullopt;
            if (!pt) {
                notes.push_back(name + ": parameter `" + trim_s(cp) + "` has no go spelling");
                bad = true;
                break;
            }
            args.push_back("a" + std::to_string(i) + " " + go_type(*pt));
            uses_unsafe = uses_unsafe || (*pt == "void*");
        }
        if (bad) continue;
        needs_unsafe = needs_unsafe || uses_unsafe;
        std::string rs = rt ? (" " + go_type(*rt)) : "";
        std::string call_args;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) call_args += ", ";
            call_args += "a" + std::to_string(i);
        }
        std::string aj;
        for (size_t i = 0; i < args.size(); ++i) { if (i) aj += ", "; aj += args[i]; }
        std::string call = (rt ? "return " : "") + std::string("C.") + name + "(" + call_args + ")";
        out.push_back("// " + name + " mirrors the C declaration; cgo checks the ABI.");
        out.push_back("func " + name + "C(" + aj + ")" + rs + " { " + call + " }");
    }
    out.emplace_back();
    if (needs_unsafe) out.insert(out.begin() + static_cast<ptrdiff_t>(import_c_idx + 1), "import \"unsafe\"");
    std::string text = out[0] + "\n";
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) text += "// skipped: " + *it + "\n";
    for (size_t i = 1; i < out.size(); ++i) text += out[i] + "\n";
    return {text, notes};
}

std::string bindgen_rust_source(std::string_view raw_src, std::string_view path, const TargetModel &tm) {
    // Strip `//` and `/* ... */` comments preserving newlines.
    std::string src;
    src.reserve(raw_src.size());
    for (size_t i = 0; i < raw_src.size();) {
        if (i + 1 < raw_src.size() && raw_src[i] == '/' && raw_src[i + 1] == '/') {
            i += 2;
            while (i < raw_src.size() && raw_src[i] != '\n') ++i;
            src.push_back(' ');
        } else if (i + 1 < raw_src.size() && raw_src[i] == '/' && raw_src[i + 1] == '*') {
            i += 2;
            while (i + 1 < raw_src.size() && !(raw_src[i] == '*' && raw_src[i + 1] == '/')) {
                if (raw_src[i] == '\n') src.push_back('\n');
                ++i;
            }
            if (i + 1 < raw_src.size()) i += 2;
            src.push_back(' ');
        } else {
            src.push_back(raw_src[i++]);
        }
    }

    auto attrs_before = [&](size_t idx) {
        std::vector<std::string> lines;
        std::string prefix = trim_s(std::string_view(src).substr(0, idx));
        std::istringstream iss(prefix);
        std::string l;
        while (std::getline(iss, l)) lines.push_back(l);
        std::vector<std::string> out;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            std::string st = trim_s(*it);
            if (st.rfind("#[", 0) == 0) out.push_back(st);
            else if (out.empty() && (st == "pub" || st == "unsafe" || st == "pub(crate)" || st == "pub(super)" ||
                                     st.empty() || st.rfind("pub(", 0) == 0)) continue;
            else break;
        }
        return out;
    };

    auto balanced = [&](size_t open_at, char oc, char cc) -> size_t {
        int d = 0;
        for (size_t i = open_at; i < src.size(); ++i) {
            if (src[i] == oc) ++d;
            else if (src[i] == cc) {
                --d;
                if (d == 0) return i;
            }
        }
        return std::string::npos;
    };

    const std::string isize_brat = (tm.ptr == 4) ? "i32" : "i64";
    const std::map<std::string, std::string> kRustPrim = {
        {"i8", "i8"}, {"u8", "u8"}, {"i16", "i16"}, {"u16", "u16"},
        {"i32", "i32"}, {"u32", "u32"}, {"isize", isize_brat}, {"usize", "usize"},
        {"i64", "i64"}, {"u64", "u64"},
        {"f32", "f32"}, {"f64", "f64"}, {"bool", "bool"}, {"char", "u32"},
    };

    auto map_rust_type = [&](std::string_view raw_t, const std::set<std::string> &structs, const std::map<std::string, std::string> &enums)
        -> std::pair<std::string, bool> {
        std::string collapsed;
        bool prev_sp = false;
        for (char c : trim_s(raw_t)) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!prev_sp) collapsed.push_back(' ');
                prev_sp = true;
            } else {
                collapsed.push_back(c);
                prev_sp = false;
            }
        }
        std::string base = collapsed;
        for (;;) {
            size_t p = base.find("const ");
            if (p == std::string::npos) break;
            base.erase(p, 6);
        }
        base = trim_s(base);
        // Fat pointers (`&str`, `&[T]`, `*const str`, `*const [T]`, `&dyn T`) are two words in Rust, not a thin `ptr`.
        if (base == "&str" || base == "&mut str" || base.rfind("&[", 0) == 0 || base.rfind("&mut [", 0) == 0 ||
            base == "*str" || base == "*mut str" || base.rfind("*[", 0) == 0 || base.rfind("*mut [", 0) == 0 ||
            base.find("dyn ") != std::string::npos)
            return {"ptr", false};
        // Inline Rust arrays `[T; N]` passed by value are not pointers.
        if (!base.empty() && base[0] == '[') return {"ptr", false};
        if (!base.empty() && (base[0] == '&' || base[0] == '*')) return {"ptr", true};
        if (kRustPrim.count(base)) return {kRustPrim.at(base), true};
        if (base == "i128" || base == "u128" ||
            base == "String" || base == "str" || base == "Vec" || base == "Option" || base == "Result" ||
            base == "Box" || base == "Rc" || base == "Arc" || base.find('<') != std::string::npos)
            return {"ptr", false};
        if (enums.count(base)) return {enums.at(base), true};
        if (structs.count(base)) return {base, true};
        return {"ptr", false};
    };

    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> structs;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>> enums;
    std::map<std::string, std::string> enum_reprs;
    struct RustFn { std::string name; std::vector<std::pair<std::string, std::string>> params; std::string ret; };
    std::vector<RustFn> fns;
    std::vector<std::string> skipped;

    // Scan `struct Name {`
    for (size_t pos = 0; pos < src.size();) {
        size_t kw = src.find("struct", pos);
        if (kw == std::string::npos) break;
        if ((kw > 0 && identc(src[kw - 1])) || (kw + 6 < src.size() && identc(src[kw + 6]))) { pos = kw + 6; continue; }
        size_t p = kw + 6;
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        size_t ns = p;
        while (p < src.size() && identc(src[p])) ++p;
        std::string name = src.substr(ns, p - ns);
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        if (name.empty() || !ident0(name[0]) || p >= src.size() || src[p] != '{') { pos = kw + 6; continue; }
        auto attrs = attrs_before(kw);
        bool repr_c = false;
        for (const auto &a : attrs) if (a.find("repr(C") != std::string::npos) repr_c = true;
        size_t end = balanced(p, '{', '}');
        if (end == std::string::npos) break;
        if (!repr_c) {
            skipped.push_back(name + ": not #[repr(C)]");
            pos = end + 1;
            continue;
        }
        if (!bindgen_name_ok(name)) {
            skipped.push_back(name + ": a Rust struct name the BRAT parser reads as a keyword");
            pos = end + 1;
            continue;
        }
        std::string body = src.substr(p + 1, end - p - 1);
        std::vector<std::pair<std::string, std::string>> rfields;
        std::istringstream bss(body);
        std::string part;
        bool bad_field_kw = false;
        while (std::getline(bss, part, ',')) {
            std::string pt = trim_s(part);
            if (pt.empty() || pt.find("fn ") != std::string::npos) continue;
            if (pt.rfind("pub ", 0) == 0) pt = trim_s(std::string_view(pt).substr(4));
            size_t col = pt.find(':');
            if (col == std::string::npos) continue;
            std::string fnm = trim_s(std::string_view(pt).substr(0, col));
            std::string fty = trim_s(std::string_view(pt).substr(col + 1));
            if (!bindgen_name_ok(fnm)) {
                skipped.push_back(name + "." + fnm + ": a Rust field name the BRAT parser reads as a keyword");
                bad_field_kw = true;
                break;
            }
            rfields.emplace_back(fnm, fty);
        }
        if (!bad_field_kw && !rfields.empty())
            structs.emplace_back(name, std::move(rfields));
        pos = end + 1;
    }

    // Scan `enum Name {`
    for (size_t pos = 0; pos < src.size();) {
        size_t kw = src.find("enum", pos);
        if (kw == std::string::npos) break;
        if ((kw > 0 && identc(src[kw - 1])) || (kw + 4 < src.size() && identc(src[kw + 4]))) { pos = kw + 4; continue; }
        size_t p = kw + 4;
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        size_t ns = p;
        while (p < src.size() && identc(src[p])) ++p;
        std::string name = src.substr(ns, p - ns);
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        if (name.empty() || !ident0(name[0]) || p >= src.size() || src[p] != '{') { pos = kw + 4; continue; }
        auto attrs = attrs_before(kw);
        std::string enum_repr;
        for (const auto &a : attrs) {
            if (a.find("repr(C") != std::string::npos) enum_repr = "i32";
            for (const char *rt : {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize", "isize"}) {
                if (a.find(std::string("repr(") + rt + ")") != std::string::npos ||
                    a.find(std::string("repr(") + rt + ",") != std::string::npos) {
                    enum_repr = (std::string_view(rt) == "isize") ? isize_brat : std::string(rt);
                }
            }
        }
        size_t end = balanced(p, '{', '}');
        if (end == std::string::npos) break;
        if (enum_repr.empty()) {
            skipped.push_back(name + ": not #[repr(C)] or #[repr(uN)]");
            pos = end + 1;
            continue;
        }
        std::string body = src.substr(p + 1, end - p - 1);
        if (body.find('(') != std::string::npos || body.find('{') != std::string::npos) {
            skipped.push_back(name + ": enum with payload");
            pos = end + 1;
            continue;
        }
        std::vector<std::pair<std::string, int64_t>> vars;
        int64_t nxt = 0;
        std::istringstream bss(body);
        std::string part;
        while (std::getline(bss, part, ',')) {
            std::string pt = trim_s(part);
            if (pt.empty()) continue;
            size_t eq = pt.find('=');
            std::string vnm = trim_s(eq == std::string::npos ? std::string_view(pt) : std::string_view(pt).substr(0, eq));
            if (eq != std::string::npos) {
                std::string vs = trim_s(std::string_view(pt).substr(eq + 1));
                char *ep = nullptr;
                errno = 0;
                long long v = std::strtoll(vs.c_str(), &ep, 0);
                if (errno == ERANGE || !ep || *ep != '\0') { skipped.push_back(name + ": cannot read variant `" + pt + "`"); continue; }
                nxt = v;
            }
            if (bindgen_name_ok(vnm)) vars.emplace_back(vnm, nxt);
            nxt = static_cast<int64_t>(static_cast<uint64_t>(nxt) + 1ULL);
        }
        enum_reprs[name] = enum_repr;
        enums.emplace_back(name, std::move(vars));
        pos = end + 1;
    }

    std::set<std::string> known_structs;
    std::map<std::string, std::string> known_enums;
    for (const auto &[nm, _] : structs) known_structs.insert(nm);
    for (const auto &[nm, _] : enums) known_enums[nm] = enum_reprs.count(nm) ? enum_reprs[nm] : "i32";

    // Validate all struct fields before function mapping; dropping an unrepresentable
    // field inside a `#[repr(C)]` struct would shift subsequent member offsets.
    std::vector<std::pair<std::string, std::vector<std::string>>> valid_structs;
    for (;;) {
        bool dropped_any = false;
        valid_structs.clear();
        std::set<std::string> failed_this_pass;
        for (const auto &[name, rfields] : structs) {
            if (!known_structs.count(name)) continue;
            std::vector<std::string> fl;
            bool ok_st = true;
            for (const auto &[fnm, fty] : rfields) {
                auto [bty, safe] = map_rust_type(fty, known_structs, known_enums);
                if (!safe || bty == name) {
                    failed_this_pass.insert(name);
                    ok_st = false;
                    break;
                }
                fl.push_back(fnm + ": " + bty);
            }
            if (ok_st && !fl.empty()) valid_structs.emplace_back(name, std::move(fl));
        }
        if (failed_this_pass.empty()) break;
        for (const auto &bad_nm : failed_this_pass) {
            known_structs.erase(bad_nm);
            dropped_any = true;
        }
        if (!dropped_any) break;
    }
    for (const auto &[name, rfields] : structs) {
        if (known_structs.count(name)) continue;
        for (const auto &[fnm, fty] : rfields) {
            auto [_bty, safe] = map_rust_type(fty, known_structs, known_enums);
            if (!safe || _bty == name) {
                skipped.push_back(name + "." + fnm + ": `" + fty + "` has no C layout");
                break;
            }
        }
    }

    // Scan `extern "C" fn name(`
    for (size_t pos = 0; pos < src.size();) {
        size_t ex = src.find("extern", pos);
        if (ex == std::string::npos) break;
        if (ex > 0 && identc(src[ex - 1])) { pos = ex + 6; continue; }
        size_t p = ex + 6;
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        if (src.compare(p, 3, "\"C\"") != 0) { pos = p; continue; }
        p += 3;
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        if (src.compare(p, 2, "fn") != 0 || (p + 2 < src.size() && identc(src[p + 2]))) { pos = p; continue; }
        p += 2;
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        size_t ns = p;
        while (p < src.size() && identc(src[p])) ++p;
        std::string name = src.substr(ns, p - ns);
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        if (name.empty() || p >= src.size() || src[p] != '(') { pos = p; continue; }
        size_t close_at = balanced(p, '(', ')');
        if (close_at == std::string::npos) break;
        if (!bindgen_name_ok(name, true)) {
            skipped.push_back(name + ": a Rust function name the BRAT parser reads as a keyword");
            pos = close_at + 1;
            continue;
        }
        std::string params_txt = trim_s(std::string_view(src).substr(p + 1, close_at - p - 1));
        size_t after = close_at + 1;
        while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after]))) ++after;
        std::string ret = "none";
        if (src.compare(after, 2, "->") == 0) {
            size_t rs = after + 2;
            size_t re = rs;
            while (re < src.size() && src[re] != '{' && src[re] != ';') ++re;
            std::string rraw = trim_s(std::string_view(src).substr(rs, re - rs));
            auto [bty, safe] = map_rust_type(rraw, known_structs, known_enums);
            if (!safe) {
                skipped.push_back(name + ": return type `" + rraw + "` has no C layout");
                pos = re;
                continue;
            }
            ret = bty;
        }
        std::vector<std::pair<std::string, std::string>> params;
        bool ok = true;
        if (!params_txt.empty()) {
            std::istringstream pss(params_txt);
            std::string part;
            size_t idx = 0;
            while (std::getline(pss, part, ',')) {
                std::string pt = trim_s(part);
                if (pt.empty()) { ++idx; continue; }
                if (pt == "self" || pt == "&self" || pt == "&mut self") {
                    skipped.push_back(name + ": method, not an FFI function");
                    ok = false;
                    break;
                }
                if (pt.rfind("mut ", 0) == 0) pt = trim_s(std::string_view(pt).substr(4));
                size_t col = pt.find(':');
                std::string pnm = (col == std::string::npos) ? ("a" + std::to_string(idx)) : trim_s(std::string_view(pt).substr(0, col));
                if (!bindgen_name_ok(pnm)) pnm = "a" + std::to_string(idx);
                std::string pty = (col == std::string::npos) ? pt : trim_s(std::string_view(pt).substr(col + 1));
                auto [bty, safe] = map_rust_type(pty, known_structs, known_enums);
                if (!safe) {
                    skipped.push_back(name + ": parameter `" + pty + "` has no C layout");
                    ok = false;
                    break;
                }
                params.emplace_back(pnm, bty);
                ++idx;
            }
        }
        if (ok) fns.push_back({name, std::move(params), ret});
        pos = close_at + 1;
    }

    std::vector<std::string> lines;
    lines.push_back("// generated by brat bindgen from " + fs::path(std::string(path)).filename().string());
    for (const auto &[name, fl] : valid_structs) {
        std::string fj;
        for (size_t i = 0; i < fl.size(); ++i) { if (i) fj += ", "; fj += fl[i]; }
        lines.push_back("extern struct " + name + " { " + fj + " }");
    }
    for (const auto &[ename, vars] : enums) {
        const std::string er = enum_reprs.count(ename) ? enum_reprs.at(ename) : "i32";
        lines.emplace_back();
        for (const auto &[vnm, vval] : vars)
            lines.push_back("const " + vnm + ": " + er + " = " + std::to_string(vval));
    }
    lines.emplace_back();
    lines.push_back("extern {");
    for (const auto &fn : fns) {
        std::string pj;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) pj += ", ";
            pj += fn.params[i].first + ": " + fn.params[i].second;
        }
        lines.push_back("    extern fn " + fn.name + "(" + pj + ") -> " + fn.ret + ";");
    }
    lines.push_back("}");
    for (const auto &note : skipped) lines.push_back("// skipped " + note);

    std::string res;
    for (const auto &l : lines) { res += l; res.push_back('\n'); }
    return res;
}

}  // namespace native_ffi

