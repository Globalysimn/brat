#include "lsp.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "borrow.h"
#include "check.h"
#include "json.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"

namespace {

using Obj = Json::Object;
using Arr = Json::Array;

Json obj(Obj v) { return Json(std::move(v)); }
Json arr(Arr v) { return Json(std::move(v)); }
Json pos(int line, int col) { return obj({{"line", line}, {"character", col}}); }
Json range(int line, int start, int end) {
    return obj({{"start", pos(line, start)}, {"end", pos(line, end)}});
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

uint32_t codepoint(const std::string &s, size_t &i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    size_t n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    uint32_t cp = c & (n == 2 ? 0x1f : n == 3 ? 0x0f : 0x07);
    for (size_t k = 1; k < n && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += n;
    return cp;
}

int utf16_length(const std::string &s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) n += codepoint(s, i) > 0xffff ? 2 : 1;
    return n;
}

bool position_offset(const std::string &text, int line, int character, size_t &offset) {
    if (line < 0 || character < 0) return false;
    size_t start = 0;
    for (int n = 0; n < line; ++n) {
        const size_t newline = text.find('\n', start);
        if (newline == std::string::npos) return false;
        start = newline + 1;
    }
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    else if (end > start && text[end - 1] == '\r') --end;
    size_t at = start;
    int units = 0;
    while (at < end) {
        if (units == character) {
            offset = at;
            return true;
        }
        const size_t next = at;
        const int width = codepoint(text, at) > 0xffff ? 2 : 1;
        if (at > end || units + width > character) return false;
        units += width;
        if (at <= next) return false;
    }
    if (units != character) return false;
    offset = end;
    return true;
}

bool exact_integer(const Json &value, int &out) {
    const auto *number = std::get_if<double>(&value.data);
    if (!number || !std::isfinite(*number) || std::trunc(*number) != *number ||
        *number < std::numeric_limits<int>::min() ||
        *number > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(*number);
    return true;
}

bool apply_content_changes(const std::string &original, const Json::Array &changes,
                           std::string &updated, std::string &error) {
    updated = original;
    for (const Json &change : changes) {
        const Json &range_value = change.get("range");
        const Json &text_value = change.get("text");
        const auto *replacement = std::get_if<std::string>(&text_value.data);
        if (!replacement) {
            error = "text change has no string text";
            return false;
        }
        if (range_value.is_null()) {
            updated = *replacement;
            continue;
        }
        const Json &start = range_value.get("start");
        const Json &end = range_value.get("end");
        int start_line = 0, start_character = 0, end_line = 0, end_character = 0;
        size_t first = 0, last = 0;
        if (!exact_integer(start.get("line"), start_line) ||
            !exact_integer(start.get("character"), start_character) ||
            !exact_integer(end.get("line"), end_line) ||
            !exact_integer(end.get("character"), end_character) ||
            !position_offset(updated, start_line, start_character, first) ||
            !position_offset(updated, end_line, end_character, last) ||
            last < first) {
            error = "text change has an invalid UTF-16 range";
            return false;
        }
        const Json &range_length = change.get("rangeLength");
        if (!range_length.is_null()) {
            int expected = -1;
            if (!exact_integer(range_length, expected) || expected < 0 ||
                utf16_length(updated.substr(first, last - first)) != expected) {
                error = "text change rangeLength does not match its range";
                return false;
            }
        }
        updated.replace(first, last - first, *replacement);
    }
    return true;
}

size_t utf16_offset(const std::string &s, int units) {
    size_t i = 0;
    while (i < s.size() && units > 0) {
        const size_t old = i;
        units -= codepoint(s, i) > 0xffff ? 2 : 1;
        if (units < 0) return old;
    }
    return i;
}

bool ident(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u == '_';
}

struct Word {
    std::string name;
    std::string qualifier;
    int start = 0;
    int end = 0;
};

Word word_at(const std::string &text, int line_no, int character) {
    const auto lines = lines_of(text);
    if (line_no < 0 || static_cast<size_t>(line_no) >= lines.size()) return {};
    const std::string &line = lines[static_cast<size_t>(line_no)];
    size_t at = utf16_offset(line, character);
    if (at > 0 && (at == line.size() || !ident(line[at]))) --at;
    size_t start = at, end = at;
    while (start > 0 && ident(line[start - 1])) --start;
    while (end < line.size() && ident(line[end])) ++end;
    if (start == end) return {};
    Word result{line.substr(start, end - start), {}, utf16_length(line.substr(0, start)),
                utf16_length(line.substr(0, end))};
    if (start >= 2 && line[start - 1] == '.') {
        size_t q = start - 1;
        while (q > 0 && ident(line[q - 1])) --q;
        result.qualifier = line.substr(q, start - 1 - q);
    }
    return result;
}

std::string path_from_uri(const std::string &uri) {
    std::string path = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            char *end = nullptr;
            const std::string hex = path.substr(i + 1, 2);
            long value = std::strtol(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 2) {
                decoded.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }
    return decoded;
}

std::string classify(const std::string &m) {
    if (m.find("use of moved") != std::string::npos) return "E0382";
    if (m.find("cannot move borrowed parameter") != std::string::npos) return "E0507";
    if (m.find("while borrowed") != std::string::npos ||
        m.find("cannot pass") != std::string::npos ||
        m.find("cannot use") != std::string::npos ||
        m.find("cannot assign") != std::string::npos) return "E0505";
    if (m.find("double mut borrow") != std::string::npos ||
        m.find("law of exclusivity") != std::string::npos) return "E0499";
    if (m.find("escapes via return") != std::string::npos) return "E0515";
    if (m.find("escapes via spawn") != std::string::npos ||
        m.find("return escapes spawn") != std::string::npos) return "B0521";
    if (m.find("assign to immutable") != std::string::npos ||
        m.find("cannot mutate immutable") != std::string::npos ||
        m.find("pass immutable") != std::string::npos ||
        m.find("assign to const") != std::string::npos ||
        m.find("write through") != std::string::npos) return "E0596";
    if (m.find("unsupported FFI type") != std::string::npos) return "F0001";
    if (m.find("use of undeclared var") != std::string::npos) return "B0101";
    if (m.find("unused var") != std::string::npos) return "W0001";
    if (m.find("unused mut") != std::string::npos) return "W0002";
    if (m.find("unused import") != std::string::npos) return "W0003";
    if (m.find("looks cut short") != std::string::npos) return "W0004";
    return "B0000";
}

struct Diagnostic {
    int line = 0;
    std::string severity;
    std::string code;
    std::string message;
    std::optional<BorrowNote> note;
};

struct DocState {
    std::string text;
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, std::vector<ParamMode>> borrow_extra;
    Sigs type_extra;
    std::vector<int> function_lines;
    std::vector<int> import_lines;
    std::optional<int> version;
    bool incremental = false;
};

void sort_diagnostics(std::vector<Diagnostic> &out) {
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic &a, const Diagnostic &b) {
        if (a.line != b.line) return a.line < b.line;
        return (a.severity != "error") < (b.severity != "error");
    });
}

void append_check_results(std::vector<Diagnostic> &out, const ParseOut &parsed,
                          const std::map<std::string, std::vector<ParamMode>> &borrow_extra,
                          const Sigs &type_extra) {
    BorrowOut borrow = borrow_check(parsed.body, borrow_extra, parsed.stray);
    for (const BorrowDiag &d : borrow.errs)
        out.push_back({d.line, "error", classify(d.msg), d.msg, d.note});
    for (const BorrowDiag &d : borrow.warns)
        out.push_back({d.line, "warning", classify(d.msg), d.msg, d.note});
    for (const TypeErr &e : type_check(parsed.body, type_extra))
        out.push_back({e.line, "error", "T0001", e.msg, std::nullopt});
}

bool root_is_function_file(const std::vector<StmtP> &body) {
    for (const auto &ptr : body) {
        if (!ptr) return false;
        const Stmt *node = ptr.get();
        if (node->k == SK::Import) continue;
        if (node->k == SK::Pub && !node->body.empty()) node = node->body.front().get();
        if (node->k != SK::Fn) return false;
    }
    return true;
}

void collect_root_modes(const std::vector<StmtP> &body,
                        std::map<std::string, std::vector<ParamMode>> &modes) {
    for (const auto &ptr : body) {
        const Stmt *node = ptr.get();
        if (node->k == SK::Pub && !node->body.empty()) node = node->body.front().get();
        if (node->k != SK::Fn && node->k != SK::ExportFn) continue;
        for (const FnDecl &fn : node->fns) {
            std::vector<ParamMode> params;
            for (const Param &param : fn.params)
                params.push_back({param.name, param.mode});
            modes[fn.name.value_or("")] = std::move(params);
        }
    }
}

void analyze_document(const std::string &path, DocState &state) {
    std::vector<Diagnostic> out;
    ParseOut parsed;
    try {
        parsed = parse(state.text);
    } catch (const ParseError &e) {
        auto source_lines = lines_of(state.text);
        while (source_lines.size() > 1 && source_lines.back().empty()) source_lines.pop_back();
        const int line = std::min(static_cast<int>(e.line),
                                  static_cast<int>(source_lines.size()));
        out.push_back({line, "error", "P0001", e.what(), std::nullopt});
        state.diagnostics = std::move(out);
        state.incremental = false;
        return;
    } catch (const std::exception &e) {
        out.push_back({0, "error", "P0001", std::string("parse failed: ") + e.what(),
                       std::nullopt});
        state.diagnostics = std::move(out);
        state.incremental = false;
        return;
    }

    state.borrow_extra.clear();
    state.type_extra.clear();
    state.function_lines.clear();
    state.import_lines.clear();
    std::vector<SourceFile> imports;
    if (std::filesystem::exists(path)) {
        try {
            ModuleSet modules = load_modules(path, state.text);
            for (size_t i = 1; i < modules.files.size(); ++i)
                imports.push_back(std::move(modules.files[i]));
        } catch (const std::exception &e) {
            out.push_back({0, "error", "B0000",
                           std::string("module load failed: ") + e.what(), std::nullopt});
            state.diagnostics = std::move(out);
            state.incremental = false;
            return;
        }
    }
    for (const SourceFile &file : imports) {
        for (auto &[name, modes] : pub_sigs(file.body).sigs)
            state.borrow_extra[name] = modes;
        collect_sigs(file.body, state.type_extra);
    }

    Sigs root_sigs;
    collect_sigs(parsed.body, root_sigs);
    for (auto &[name, sig] : root_sigs) state.type_extra[name] = std::move(sig);
    collect_root_modes(parsed.body, state.borrow_extra);
    if (root_is_function_file(parsed.body)) {
        for (const auto &ptr : parsed.body) {
            if (ptr->k == SK::Import) state.import_lines.push_back(ptr->line);
            else state.function_lines.push_back(ptr->line);
        }
    }
    append_check_results(out, parsed, state.borrow_extra, state.type_extra);
    sort_diagnostics(out);
    state.diagnostics = std::move(out);
    state.incremental = root_is_function_file(parsed.body);
}

size_t line_offset(const std::string &text, int line) {
    size_t offset = 0;
    for (int n = 1; n < line && offset < text.size(); ++n) {
        size_t end = text.find('\n', offset);
        if (end == std::string::npos) return text.size();
        offset = end + 1;
    }
    return offset;
}

size_t function_end(const std::string &text, size_t start) {
    bool in_string = false, escaped = false, in_comment = false;
    int depth = 0;
    bool opened = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_comment) {
            if (c == '\n') in_comment = false;
            continue;
        }
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            in_comment = true;
            ++i;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            opened = true;
            ++depth;
        } else if (c == '}' && opened && --depth == 0) {
            return i + 1;
        }
    }
    return std::string::npos;
}

