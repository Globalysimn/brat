#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace native_ffi {

inline std::string trim_c_ws(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

inline bool is_c_ident0(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

inline bool is_c_identc(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline bool is_valid_c_ident(std::string_view s) {
    if (s.empty() || !is_c_ident0(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (!is_c_identc(s[i])) return false;
    return true;
}

std::vector<std::string> c_ident_words(std::string_view s);
bool spelled_plain_char(std::string_view ctype);
bool is_valid_brat_ffi_ident(std::string_view name, bool reject_c_kw = false);
std::string find_tool_in_path(std::string_view bin);

struct TargetModel {
    std::string triple;      // empty means host
    std::string model;       // "lp64", "llp64", "ilp32"
    int ptr = 8;
    int lng = 8;
    int wide_align = 8;
};

TargetModel make_target_model(std::string_view triple);

struct RecordLayout {
    int64_t size = 0;
    int64_t align = 1;
    std::vector<std::pair<std::string, int64_t>> slots;
};

struct CBitfield {
    std::string name;  // empty for unnamed padding bitfield
    std::string ctype;
    std::string width;
};

struct CFnDecl {
    std::string ret;
    std::vector<std::string> params;
    std::vector<std::string> param_names;
    bool variadic = false;
};

struct CDecl {
    std::map<std::string, CFnDecl> fns;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> structs;
    std::set<std::string> packed;
    std::set<std::string> unions;
    std::map<std::string, std::string> typedefs;
    std::map<std::string, std::string> fn_typedefs;
    std::map<std::string, std::vector<std::pair<std::string, int64_t>>> enums;
    std::map<std::string, std::string> enum_types;
    std::vector<std::pair<std::string, int64_t>> consts;
    std::set<std::string> inline_structs;
    std::map<std::pair<std::string, std::string>, std::string> anon_members;
    std::set<std::string> aliases;
    std::set<std::string> tags;
    std::set<std::string> anon;
    std::set<std::string> arrays;
    std::map<std::string, int> pack_align;
    std::map<std::string, std::vector<CBitfield>> bitfields;
    std::map<std::string, std::string> abi_unsupported;

    std::string c_name(const std::string &name) const {
        if (tags.count(name) && !aliases.count(name))
            return (unions.count(name) ? "union " : "struct ") + name;
        return name;
    }
};

struct ResolvedCType {
    std::string kind;        // e.g. "i32", "ptr", "str", "struct", "fn", "array", "typedef", "none"
    std::string extra;       // pointee/struct/fn-sig
    std::string arr_elem_k;  // when kind == "array"
    std::string arr_elem_e;
    int64_t arr_count = 0;
    bool opaque = false;     // exactly `void *`, the only C type BRAT's plain `ptr` stands for in a callback
};

bool read_header_inlined(const std::string &path, std::string &out, std::string &err);
bool preprocess_header_cc(const std::string &path,
                          const std::vector<std::string> &include_dirs,
                          const std::vector<std::string> &defines,
                          std::string_view target,
                          std::string &out,
                          std::string &err,
                          std::string *root_only_out = nullptr);

bool parse_c_header(std::string_view text,
                    const TargetModel &tm,
                    bool strict_directives,
                    CDecl &out,
                    std::string &err);

ResolvedCType resolve_c_type(const CDecl *cdecl, std::string_view raw_type, const TargetModel &tm);

bool parse_c_callback(std::string_view ctype,
                      const CDecl *cdecl,
                      std::string &ret_out,
                      std::vector<std::string> &params_out);

bool parse_brat_callback(std::string_view ann,
                         std::string &ret_out,
                         std::vector<std::string> &params_out);

bool is_const_pointee(std::string_view ct, const CDecl *cdecl = nullptr);

std::optional<std::pair<std::string, int64_t>> parse_array_field_type(std::string_view t);
std::string element_of_ptr(std::string_view t);
std::optional<int64_t> brat_abi_width(std::string_view t, const TargetModel &tm);
bool same_brat_width_type(std::string_view a, std::string_view b, const TargetModel &tm);
bool pointee_ok(std::string_view elem,
                std::string_view cextra,
                const std::set<std::string> &brat_structs,
                const TargetModel &tm);

std::map<std::string, RecordLayout> compute_brat_struct_layout(
    const std::map<std::string, std::vector<std::pair<std::string, std::string>>> &structs,
    const std::set<std::string> &packed,
    const std::set<std::string> &unions,
    const TargetModel &tm);

std::map<std::string, RecordLayout> compute_c_struct_layout(const CDecl &cdecl,
                                                            const TargetModel &tm);

std::set<std::string> platform_width_uses(const CDecl &cdecl);

std::pair<std::string, std::vector<std::string>> emit_brat_bindings(
    const CDecl &cdecl,
    std::string_view header_name,
    std::string_view lib,
    bool inline_arrays,
    const TargetModel &tm);

std::pair<std::string, std::vector<std::string>> emit_lang_bindings(
    const CDecl &cdecl,
    std::string_view header_name,
    std::string_view lang,
    std::string_view lib,
    const TargetModel &tm);

std::string bindgen_rust_source(std::string_view src,
                                std::string_view path,
                                const TargetModel &tm = make_target_model(""));

}  // namespace native_ffi
