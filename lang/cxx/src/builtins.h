// Shared type-annotation and builtin predicates across static-analysis passes.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// A bare `"` inside a `{...}` placeholder ends the literal there. The checker
// warns where the pieces are joined with `+`; the parser stops where the next
// piece follows with no comma.
inline constexpr char kCutShortString[] =
    R"BRAT(string literal looks cut short: a `{...}` placeholder stops at a bare `"`, so `"{e[\"hits\"]}"` is not the placeholder it looks like (help: `"{e.hits}"`, or `let hits = e["hits"]` then `"{hits}"`; a quote inside a call is fine: `"{m.get(\"hits\")}"`))BRAT";

inline std::string trim_type_ann(std::string_view raw) {
    const size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    const size_t z = raw.find_last_not_of(" \t\r\n");
    return std::string(raw.substr(b, z - b + 1));
}

inline std::string unwrap_adt_ann(std::string_view raw) {
    std::string a = trim_type_ann(raw);
    if (a.size() > 8 && a.compare(0, 7, "Option[") == 0 && a.back() == ']')
        return trim_type_ann(std::string_view(a).substr(7, a.size() - 8));
    if (a.size() > 8 && a.compare(0, 7, "Result[") == 0 && a.back() == ']') {
        const std::string_view inner = std::string_view(a).substr(7, a.size() - 8);
        int depth = 0;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '[' || inner[i] == '(') ++depth;
            else if (inner[i] == ']' || inner[i] == ')') --depth;
            else if (inner[i] == ',' && depth == 0)
                return trim_type_ann(inner.substr(0, i));
        }
        return trim_type_ann(inner);
    }
    return a;
}

inline std::string container_elem_ann(std::string_view raw) {
    std::string a = trim_type_ann(raw);
    if (a.size() > 5 && a.compare(0, 4, "vec[") == 0 && a.back() == ']')
        return trim_type_ann(std::string_view(a).substr(4, a.size() - 5));
    if (a.size() > 5 && a.compare(0, 4, "map[") == 0 && a.back() == ']') {
        const std::string_view inner = std::string_view(a).substr(4, a.size() - 5);
        size_t comma = std::string_view::npos;
        int depth = 0;
        for (size_t i = 0; i < inner.size(); ++i) {
            const char c = inner[i];
            if (c == '[' || c == '(') ++depth;
            else if ((c == ']' || c == ')') && depth > 0) --depth;
            else if (c == ',' && depth == 0) { comma = i; break; }
        }
        return trim_type_ann(comma == std::string_view::npos ? inner : inner.substr(comma + 1));
    }
    return {};
}

inline constexpr bool is_copy_scalar_ann(std::string_view a) {
    return a == "i8" || a == "u8" || a == "i16" || a == "u16" ||
           a == "i32" || a == "u32" || a == "i64" || a == "u64" ||
           a == "usize" || a == "isize" || a == "f32" || a == "f64" ||
           a == "bool";
}

inline constexpr bool is_numeric_ann(std::string_view a) {
    return a != "bool" && is_copy_scalar_ann(a);
}

inline constexpr bool is_insert_mutator(std::string_view m) {
    return m == "push" || m == "ins" || m == "set";
}

inline constexpr bool is_inplace_mutator(std::string_view m) {
    return m == "pop" || m == "sort" || m == "sort_by" || m == "del";
}

inline constexpr bool is_vec_map_mutator(std::string_view m) {
    return is_insert_mutator(m) || is_inplace_mutator(m);
}

inline constexpr bool is_cap_mutator(std::string_view m) {
    return is_vec_map_mutator(m) || m == "clear";
}

inline constexpr bool is_ptr_write_builtin(std::string_view n) {
    return n == "ptr_write_i64" || n == "ptr_write_i32" ||
           n == "ptr_write_f64" || n == "ptr_write_f32" ||
           n == "ptr_write_u8" || n == "ptr_write_str" ||
           n == "ptr_write_ptr" || n == "ptr_write_struct" ||
           n == "slice_set";
}

inline constexpr bool is_ptr_read_builtin(std::string_view n) {
    return n == "ptr_read_i64" || n == "ptr_read_i32" ||
           n == "ptr_read_f64" || n == "ptr_read_f32" ||
           n == "ptr_read_u8" || n == "ptr_read_str" ||
           n == "ptr_read_ptr" || n == "ptr_read_struct";
}

// `+` and `*` also build strings and vectors, `&&` and `||` answer an operand;
// these answer a number or a bool, or raise.
inline constexpr bool is_scalar_result_op(std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
           op == "-" || op == "/" || op == "%" || op == "&" || op == "|" || op == "^" ||
           op == "<<" || op == ">>";
}

inline constexpr bool is_scalar_only_builtin(std::string_view n) {
    return n == "own" || n == "len" ||
           n == "is_ok" || n == "is_err" || n == "is_some" || n == "is_none" ||
           n == "is_vec" || n == "is_str" || n == "is_map" ||
           n == "is_bool" || n == "is_int" || n == "is_float" ||
           n == "__stag_is" || n == "__shas" ||
           n == "ptr_is_null" || n == "ptr_read_i64" || n == "ptr_read_i32" ||
           n == "ptr_read_f64" || n == "ptr_read_f32" || n == "ptr_read_u8" ||
           n == "re_match" || n == "fs_exists" || n == "fs_is_dir" ||
           n == "sbuf_len" || n == "slice_len" ||
           n == "time_ms" || n == "mono_ns" || n == "errno" || n == "i64" || n == "f64" ||
           n == "ord";
}