size_t header_end(const std::string &text, size_t start, size_t end) {
    bool in_string = false, escaped = false, in_comment = false;
    for (size_t i = start; i < end; ++i) {
        const char c = text[i];
        if (in_comment) {
            if (c == '\n') in_comment = false;
        } else if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '/' && i + 1 < end && text[i + 1] == '/') {
            in_comment = true;
            ++i;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            return i;
        }
    }
    return end;
}

bool incremental_analyze(const std::string &text, DocState &state) {
    if (!state.incremental || text == state.text) return false;
    size_t prefix = 0;
    while (prefix < text.size() && prefix < state.text.size() &&
           text[prefix] == state.text[prefix]) ++prefix;
    size_t old_end = state.text.size(), new_end = text.size();
    while (old_end > prefix && new_end > prefix &&
           state.text[old_end - 1] == text[new_end - 1]) {
        --old_end;
        --new_end;
    }

    const int changed_line = static_cast<int>(
        std::count(state.text.begin(), state.text.begin() + static_cast<std::ptrdiff_t>(prefix),
                   '\n')) + 1;
    size_t old_start = 0, old_finish = 0, new_finish = 0;
    bool found = false;
    int function_line = 0;
    for (int line : state.function_lines) {
        if (line > changed_line) break;
        const size_t start = line_offset(state.text, line);
        const size_t finish = function_end(state.text, start);
        if (finish == std::string::npos || prefix > finish || old_end > finish) continue;
        const size_t next_finish = function_end(text, start);
        if (next_finish == std::string::npos || new_end > next_finish) continue;
        old_start = start;
        old_finish = finish;
        new_finish = next_finish;
        function_line = line;
        found = true;
        break;
    }
    if (!found || state.text.substr(0, old_start) != text.substr(0, old_start) ||
        state.text.substr(old_finish) != text.substr(new_finish)) return false;
    const size_t old_header_end = header_end(state.text, old_start, old_finish);
    const size_t new_header_end = header_end(text, old_start, new_finish);
    if (state.text.substr(old_start, old_header_end - old_start) !=
        text.substr(old_start, new_header_end - old_start)) return false;
    const int old_last_line = static_cast<int>(
        std::count(state.text.begin(), state.text.begin() +
                       static_cast<std::ptrdiff_t>(old_finish), '\n')) + 1;
    const int new_last_line = static_cast<int>(
        std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(new_finish),
                   '\n')) + 1;
    const int line_delta = new_last_line - old_last_line;
    auto update_line_metadata = [&] {
        if (line_delta == 0) return;
        for (int &line : state.function_lines)
            if (line > function_line) line += line_delta;
        for (int &line : state.import_lines)
            if (line > function_line) line += line_delta;
    };
    auto outside_function = [&](const Diagnostic &d) {
        return d.line < function_line || d.line > old_last_line;
    };
    auto shift_following_diagnostics = [&](std::vector<Diagnostic> &diagnostics) {
        if (line_delta == 0) return;
        for (Diagnostic &d : diagnostics)
            if (d.line > old_last_line) d.line += line_delta;
        for (Diagnostic &d : diagnostics) {
            if (d.note && d.note->line > old_last_line)
                d.note->line += line_delta;
        }
    };

    std::string filtered(text.size(), ' ');
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n' || text[i] == '\r') filtered[i] = text[i];
    std::vector<std::string> source_lines = lines_of(text);
    for (int import_line : state.import_lines) {
        const size_t start = line_offset(text, import_line);
        const size_t end = text.find('\n', start);
        const size_t length = (end == std::string::npos ? text.size() : end) - start;
        if (start + length <= filtered.size())
            filtered.replace(start, length, text, start, length);
    }
    filtered.replace(old_start, new_finish - old_start, text, old_start,
                     new_finish - old_start);
    ParseOut parsed;
    try {
        parsed = parse(filtered);
    } catch (const ParseError &e) {
        std::vector<Diagnostic> updated;
        for (const Diagnostic &d : state.diagnostics)
            if (outside_function(d)) updated.push_back(d);
        shift_following_diagnostics(updated);
        updated.push_back({std::min(static_cast<int>(e.line),
                                    static_cast<int>(source_lines.size())),
                           "error", "P0001", e.what(), std::nullopt});
        sort_diagnostics(updated);
        state.diagnostics = std::move(updated);
        state.text = text;
        update_line_metadata();
        return true;
    } catch (const std::exception &) {
        return false;
    }
    bool target_found = false;
    for (const auto &ptr : parsed.body) {
        const Stmt *node = ptr.get();
        if (node->k == SK::Pub && !node->body.empty()) node = node->body.front().get();
        if (node->k == SK::Fn && node->line == function_line) target_found = true;
    }
    if (!target_found) return false;

    std::vector<Diagnostic> changed;
    try {
        append_check_results(changed, parsed, state.borrow_extra, state.type_extra);
    } catch (const std::exception &) {
        return false;
    }
    sort_diagnostics(changed);
    std::vector<Diagnostic> updated;
    for (const Diagnostic &d : state.diagnostics)
        if (outside_function(d)) updated.push_back(d);
    shift_following_diagnostics(updated);
    for (const Diagnostic &d : changed)
        if (d.line >= function_line && d.line <= new_last_line) updated.push_back(d);
    sort_diagnostics(updated);
    state.diagnostics = std::move(updated);
    state.text = text;
    update_line_metadata();
    return true;
}

Json diag_json(const Diagnostic &d, const std::vector<std::string> &lines) {
    const int ln = std::max(0, d.line - 1);
    const std::string source = static_cast<size_t>(ln) < lines.size() ? lines[ln] : "";
    Obj out{{"range", range(ln, 0, utf16_length(source))},
            {"severity", d.severity == "error" ? 1 : d.severity == "warning" ? 2 : 3},
            {"code", d.code}, {"source", "brat"}, {"message", d.message}};
    if (d.note) {
        const int note_line = std::max(0, d.note->line - 1);
        out["relatedInformation"] = arr({obj({
            {"location", obj({{"uri", ""}, {"range", range(note_line, 0, 0)}})},
            {"message", d.note->msg}})});
    }
    return obj(std::move(out));
}

struct Decl {
    std::string name;
    int line = 0;
    std::string container;
};

void collect_decls(const std::vector<StmtP> &body, std::vector<Decl> &out);

void collect_decl(const Stmt &s, std::vector<Decl> &out) {
    if (s.k == SK::Fn || s.k == SK::ExportFn) {
        for (const FnDecl &fn : s.fns) {
            if (fn.name) out.push_back({*fn.name, s.line, ""});
            for (const Param &p : fn.params)
                out.push_back({p.name, s.line, fn.name.value_or("")});
            collect_decls(fn.body, out);
        }
    } else if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::ForRange ||
               s.k == SK::ForIter) {
        out.push_back({s.a, s.line, ""});
    }
    collect_decls(s.body, out);
    collect_decls(s.body2, out);
    if (s.els) collect_decl(*s.els, out);
    for (const FnDecl &fn : s.fns) collect_decls(fn.body, out);
}

void collect_decls(const std::vector<StmtP> &body, std::vector<Decl> &out) {
    for (const auto &ptr : body)
        if (ptr) collect_decl(*ptr, out);
}

Json document_symbols(const std::string &text) {
    ParseOut parsed;
    try { parsed = parse(text); } catch (const std::exception &) { return arr({}); }
    const auto lines = lines_of(text);
    Arr result;
    for (const auto &ptr : parsed.body) {
        const Stmt *s = ptr.get();
        if (s->k == SK::Pub && !s->body.empty()) s = s->body.front().get();
        if (s->k != SK::Fn && s->k != SK::ExportFn) continue;
        for (const FnDecl &fn : s->fns) {
            if (!fn.name) continue;
            const int line = std::max(0, s->line - 1);
            const std::string src = static_cast<size_t>(line) < lines.size() ? lines[line] : "";
            size_t byte_col = src.find(*fn.name);
            if (byte_col == std::string::npos) byte_col = 0;
            const int col = utf16_length(src.substr(0, byte_col));
            const int end = col + utf16_length(*fn.name);
            result.push_back(obj({
                {"name", *fn.name}, {"kind", 12},
                {"range", range(line, 0, utf16_length(src))},
                {"selectionRange", range(line, col, end)}}));
        }
    }
    return arr(std::move(result));
}

Json definition(const std::string &path, const std::string &text, int line, int character) {
    const Word word = word_at(text, line, character);
    if (word.name.empty()) return Json(nullptr);
    ParseOut parsed;
    try { parsed = parse(text); } catch (const std::exception &) { return Json(nullptr); }
    std::vector<Decl> decls;
    collect_decls(parsed.body, decls);
    std::optional<Decl> best;
    for (const Decl &d : decls)
        if (d.name == word.name && d.line - 1 <= line && (!best || d.line > best->line))
            best = d;
    std::string target = path;
    if (!best && !word.qualifier.empty()) {
        try {
            ModuleSet modules = load_modules(path, text);
            for (const SourceFile &file : modules.files) {
                if (!file.ns) continue;
                const std::string ns = *file.ns;
                if (ns != word.qualifier && ns.substr(ns.rfind("__") == std::string::npos
                        ? 0 : ns.rfind("__") + 2) != word.qualifier) continue;
                std::vector<Decl> candidate;
                collect_decls(file.body, candidate);
                bool exported = false;
                for (const auto &stmt : file.body) {
                    if (!stmt || stmt->k != SK::Pub || stmt->body.empty()) continue;
                    const Stmt &inner = *stmt->body.front();
                    if (inner.k == SK::Fn && !inner.fns.empty() &&
                        inner.fns.front().name == word.name) exported = true;
                }
                if (!exported) continue;
                for (const Decl &d : candidate)
                    if (d.name == word.name) {
                        best = d;
                        target = file.path;
                        break;
                    }
                if (best) break;
            }
        } catch (const std::exception &) {
            return Json(nullptr);
        }
    }
    if (!best) return Json(nullptr);
    std::string source = text;
    if (target != path) {
        std::ifstream in(target, std::ios::binary);
        if (!in) return Json(nullptr);
        source.assign(std::istreambuf_iterator<char>(in), {});
    }
    const auto lines = lines_of(source);
    const int target_line = std::max(0, best->line - 1);
    if (static_cast<size_t>(target_line) >= lines.size()) return Json(nullptr);
    const size_t byte_col = lines[target_line].find(best->name);
    const int col = byte_col == std::string::npos
        ? 0 : utf16_length(lines[target_line].substr(0, byte_col));
    return obj({{"uri", "file://" + target},
                {"range", range(target_line, col, col + utf16_length(best->name))}});
}

class Server {
public:
    int run() {
        while (true) {
            auto message = read_message();
            if (!message) return 0;
            try {
                if (!handle(*message)) return 0;
            } catch (const std::exception &e) {
                const Json &id = message->get("id");
                if (!id.is_null())
                    send(obj({{"jsonrpc", "2.0"}, {"id", id},
                              {"error", obj({{"code", -32603}, {"message", e.what()}})}}));
                else
                    std::fprintf(stderr, "brat-lsp: notification failed: %s\n", e.what());
            }
        }
    }

private:
    std::map<std::string, DocState> docs_;

    void send(const Json &json) {
        const std::string body = json_write(json);
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        std::cout.flush();
    }

    std::optional<Json> read_message() {
        std::string line;
        size_t length = 0;
        bool have_length = false;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "brat-lsp: malformed message header\n");
                return std::nullopt;
            }
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name != "content-length") continue;
            if (have_length) {
                std::fprintf(stderr, "brat-lsp: duplicate Content-Length header\n");
                return std::nullopt;
            }
            std::string value = line.substr(colon + 1);
            const size_t first = value.find_first_not_of(" \t");
            const size_t last = value.find_last_not_of(" \t");
            if (first == std::string::npos) {
                std::fprintf(stderr, "brat-lsp: invalid Content-Length header\n");
                return std::nullopt;
            }
            value = value.substr(first, last - first + 1);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                length == 0 || length > 64 * 1024 * 1024) {
                std::fprintf(stderr, "brat-lsp: invalid Content-Length header\n");
                return std::nullopt;
            }
            have_length = true;
        }
        if (!std::cin || !have_length) {
            if (std::cin.eof() && !std::cin.bad()) return std::nullopt;
            std::fprintf(stderr, "brat-lsp: missing Content-Length header\n");
            return std::nullopt;
        }
        std::string body(length, '\0');
        std::cin.read(body.data(), static_cast<std::streamsize>(length));
        if (static_cast<size_t>(std::cin.gcount()) != length) {
            std::fprintf(stderr, "brat-lsp: truncated JSON-RPC body\n");
            return std::nullopt;
        }
        try {
            return json_parse(body);
        } catch (const std::exception &e) {
            send(obj({{"jsonrpc", "2.0"}, {"id", Json(nullptr)},
                      {"error", obj({{"code", -32700},
                                     {"message", std::string("parse error: ") + e.what()}})}}));
            return Json(nullptr);
        }
    }

    void publish(const std::string &uri) {
        const DocState &state = docs_[uri];
        const std::string &text = state.text;
        const auto lines = lines_of(text);
        Arr diagnostics;
        for (const Diagnostic &d : state.diagnostics) {
            Json value = diag_json(d, lines);
            if (d.note) {
                Obj item = std::get<Obj>(value.data);
                Obj related = std::get<Obj>(item["relatedInformation"].array()[0].data);
                Obj location = std::get<Obj>(related["location"].data);
                location["uri"] = uri;
                related["location"] = obj(std::move(location));
                item["relatedInformation"] = arr({obj(std::move(related))});
                value = obj(std::move(item));
            }
            diagnostics.push_back(std::move(value));
        }
        send(obj({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
                  {"params", obj({{"uri", uri}, {"diagnostics", arr(std::move(diagnostics))}})}}));
    }

    bool handle(const Json &message) {
        const std::string method = message.get("method").string();
        const Json id = message.get("id");
        const Json &params = message.get("params");
        if (method == "initialize") {
            send(obj({{"jsonrpc", "2.0"}, {"id", id},
                {"result", obj({
                    {"capabilities", obj({
                        {"textDocumentSync", obj({{"openClose", true}, {"change", 2}})},
                        {"documentSymbolProvider", true}, {"definitionProvider", true}})},
                    {"serverInfo", obj({{"name", "brat-lsp"}, {"version", "0.1"}})}})}}));
        } else if (method == "shutdown") {
            send(obj({{"jsonrpc", "2.0"}, {"id", id}, {"result", Json(nullptr)}}));
        } else if (method == "exit") {
            return false;
        } else if (method == "textDocument/didOpen") {
            const Json &document = params.get("textDocument");
            const std::string uri = document.get("uri").string();
            DocState state;
            state.text = document.get("text").string();
            const int version = document.get("version").integer(-1);
            if (version >= 0) state.version = version;
            analyze_document(path_from_uri(uri), state);
            docs_[uri] = std::move(state);
            publish(uri);
        } else if (method == "textDocument/didChange") {
            const Json &document = params.get("textDocument");
            const std::string uri = document.get("uri").string();
            const auto &changes = params.get("contentChanges").array();
            if (!changes.empty()) {
                const int version = document.get("version").integer(-1);
                auto current = docs_.find(uri);
                if (current != docs_.end() && version >= 0 && current->second.version &&
                    version <= *current->second.version) return true;
                const std::string previous = current == docs_.end() ? "" : current->second.text;
                std::string text, error;
                if (!apply_content_changes(previous, changes, text, error)) {
                    send(obj({{"jsonrpc", "2.0"}, {"method", "window/logMessage"},
                              {"params", obj({{"type", 1},
                                              {"message", "brat-lsp: " + error}})}}));
                    return true;
                }
                auto it = current;
                if (it == docs_.end()) {
                    DocState state;
                    state.text = text;
                    if (version >= 0) state.version = version;
                    analyze_document(path_from_uri(uri), state);
                    docs_[uri] = std::move(state);
                } else if (!incremental_analyze(text, it->second)) {
                    it->second.text = text;
                    if (version >= 0) it->second.version = version;
                    analyze_document(path_from_uri(uri), it->second);
                } else if (version >= 0) {
                    it->second.version = version;
                }
                publish(uri);
            }
        } else if (method == "textDocument/didClose") {
            const std::string uri = params.get("textDocument").get("uri").string();
            docs_.erase(uri);
            send(obj({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
                      {"params", obj({{"uri", uri}, {"diagnostics", arr({})}})}}));
        } else if (method == "textDocument/documentSymbol") {
            const std::string uri = params.get("textDocument").get("uri").string();
            Json result = document_symbols(docs_[uri].text);
            send(obj({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}));
        } else if (method == "textDocument/definition") {
            const std::string uri = params.get("textDocument").get("uri").string();
            const Json &position = params.get("position");
            Json result = definition(path_from_uri(uri), docs_[uri].text,
                                     position.get("line").integer(),
                                     position.get("character").integer());
            send(obj({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}));
        } else if (!id.is_null() && !method.empty()) {
            send(obj({{"jsonrpc", "2.0"}, {"id", id},
                      {"error", obj({{"code", -32601},
                                     {"message", "unknown method `" + method + "`"}})}}));
        }
        return true;
    }
};

}  // namespace

int run_lsp() { return Server().run(); }
