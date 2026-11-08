#include "lsp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "borrow.h"
#include "check.h"
#include "lexer.h"
#include "lsp_wire.h"
#include "module.h"
#include "parser.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using lsp_wire::FrameStatus;
using lsp_wire::JsonWriter;
using lsp_wire::LineIndex;
using lsp_wire::LspFrameReader;
using lsp_wire::LspJsonTape;

// Fast 64-bit FNV-1a fingerprint for Salsa-style signature & text invalidation.
inline uint64_t fnv1a64(std::string_view sv, uint64_t h = 14695981039346656037ULL) {
    for (unsigned char c : sv) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

inline bool is_ident_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u == '_';
}

inline bool is_ident_start(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

bool valid_brat_ident(std::string_view s) {
    if (s.empty() || is_keyword(s) || !is_ident_start(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (!is_ident_char(s[i])) return false;
    return true;
}

std::string path_from_uri(std::string_view uri) {
    if (uri.find('\0') != std::string_view::npos) return {};
    std::string_view path = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            auto hex_val = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int hi = hex_val(path[i + 1]);
            const int lo = hex_val(path[i + 2]);
            if (hi >= 0 && lo >= 0) {
                const int byte = (hi << 4) | lo;
                if (byte == 0) return {};
                decoded.push_back(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }
#if defined(_WIN32)
    if (decoded.size() >= 3 && decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
#endif
    return decoded;
}

std::string dots_to_ns(std::string_view dotted) {
    std::string out;
    out.reserve(dotted.size() + 4);
    for (char c : dotted) {
        if (c == '.') out += "__";
        else out.push_back(c);
    }
    return out;
}

struct Word {
    std::string name;
    std::string qualifier;
    std::string full_qualifier;
    int start = 0;
    int end = 0;
};

Word word_at(std::string_view text, const LineIndex &lines, int line_no, int character) {
    if (line_no < 0 || static_cast<size_t>(line_no) >= lines.line_count()) return {};
    const std::string_view line = lines.line_view(text, line_no);
    size_t at = lines.clamp_col_in_line(text, line_no, character);
    if (at > 0 && (at == line.size() || !is_ident_char(line[at]))) --at;
    size_t start = at, end = at;
    while (start > 0 && is_ident_char(line[start - 1])) --start;
    while (end < line.size() && is_ident_char(line[end])) ++end;
    if (start == end) return {};
    Word res;
    res.name.assign(line.data() + start, end - start);
    res.start = lines.byte_to_utf16_col(text, line_no, start);
    res.end = lines.byte_to_utf16_col(text, line_no, end);
    if (start >= 2 && line[start - 1] == '.') {
        size_t q = start - 1;
        while (q > 0 && is_ident_char(line[q - 1])) --q;
        res.qualifier.assign(line.data() + q, start - 1 - q);
        size_t fq = q;
        while (fq >= 2 && line[fq - 1] == '.' && is_ident_char(line[fq - 2])) {
            --fq;
            while (fq > 0 && is_ident_char(line[fq - 1])) --fq;
        }
        res.full_qualifier.assign(line.data() + fq, start - 1 - fq);
    }
    return res;
}

bool ns_matches_word(std::string_view ns, const Word &word) {
    if (!word.full_qualifier.empty() && ns == dots_to_ns(word.full_qualifier)) return true;
    if (ns == word.qualifier) return true;
    const size_t tail = ns.rfind("__");
    return tail != std::string_view::npos && ns.substr(tail + 2) == word.qualifier;
}

std::string classify(std::string_view m) {
    if (m.find("use of moved") != std::string_view::npos) return "E0382";
    if (m.find("cannot move borrowed parameter") != std::string_view::npos) return "E0507";
    if (m.find("while borrowed") != std::string_view::npos ||
        m.find("cannot move borrowed pointer") != std::string_view::npos ||
        m.find("cannot pass") != std::string_view::npos ||
        m.find("cannot use") != std::string_view::npos ||
        m.find("cannot assign") != std::string_view::npos) return "E0505";
    if (m.find("while iterating it") != std::string_view::npos) return "E0502";
    if (m.find("double mut borrow") != std::string_view::npos ||
        m.find("law of exclusivity") != std::string_view::npos) return "E0499";
    if (m.find("escapes via return") != std::string_view::npos) return "E0515";
    if (m.find("escapes via spawn") != std::string_view::npos ||
        m.find("return escapes spawn") != std::string_view::npos) return "B0521";
    if (m.find("inside `spawn`") != std::string_view::npos ||
        m.find("spawn` launched in a loop") != std::string_view::npos) return "B0522";
    if (m.find("is from library") != std::string_view::npos ||
        m.find("is closed twice") != std::string_view::npos ||
        m.find("is closed on line") != std::string_view::npos) return "B0530";
    if (m.find("the signature says") != std::string_view::npos ||
        m.find("the signature has") != std::string_view::npos) return "B0531";
    if (m.find("assign to immutable") != std::string_view::npos ||
        m.find("cannot mutate immutable") != std::string_view::npos ||
        m.find("pass immutable") != std::string_view::npos ||
        m.find("assign to const") != std::string_view::npos ||
        m.find("write through") != std::string_view::npos) return "E0596";
    if (m.find("unsupported FFI type") != std::string_view::npos) return "F0001";
    if (m.find("use of undeclared var") != std::string_view::npos) return "B0101";
    if (m.find("unused var") != std::string_view::npos) return "W0001";
    if (m.find("unused mut") != std::string_view::npos) return "W0002";
    if (m.find("unused import") != std::string_view::npos) return "W0003";
    if (m.find("looks cut short") != std::string_view::npos) return "W0004";
    return "B0000";
}

struct Diagnostic {
    int line = 0;
    uint8_t severity = 1;  // 1=error, 2=warning, 3=info
    std::string code;
    std::string message;
    std::optional<BorrowNote> note;
};

void sort_diagnostics(std::vector<Diagnostic> &out) {
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic &a, const Diagnostic &b) {
        if (a.line != b.line) return a.line < b.line;
        return a.severity < b.severity;
    });
}

// Extract leading `///` or contiguous `//` doc comments immediately preceding 1-based `decl_line`.
std::string extract_doc_comment(std::string_view text, const LineIndex &lines, int decl_line) {
    int ln = decl_line - 2;  // 0-based line above decl
    std::vector<std::string_view> rev;
    while (ln >= 0 && static_cast<size_t>(ln) < lines.line_count()) {
        std::string_view sv = lines.line_view(text, ln);
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
        if (sv.rfind("///", 0) == 0) {
            sv.remove_prefix(3);
            if (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
            rev.push_back(sv);
            --ln;
        } else if (sv.rfind("//", 0) == 0) {
            sv.remove_prefix(2);
            if (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
            rev.push_back(sv);
            --ln;
        } else {
            break;
        }
    }
    if (rev.empty()) return {};
    std::string out;
    for (size_t i = rev.size(); i-- > 0;) {
        if (!out.empty()) out.push_back('\n');
        out.append(rev[i].data(), rev[i].size());
    }
    return out;
}

struct DocSymbol {
    std::string name;
    int kind = 12;          // LSP SymbolKind (12=Function, 10=Enum, 23=Struct, 11=Interface, 6=Method, 14=Constant)
    int line = 0;           // 0-based
    int line_end_col = 0;   // UTF-16 length of full line
    int sel_start = 0;      // UTF-16 start col
    int sel_end = 0;        // UTF-16 end col
    bool is_pub = false;
    std::string lower_name;
};

struct Decl {
    std::string name;
    int line = 0;  // 1-based
    std::string container;
    bool is_pub = false;
};

void collect_decls(const std::vector<StmtP> &body, std::vector<Decl> &out, bool top_pub = false);

void collect_decl(const Stmt &s, std::vector<Decl> &out, bool is_pub) {
    if (s.k == SK::Pub && !s.body.empty()) {
        collect_decl(*s.body.front(), out, true);
        return;
    }
    if (s.k == SK::Fn || s.k == SK::ExportFn) {
        for (const FnDecl &fn : s.fns) {
            if (fn.name) out.push_back({*fn.name, s.line, "", is_pub || s.k == SK::ExportFn});
            for (const Param &p : fn.params)
                out.push_back({p.name, s.line, fn.name.value_or(""), false});
            collect_decls(fn.body, out, false);
        }
    } else if (s.k == SK::Impl) {
        for (const FnDecl &fn : s.fns) {
            if (fn.name) out.push_back({*fn.name, s.line, s.a, is_pub});
            for (const Param &p : fn.params)
                out.push_back({p.name, s.line, fn.name.value_or(""), false});
            collect_decls(fn.body, out, false);
        }
    } else if (s.k == SK::Extern) {
        out.push_back({s.ext.name, s.line, "extern", true});
    } else if (s.k == SK::ExternStruct || s.k == SK::Struct || s.k == SK::Enum || s.k == SK::Trait) {
        out.push_back({s.a, s.line, "type", is_pub});
        if (s.k == SK::Enum) {
            for (const auto &v : s.variants)
                out.push_back({v.name, s.line, "enum " + s.a, is_pub});
        }
    } else if (s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::ForRange ||
               s.k == SK::ForIter) {
        out.push_back({s.a, s.line, "", is_pub});
    }
    collect_decls(s.body, out, false);
    collect_decls(s.body2, out, false);
    if (s.els) collect_decl(*s.els, out, false);
}

void collect_decls(const std::vector<StmtP> &body, std::vector<Decl> &out, bool top_pub) {
    for (const auto &ptr : body)
        if (ptr) collect_decl(*ptr, out, top_pub);
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        out[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
    }
    return out;
}

void extract_doc_symbols(std::string_view text, const LineIndex &lines,
                         const std::vector<StmtP> &body, std::vector<DocSymbol> &out) {
    out.clear();
    auto push_sym = [&](const std::string &nm, int kind, int line1, bool is_pub) {
        if (nm.empty()) return;
        const int ln = std::max(0, line1 - 1);
        const std::string_view src = lines.line_view(text, ln);
        size_t bcol = src.find(nm);
        if (bcol == std::string_view::npos) bcol = 0;
        const int scol = lines.byte_to_utf16_col(text, ln, bcol);
        const int ecol = scol + LineIndex::utf16_len(nm, false);
        const int lmax = lines.line_utf16_len(text, ln);
        out.push_back({nm, kind, ln, lmax, scol, ecol, is_pub, to_lower_ascii(nm)});
    };

    for (const auto &ptr : body) {
        if (!ptr) continue;
        bool is_pub = (ptr->k == SK::Pub && !ptr->body.empty());
        const Stmt *s = is_pub ? ptr->body.front().get() : ptr.get();
        if (!s) continue;
        if (s->k == SK::Fn || s->k == SK::ExportFn) {
            for (const FnDecl &fn : s->fns)
                if (fn.name) push_sym(*fn.name, 12, s->line, is_pub || s->k == SK::ExportFn);
        } else if (s->k == SK::Extern) {
            push_sym(s->ext.name, 12, s->line, true);
        } else if (s->k == SK::ExternStruct || s->k == SK::Struct) {
            push_sym(s->a, 23, s->line, is_pub);
        } else if (s->k == SK::Enum) {
            push_sym(s->a, 10, s->line, is_pub);
        } else if (s->k == SK::Trait) {
            push_sym(s->a, 11, s->line, is_pub);
        } else if (s->k == SK::Impl) {
            for (const FnDecl &fn : s->fns)
                if (fn.name) push_sym(*fn.name, 6, s->line, is_pub);
        } else if ((s->k == SK::Let || s->k == SK::LetTmp) && s->konst) {
            push_sym(s->a, 14, s->line, is_pub);
        }
    }
}

uint64_t compute_signature_hash(const std::vector<StmtP> &body) {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](std::string_view sv) { h = fnv1a64(sv, fnv1a64("|", h)); };
    for (const auto &ptr : body) {
        if (!ptr) continue;
        const bool is_pub = (ptr->k == SK::Pub && !ptr->body.empty());
        const Stmt *s = is_pub ? ptr->body.front().get() : ptr.get();
        if (!s) continue;
        if (is_pub) mix("pub");
        if (s->k == SK::Fn || s->k == SK::ExportFn) {
            mix(s->k == SK::ExportFn ? "exportfn" : "fn");
            for (const FnDecl &fn : s->fns) {
                mix(fn.name.value_or(""));
                mix(fn.ret.value_or(""));
                for (const Param &p : fn.params) {
                    mix(p.name);
                    mix(p.mode == Mode::Mut ? "mut" : (p.mode == Mode::Move ? "move" : "ref"));
                    mix(p.ann.value_or(""));
                }
            }
        } else if (s->k == SK::Extern) {
            mix("extern");
            mix(s->ext.name);
            mix(s->ext.ret.value_or(""));
            for (const Param &p : s->ext.params) mix(p.ann.value_or(""));
            for (const std::string &ef : s->ext.effects) mix(ef);
        } else if (s->k == SK::ExternStruct || s->k == SK::Struct) {
            mix(s->k == SK::Struct ? "nativestruct" : s->opt1.value_or("struct"));
            mix(s->a);
            for (const auto &f : s->fields) {
                mix(f.name);
                mix(f.ann.value_or(""));
            }
        } else if (s->k == SK::Enum) {
            mix("enum");
            mix(s->a);
            for (const auto &v : s->variants) {
                mix(v.name);
                mix(std::to_string(v.fields.size()));
                for (size_t fi = 0; fi < v.fields.size(); ++fi) {
                    mix((fi < v.field_anns.size() && v.field_anns[fi]) ? *v.field_anns[fi] : "");
                }
            }
        } else if ((s->k == SK::Let || s->k == SK::LetTmp) && s->konst) {
            mix("const");
            mix(s->a);
            mix(s->ann.value_or(""));
        }
    }
    return h;
}

bool ast_mentions_interproc_effects(std::string_view text) {
    // `arena` escape summaries, `spawn` mutation summaries, and plugin callback
    // reentrancy/global checks are interprocedural across callees; when absent,
    // per-function body isolation is exact.
    return text.find("arena") != std::string_view::npos ||
           text.find("spawn") != std::string_view::npos ||
           text.find("lib_open") != std::string_view::npos;
}

static bool is_raw_str_open(std::string_view text, size_t quote_idx) {
    if (quote_idx == 0 || text[quote_idx - 1] != 'r') return false;
    if (quote_idx >= 2) {
        const unsigned char prev = static_cast<unsigned char>(text[quote_idx - 2]);
        if (std::isalnum(prev) || prev == '_') return false;
    }
    return true;
}

size_t function_end(std::string_view text, size_t start) {
    bool in_string = false, raw_string = false, escaped = false;
    bool in_line_comment = false, in_block_comment = false;
    int depth = 0;
    bool opened = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (in_string) {
            if (!raw_string && escaped) escaped = false;
            else if (!raw_string && c == '\\') escaped = true;
            else if (c == '"') { in_string = false; raw_string = false; }
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            in_line_comment = true;
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            in_block_comment = true;
            ++i;
        } else if (c == '"') {
            in_string = true;
            raw_string = is_raw_str_open(text, i);
        } else if (c == '{') {
            opened = true;
            ++depth;
        } else if (c == '}' && opened && --depth == 0) {
            return i + 1;
        }
    }
    return std::string_view::npos;
}

size_t header_end(std::string_view text, size_t start, size_t end) {
    bool in_string = false, raw_string = false, escaped = false;
    bool in_line_comment = false, in_block_comment = false;
    for (size_t i = start; i < end; ++i) {
        const char c = text[i];
        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
        } else if (in_block_comment) {
            if (c == '*' && i + 1 < end && text[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            }
        } else if (in_string) {
            if (!raw_string && escaped) escaped = false;
            else if (!raw_string && c == '\\') escaped = true;
            else if (c == '"') { in_string = false; raw_string = false; }
        } else if (c == '/' && i + 1 < end && text[i + 1] == '/') {
            in_line_comment = true;
            ++i;
        } else if (c == '/' && i + 1 < end && text[i + 1] == '*') {
            in_block_comment = true;
            ++i;
        } else if (c == '"') {
            in_string = true;
            raw_string = is_raw_str_open(text, i);
        } else if (c == '{') {
            return i;
        }
    }
    return end;
}

struct TopFnSpan {
    int start_line = 0;  // 1-based
    int end_line = 0;    // 1-based
    size_t start_off = 0;
    size_t end_off = 0;
    size_t header_len = 0;
    size_t stub_idx = 0;
    size_t non_fn_idx = static_cast<size_t>(-1);
    bool is_impl = false;
};

struct ImportedEnv {
    std::map<std::string, std::vector<ParamMode>> borrow_sigs;
    std::set<std::string> module_heads;
    std::set<std::string> pub_consts;
    std::set<std::string> scalar_rets;
    std::map<std::string, std::vector<size_t>> borrows_rets;
    std::set<std::string> owned_rets;
    std::map<std::string, std::map<std::string, std::string>> struct_fields;
    std::map<std::string, std::string> fn_rets;
    Sigs type_sigs;
    std::map<std::string, Ty> type_consts;
    std::map<std::string, std::map<std::string, Ty>> type_structs;
    uint64_t dep_sig_hash = 0;
};

struct DocState {
    std::string text;
    LineIndex lines;
    uint64_t text_hash = 0;
    uint64_t sig_hash = 0;
    std::vector<Diagnostic> diagnostics;
    std::vector<StmtP> ast_body;
    std::vector<StmtP> non_fn_stubs;
    std::vector<TopFnSpan> fn_spans;
    std::map<std::string, std::vector<ParamMode>> cached_borrow_all;
    Sigs cached_type_all;
    std::vector<std::string> local_const_names;
    std::vector<DocSymbol> symbols;
    mutable std::vector<Decl> decls;
    mutable bool decls_dirty = false;
    ImportedEnv imports;
    std::optional<int> version;
    bool ast_valid = false;
    bool incremental = false;
};

inline void ensure_decls(const DocState &st) {
    if (st.decls_dirty) {
        st.decls.clear();
        if (st.ast_valid) collect_decls(st.ast_body, st.decls);
        st.decls_dirty = false;
    }
}

// Persistent on-disk workspace file cache entry keyed by (canonical_path, mtime, file_size).
struct DiskFileCache {
    std::filesystem::file_time_type mtime{};
    uintmax_t file_size = 0;
    std::string text;
    LineIndex lines;
    std::vector<StmtP> ast_body;
    std::vector<DocSymbol> symbols;
    std::vector<Decl> decls;
    FnTable exported_borrow;
    Sigs exported_sigs;
    std::map<std::string, Ty> exported_consts;
    std::map<std::string, std::map<std::string, Ty>> exported_structs;
    uint64_t sig_hash = 0;
    bool parse_ok = false;
};

class WorkspaceDB {
public:
    void add_root(const std::filesystem::path &root) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        const auto canon = std::filesystem::weakly_canonical(root, ec);
        if (ec) return;
        if (roots_.insert(canon.string()).second) dirty_scan_ = true;
    }

    const DiskFileCache *get_file(const std::string &raw_path) {
        std::error_code ec;
        const auto p = std::filesystem::weakly_canonical(raw_path, ec);
        const std::string key = ec ? raw_path : p.string();
        const auto st = std::filesystem::status(key, ec);
        if (ec || !std::filesystem::is_regular_file(st)) return nullptr;
        const auto mtime = std::filesystem::last_write_time(key, ec);
        if (ec) return nullptr;
        const auto fsz = std::filesystem::file_size(key, ec);
        if (ec) return nullptr;

        auto it = files_.find(key);
        if (it != files_.end() && it->second.mtime == mtime && it->second.file_size == fsz) {
            return &it->second;
        }

        std::ifstream in(key, std::ios::binary);
        if (!in) return nullptr;
        DiskFileCache entry;
        entry.mtime = mtime;
        entry.file_size = fsz;
        entry.text.resize(static_cast<size_t>(fsz));
        if (fsz > 0) in.read(entry.text.data(), static_cast<std::streamsize>(fsz));
        entry.lines.build(entry.text);
        try {
            ParseOut parsed = parse(entry.text);
            entry.ast_body = std::move(parsed.body);
            entry.parse_ok = true;
            extract_doc_symbols(entry.text, entry.lines, entry.ast_body, entry.symbols);
            collect_decls(entry.ast_body, entry.decls);
            entry.exported_borrow = pub_sigs(entry.ast_body);
            collect_sigs(entry.ast_body, entry.exported_sigs);
            collect_exports(entry.ast_body, entry.exported_consts, entry.exported_structs);
            entry.sig_hash = compute_signature_hash(entry.ast_body);
        } catch (...) {
            entry.parse_ok = false;
        }
        auto [ins_it, _] = files_.insert_or_assign(key, std::move(entry));
        known_workspace_files_.insert(key);
        return &ins_it->second;
    }

    void invalidate_file(const std::string &raw_path) {
        std::error_code ec;
        const auto p = std::filesystem::weakly_canonical(raw_path, ec);
        const std::string key = ec ? raw_path : p.string();
        files_.erase(key);
        if (std::filesystem::path(key).extension() == ".brat") {
            get_file(key);
        }
    }

    void evict_file(const std::string &raw_path) {
        std::error_code ec;
        const auto p = std::filesystem::weakly_canonical(raw_path, ec);
        const std::string key = ec ? raw_path : p.string();
        files_.erase(key);
        known_workspace_files_.erase(key);
    }

    std::string primary_root() const {
        if (!roots_.empty()) return *roots_.begin();
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        return ec ? std::string(".") : cwd.string();
    }

    void ensure_roots_indexed() {
        if (!dirty_scan_) return;
        dirty_scan_ = false;
        std::error_code ec;
        const auto std_dir = std::filesystem::weakly_canonical("lang/std", ec);
        if (!ec && std::filesystem::is_directory(std_dir, ec)) roots_.insert(std_dir.string());

        for (const std::string &r : roots_) {
            auto it = std::filesystem::recursive_directory_iterator(
                r, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                const auto &dent = *it;
                if (dent.is_directory(ec)) {
                    const std::string dname = dent.path().filename().string();
                    if (dname == ".git" || dname == "node_modules" || dname == "build" ||
                        dname == "dist" || dname == "out" || dname == ".venv" ||
                        dname == "__pycache__") {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (ec || !dent.is_regular_file(ec)) { ec.clear(); continue; }
                const auto &p = dent.path();
                if (p.extension() != ".brat") continue;
                get_file(p.string());
            }
        }
    }

    const std::set<std::string> &workspace_files() const { return known_workspace_files_; }
    const std::unordered_map<std::string, DiskFileCache> &cache_map() const { return files_; }

private:
    std::set<std::string> roots_;
    std::set<std::string> known_workspace_files_;
    std::unordered_map<std::string, DiskFileCache> files_;
    bool dirty_scan_ = true;
};

struct EditSpanHint {
    bool valid = false;
    size_t old_first = 0;
    size_t old_last = 0;
    size_t new_last = 0;
};

// Apply incremental or full-document UTF-16 `contentChanges` from `LspJsonTape`.
bool apply_content_changes_tape(const LspJsonTape &tape, uint32_t changes_arr,
                                std::string &updated, LineIndex &lines, std::string &error,
                                EditSpanHint *hint = nullptr) {
    if (hint) *hint = EditSpanHint{};
    const bool single_change = (tape.at(changes_arr).count == 1);
    uint32_t item = tape.at(changes_arr).first_child;
    while (item != 0) {
        const uint32_t text_idx = tape.get(item, "text");
        if (!tape.is_string(text_idx)) {
            error = "text change has no string text";
            return false;
        }
        const std::string_view repl = tape.str_view(text_idx);
        if (repl.size() > lsp_wire::kMaxFrameBytes) {
            error = "document exceeds 16 MiB limit";
            return false;
        }
        const uint32_t range_idx = tape.get(item, "range");
        if (tape.is_null(range_idx)) {
            updated.assign(repl.data(), repl.size());
            lines.build(updated);
            item = tape.at(item).next_sibling;
            continue;
        }
        const uint32_t start = tape.get(range_idx, "start");
        const uint32_t end = tape.get(range_idx, "end");
        int s_line = 0, s_char = 0, e_line = 0, e_char = 0;
        size_t first = 0, last = 0;
        if (!tape.exact_int32(tape.get(start, "line"), s_line) ||
            !tape.exact_int32(tape.get(start, "character"), s_char) ||
            !tape.exact_int32(tape.get(end, "line"), e_line) ||
            !tape.exact_int32(tape.get(end, "character"), e_char) ||
            !lines.position_to_offset(updated, s_line, s_char, first) ||
            !lines.position_to_offset(updated, e_line, e_char, last) ||
            last < first) {
            error = "text change has an invalid UTF-16 range";
            return false;
        }
        const uint32_t rlen_idx = tape.get(item, "rangeLength");
        if (!tape.is_null(rlen_idx)) {
            int expected = -1;
            const bool span_ascii = (s_line == e_line && lines.non_ascii[static_cast<size_t>(s_line)] == 0);
            const int actual = LineIndex::utf16_len(
                std::string_view(updated.data() + first, last - first), span_ascii);
            if (!tape.exact_int32(rlen_idx, expected) || expected < 0 || actual != expected) {
                error = "text change rangeLength does not match its range";
                return false;
            }
        }
        if (single_change && hint) {
            hint->valid = true;
            hint->old_first = first;
            hint->old_last = last;
            hint->new_last = first + repl.size();
        }
        const size_t removed = last - first;
        if (updated.size() - removed > lsp_wire::kMaxFrameBytes - repl.size()) {
            error = "document exceeds 16 MiB limit";
            return false;
        }
        updated.replace(first, removed, repl.data(), repl.size());
        lines.build(updated);
        item = tape.at(item).next_sibling;
    }
    return true;
}

void collect_root_modes(const std::vector<StmtP> &body,
                        std::map<std::string, std::vector<ParamMode>> &modes) {
    for (const auto &ptr : body) {
        if (!ptr) continue;
        const Stmt *node = (ptr->k == SK::Pub && !ptr->body.empty()) ? ptr->body.front().get() : ptr.get();
        if (!node || (node->k != SK::Fn && node->k != SK::ExportFn)) continue;
        for (const FnDecl &fn : node->fns) {
            std::vector<ParamMode> params;
            params.reserve(fn.params.size());
            for (const Param &param : fn.params)
                params.push_back({param.name, param.mode});
            modes[fn.name.value_or("")] = std::move(params);
        }
    }
}

void run_check_passes(std::vector<Diagnostic> &out,
                      const std::vector<StmtP> &body,
                      const std::vector<std::pair<uint32_t, std::string>> &stray,
                      const ImportedEnv &imp,
                      const std::map<std::string, std::vector<ParamMode>> &borrow_all,
                      const Sigs &type_all) {
    BorrowOut borrow = borrow_check(body, borrow_all, stray, imp.module_heads,
                                    imp.pub_consts, imp.scalar_rets, imp.borrows_rets,
                                    imp.owned_rets, imp.struct_fields, imp.fn_rets);
    for (const BorrowDiag &d : borrow.errs)
        out.push_back({d.line, 1, classify(d.msg), d.msg, d.note});
    for (const BorrowDiag &d : borrow.warns)
        out.push_back({d.line, 2, classify(d.msg), d.msg, d.note});
    for (const TypeErr &e : type_check(body, type_all, imp.type_consts, imp.type_structs))
        out.push_back({e.line, 1, "T0001", e.msg, std::nullopt});
}

bool can_build_incremental_spans(const std::vector<StmtP> &body) {
    for (const auto &ptr : body) {
        if (!ptr) return false;
        const Stmt *s = (ptr->k == SK::Pub && !ptr->body.empty()) ? ptr->body.front().get() : ptr.get();
        if (!s) return false;
        switch (s->k) {
        case SK::Import:
        case SK::PubUse:
        case SK::Fn:
        case SK::ExportFn:
        case SK::Extern:
        case SK::ExternStruct:
        case SK::Struct:
        case SK::Enum:
        case SK::Trait:
        case SK::Impl:
            break;
        case SK::Let:
        case SK::LetTmp:
            if (!s->konst) return false;  // mutable script-level globals couple functions
            break;
        default:
            return false;
        }
    }
    return true;
}

bool fn_mentions_ptr_struct(const FnDecl &fn) {
    if (fn.ret && fn.ret->find("ptr[") != std::string::npos) return true;
    for (const auto &p : fn.params) {
        if (p.ann && p.ann->find("ptr[") != std::string::npos) return true;
    }
    return false;
}

bool slice_contains_ident(std::string_view sv, std::string_view ident) {
    if (ident.empty()) return false;
    size_t pos = sv.find(ident);
    while (pos != std::string_view::npos) {
        const bool l_ok = (pos == 0 || !is_ident_char(sv[pos - 1]));
        const bool r_ok = (pos + ident.size() >= sv.size() || !is_ident_char(sv[pos + ident.size()]));
        if (l_ok && r_ok) return true;
        pos = sv.find(ident, pos + 1);
    }
    return false;
}

void rebuild_fn_spans(DocState &state) {
    state.non_fn_stubs.clear();
    state.fn_spans.clear();
    state.local_const_names.clear();
    if (!state.ast_valid || ast_mentions_interproc_effects(state.text) ||
        !can_build_incremental_spans(state.ast_body)) {
        state.incremental = false;
        return;
    }
    bool has_local_extern_struct = false;
    for (const auto &ptr : state.ast_body) {
        const Stmt *s = (ptr->k == SK::Pub && !ptr->body.empty()) ? ptr->body.front().get() : ptr.get();
        if (s && s->k == SK::ExternStruct) has_local_extern_struct = true;
        if (ptr->k != SK::Pub && s && (s->k == SK::Let || s->k == SK::LetTmp) && s->konst &&
            !s->a.empty() && s->a[0] != '_') {
            state.local_const_names.push_back(s->a);
        }
    }

    for (size_t i = 0; i < state.ast_body.size(); ++i) {
        const Stmt &raw = *state.ast_body[i];
        const Stmt *s = (raw.k == SK::Pub && !raw.body.empty()) ? raw.body.front().get() : &raw;
        const bool is_fn = s && (s->k == SK::Fn || s->k == SK::ExportFn);
        const bool is_impl = s && (s->k == SK::Impl);

        size_t non_fn_idx = static_cast<size_t>(-1);
        const bool keep_stub = !is_fn ||
            (has_local_extern_struct && !s->fns.empty() && fn_mentions_ptr_struct(s->fns[0]));
        if (keep_stub) {
            non_fn_idx = state.non_fn_stubs.size();
            state.non_fn_stubs.push_back(clone_stmt_header(raw));
        }

        if (is_fn || is_impl) {
            const int start_line = raw.line > 0 ? raw.line : s->line;
            const size_t start_off = state.lines.line_start_offset(state.text, start_line - 1);
            const size_t end_off = function_end(state.text, start_off);
            if (end_off == std::string_view::npos) continue;
            const size_t hdr_end = header_end(state.text, start_off, end_off);
            const int end_line = state.lines.line_of_offset(end_off > 0 ? end_off - 1 : 0) + 1;
            state.fn_spans.push_back({start_line, end_line, start_off, end_off,
                                      hdr_end - start_off, i, non_fn_idx, is_impl});
        }
    }
    state.incremental = !state.fn_spans.empty();
}

bool is_bri_path(std::string_view path) {
    return path.size() > 4 && path.substr(path.size() - 4) == ".bri";
}

bool parse_bri_document(const std::string &src, const LineIndex &lines,
                        ParseOut &parsed, std::vector<Diagnostic> &out) {
    const size_t nlines = lines.line_count();
    for (size_t idx = 0; idx < nlines; ++idx) {
        std::string_view raw = lines.line_view(src, static_cast<int>(idx));
        const size_t b = raw.find_first_not_of(" \t\r");
        if (b == std::string_view::npos) continue;
        const size_t e = raw.find_last_not_of(" \t\r");
        std::string_view text = raw.substr(b, e - b + 1);
        if (text.substr(0, 2) == "//") continue;

        const int start_ln = static_cast<int>(idx) + 1;
        if (text.rfind("struct ", 0) == 0 || text.rfind("pub struct ", 0) == 0 ||
            text.rfind("enum ", 0) == 0 || text.rfind("pub enum ", 0) == 0) {
            std::string chunk;
            int depth = 0;
            bool opened = false;
            size_t end_idx = idx;
            for (; end_idx < nlines; ++end_idx) {
                std::string_view ln_view = lines.line_view(src, static_cast<int>(end_idx));
                chunk.append(ln_view.data(), ln_view.size());
                chunk.push_back('\n');
                for (char ch : ln_view) {
                    if (ch == '{') { opened = true; ++depth; }
                    else if (ch == '}' && opened) --depth;
                }
                if (opened && depth <= 0) break;
            }
            idx = std::min(end_idx, nlines > 0 ? nlines - 1 : 0);
            try {
                ParseOut sp = parse(chunk, start_ln);
                for (auto &st : sp.body) {
                    if (st) {
                        st->line = start_ln;
                        if (st->k == SK::Pub && !st->body.empty() && st->body[0])
                            st->body[0]->line = start_ln;
                        parsed.body.push_back(std::move(st));
                    }
                }
            } catch (const ParseError &err) {
                const int ln = err.line > 0 ? err.line : start_ln;
                out.push_back({ln, 1, "P0001", err.what(), std::nullopt});
                return false;
            }
            continue;
        }

        try {
            ParseOut fp = parse(std::string(text) + " { return 0 }", start_ln);
            Stmt *inner_fn = nullptr;
            if (fp.body.size() == 1 && fp.body[0]) {
                Stmt *top = fp.body[0].get();
                if (top->k == SK::Fn) inner_fn = top;
                else if (top->k == SK::Pub && !top->body.empty() && top->body[0] &&
                         top->body[0]->k == SK::Fn) {
                    inner_fn = top->body[0].get();
                }
            }
            if (!inner_fn || inner_fn->fns.empty()) {
                out.push_back({start_ln, 1, "P0001",
                               "expected `fn name(params) -> type`, `struct Name { ... }`, or `enum Name { ... }` in .bri interface",
                               std::nullopt});
                return false;
            }
            inner_fn->fns[0].body.clear();
            inner_fn->line = start_ln;
            fp.body[0]->line = start_ln;
            parsed.body.push_back(std::move(fp.body[0]));
        } catch (const ParseError &err) {
            out.push_back({start_ln, 1, "P0001", err.what(), std::nullopt});
            return false;
        }
    }
    return true;
}

void analyze_document_full(const std::string &path, DocState &state,
                           const std::map<std::string, DocState> &open_docs,
                           WorkspaceDB &ws_db) {
    state.text_hash = fnv1a64(state.text);
    std::vector<Diagnostic> out;
    ParseOut parsed;
    if (is_bri_path(path)) {
        if (!parse_bri_document(state.text, state.lines, parsed, out)) {
            state.diagnostics = std::move(out);
            state.ast_valid = false;
            state.incremental = false;
            state.symbols.clear();
            state.decls.clear();
            state.decls_dirty = false;
            return;
        }
        state.ast_valid = true;
        state.imports = ImportedEnv{};
        state.diagnostics = std::move(out);
        state.ast_body = std::move(parsed.body);
        state.sig_hash = compute_signature_hash(state.ast_body);
        extract_doc_symbols(state.text, state.lines, state.ast_body, state.symbols);
        state.decls_dirty = true;
        state.incremental = false;
        return;
    }
    try {
        parsed = parse(state.text);
    } catch (const ParseError &e) {
        int max_ln = static_cast<int>(state.lines.line_count());
        while (max_ln > 1 && state.lines.line_view(state.text, max_ln - 1).empty()) --max_ln;
        const int line = std::min(static_cast<int>(e.line), std::max(1, max_ln));
        out.push_back({line, 1, "P0001", e.what(), std::nullopt});
        state.diagnostics = std::move(out);
        state.ast_valid = false;
        state.incremental = false;
        state.symbols.clear();
        state.decls.clear();
        state.decls_dirty = false;
        return;
    } catch (const std::exception &e) {
        out.push_back({0, 1, "P0001", std::string("parse failed: ") + e.what(), std::nullopt});
        state.diagnostics = std::move(out);
        state.ast_valid = false;
        state.incremental = false;
        state.symbols.clear();
        state.decls.clear();
        state.decls_dirty = false;
        return;
    }

    state.ast_valid = true;
    state.imports = ImportedEnv{};

    bool has_imports = false;
    for (const auto &sp : parsed.body) {
        if (sp && (sp->k == SK::Import || sp->k == SK::PubUse)) {
            has_imports = true;
            break;
        }
    }

    const std::string effective_path =
        (path.rfind("untitled:", 0) == 0)
            ? (std::filesystem::path(ws_db.primary_root()) / "__untitled__.brat").string()
            : path;

    std::error_code fs_ec;
    const bool can_resolve_path = !effective_path.empty() &&
        (std::filesystem::exists(effective_path, fs_ec) ||
         std::filesystem::is_directory(std::filesystem::path(effective_path).parent_path(), fs_ec));
    if (has_imports && can_resolve_path) {
        try {
            ModuleSet modules = load_modules(effective_path, state.text);
            if (!modules.files.empty()) {
                for (const auto &[imp_name, target] : modules.files[0].imports) {
                    (void)target;
                    state.imports.module_heads.insert(imp_name.substr(0, imp_name.find('.')));
                    const size_t tail = imp_name.rfind('.');
                    state.imports.module_heads.insert(
                        imp_name.substr(tail == std::string::npos ? 0 : tail + 1));
                }
            }
            uint64_t dep_h = 14695981039346656037ULL;
            for (size_t i = 1; i < modules.files.size(); ++i) {
                const SourceFile &sf = modules.files[i];
                const std::string dep_uri = "file://" + sf.path;
                const std::vector<StmtP> *dep_body = &sf.body;
                auto oit = open_docs.find(dep_uri);
                if (oit != open_docs.end() && oit->second.ast_valid) {
                    dep_body = &oit->second.ast_body;
                } else if (const DiskFileCache *cached = ws_db.get_file(sf.path);
                           cached && cached->parse_ok) {
                    dep_body = &cached->ast_body;
                }
                const std::string dep_stem = std::filesystem::path(sf.path).stem().string();
                FnTable ft = pub_sigs(*dep_body);
                for (auto &[nm, modes] : ft.sigs) state.imports.borrow_sigs[nm] = std::move(modes);
                state.imports.module_heads.insert(ft.heads.begin(), ft.heads.end());
                for (const auto &c : ft.pub_consts) state.imports.pub_consts.insert(c);
                for (const auto &sr : ft.scalar_ret) state.imports.scalar_rets.insert(sr);
                for (auto &[nm, bix] : ft.borrows_ret) state.imports.borrows_rets[nm] = std::move(bix);
                for (const auto &or_ : ft.owned_ret) state.imports.owned_rets.insert(or_);
                for (auto &[sn, flds] : ft.struct_fields) state.imports.struct_fields[sn] = std::move(flds);
                for (auto &[fnm, fr] : ft.fn_rets) {
                    if (!dep_stem.empty() && fnm.find('.') == std::string::npos) {
                        state.imports.fn_rets[dep_stem + "." + fnm] = fr;
                    }
                    state.imports.fn_rets[fnm] = std::move(fr);
                }
                Sigs mod_sigs;
                collect_sigs(*dep_body, mod_sigs);
                for (auto &[snm, fsig] : mod_sigs) {
                    if (!dep_stem.empty() && snm.rfind("__", 0) != 0) {
                        state.imports.type_sigs["__static__." + dep_stem + "." + snm] = fsig;
                    }
                    state.imports.type_sigs[snm] = std::move(fsig);
                }
                collect_exports(*dep_body, state.imports.type_consts, state.imports.type_structs);
                dep_h ^= compute_signature_hash(*dep_body);
            }
            state.imports.dep_sig_hash = dep_h;
        } catch (const std::exception &e) {
            out.push_back({0, 1, "B0000", std::string("module load failed: ") + e.what(),
                           std::nullopt});
            state.diagnostics = std::move(out);
            state.ast_body = std::move(parsed.body);
            state.sig_hash = compute_signature_hash(state.ast_body);
            extract_doc_symbols(state.text, state.lines, state.ast_body, state.symbols);
            state.decls_dirty = true;
            state.incremental = false;
            return;
        }
    }

    state.cached_borrow_all = state.imports.borrow_sigs;
    collect_root_modes(parsed.body, state.cached_borrow_all);
    state.cached_type_all = state.imports.type_sigs;
    collect_sigs(parsed.body, state.cached_type_all);

    run_check_passes(out, parsed.body, parsed.stray, state.imports,
                     state.cached_borrow_all, state.cached_type_all);
    sort_diagnostics(out);
    state.diagnostics = std::move(out);
    state.ast_body = std::move(parsed.body);
    state.sig_hash = compute_signature_hash(state.ast_body);
    extract_doc_symbols(state.text, state.lines, state.ast_body, state.symbols);
    state.decls_dirty = true;
    rebuild_fn_spans(state);
}

// Body-local O(|fn|) incremental check: slices ONLY `[fn_start, fn_end)` without blanking
// the rest of the file, parses at `start_line`, and checks against cached `non_fn_stubs`
// + precomputed `cached_borrow_all` / `cached_type_all` (zero map rebuilds, O(log F) lookup).
bool incremental_analyze_fast(std::string &new_text, LineIndex &new_lines,
                              DocState &state, const EditSpanHint &hint) {
    if (!state.incremental || state.fn_spans.empty()) {
        return false;
    }

    size_t prefix = 0, old_end = state.text.size(), new_end = new_text.size();
    if (hint.valid) {
        if (hint.old_last - hint.old_first == hint.new_last - hint.old_first &&
            std::string_view(state.text.data() + hint.old_first, hint.old_last - hint.old_first) ==
            std::string_view(new_text.data() + hint.old_first, hint.new_last - hint.old_first)) {
            return false;
        }
        prefix = hint.old_first;
        old_end = hint.old_last;
        new_end = hint.new_last;
    } else {
        if (new_text == state.text) return false;
        const size_t min_sz = std::min(new_text.size(), state.text.size());
        while (prefix < min_sz && new_text[prefix] == state.text[prefix]) ++prefix;
        while (old_end > prefix && new_end > prefix &&
               state.text[old_end - 1] == new_text[new_end - 1]) {
            --old_end;
            --new_end;
        }
    }

    // Binary search `fn_spans` (sorted by `end_off`) for the single enclosing span.
    auto it_sp = std::upper_bound(
        state.fn_spans.begin(), state.fn_spans.end(), prefix,
        [](size_t off, const TopFnSpan &sp) { return off < sp.end_off; });
    if (it_sp == state.fn_spans.end()) return false;
    const size_t span_idx = static_cast<size_t>(it_sp - state.fn_spans.begin());
    const TopFnSpan sp = state.fn_spans[span_idx];
    if (prefix < sp.start_off + sp.header_len || old_end > sp.end_off) return false;

    const size_t new_finish = function_end(new_text, sp.start_off);
    if (new_finish == std::string_view::npos || new_end > new_finish) return false;
    if (!hint.valid) {
        if (std::string_view(state.text).substr(sp.end_off) !=
            std::string_view(new_text).substr(new_finish)) {
            return false;
        }
    } else if (static_cast<ptrdiff_t>(new_finish) - static_cast<ptrdiff_t>(sp.end_off) !=
               static_cast<ptrdiff_t>(new_end) - static_cast<ptrdiff_t>(old_end)) {
        // Balanced braces shifted differently -> verify or fall back cleanly
        return false;
    }
    const size_t new_hdr_end = header_end(new_text, sp.start_off, new_finish);
    if (new_hdr_end - sp.start_off != sp.header_len) return false;

    const std::string_view old_slice(state.text.data() + sp.start_off, sp.end_off - sp.start_off);
    const std::string_view fn_slice(new_text.data() + sp.start_off, new_finish - sp.start_off);
    if (ast_mentions_interproc_effects(fn_slice)) return false;

    // Preserve exact W0001 parity on top-level non-pub `const` bindings outside `sp`
    for (const std::string &cn : state.local_const_names) {
        if (slice_contains_ident(old_slice, cn) != slice_contains_ident(fn_slice, cn))
            return false;
    }

    const int old_last_line = sp.end_line;
    const int new_last_line = new_lines.line_of_offset(new_finish > 0 ? new_finish - 1 : 0) + 1;
    const int line_delta = new_last_line - old_last_line;
    const ptrdiff_t byte_delta =
        static_cast<ptrdiff_t>(new_finish) - static_cast<ptrdiff_t>(sp.end_off);

    auto outside_fn = [&](const Diagnostic &d) {
        return d.line < sp.start_line || d.line > old_last_line;
    };
    auto shift_diags = [&](std::vector<Diagnostic> &diags) {
        if (line_delta == 0) return;
        for (Diagnostic &d : diags) {
            if (d.line > old_last_line) d.line += line_delta;
            if (d.note && d.note->line > old_last_line) d.note->line += line_delta;
        }
    };

    ParseOut parsed_fn;
    try {
        parsed_fn = parse(fn_slice, static_cast<uint32_t>(sp.start_line));
    } catch (const ParseError &e) {
        std::vector<Diagnostic> updated;
        int max_ln = static_cast<int>(new_lines.line_count());
        while (max_ln > 1 && new_lines.line_view(new_text, max_ln - 1).empty()) --max_ln;
        updated.push_back({std::min(static_cast<int>(e.line), std::max(1, max_ln)),
                           1, "P0001", e.what(), std::nullopt});
        state.diagnostics = std::move(updated);
        state.text = std::move(new_text);
        state.lines = std::move(new_lines);
        state.ast_valid = false;
        state.symbols.clear();
        state.decls.clear();
        state.decls_dirty = false;
        state.incremental = false;
        return true;
    } catch (...) {
        return false;
    }

    if (parsed_fn.body.size() != 1 || !parsed_fn.body[0]) return false;

    // For `impl` blocks, ensure method signatures inside the `impl` did not change.
    if (sp.is_impl) {
        std::vector<StmtP> old_one, new_one;
        old_one.push_back(clone_stmt_header(*state.ast_body[sp.stub_idx]));
        new_one.push_back(clone_stmt_header(*parsed_fn.body[0]));
        if (compute_signature_hash(old_one) != compute_signature_hash(new_one)) {
            return false;
        }
    }

    // Build miniature check AST: non-Fn stubs plus the single re-parsed item.
    std::vector<StmtP> mini_prog;
    mini_prog.reserve(state.non_fn_stubs.size() + 1);
    bool placed = false;
    for (size_t i = 0; i < state.non_fn_stubs.size(); ++i) {
        if (i == sp.non_fn_idx) {
            mini_prog.push_back(std::move(parsed_fn.body[0]));
            placed = true;
        } else {
            StmtP stub = clone_stmt_header(*state.non_fn_stubs[i]);
            if (line_delta != 0 && stub->line > old_last_line) stub->line += line_delta;
            mini_prog.push_back(std::move(stub));
        }
    }
    if (!placed) {
        mini_prog.push_back(std::move(parsed_fn.body[0]));
    }

    // Filter `cached_borrow_all` & `cached_type_all` to ONLY symbols referenced in `fn_slice`
    // (0–5 entries instead of thousands in a 50k+ LOC file -> avoids O(F) map deep-copies!).
    std::map<std::string, std::vector<ParamMode>> slim_borrow;
    Sigs slim_type;
    {
        size_t p = 0;
        while (p < fn_slice.size()) {
            if (is_ident_char(fn_slice[p]) && !(fn_slice[p] >= '0' && fn_slice[p] <= '9')) {
                const size_t s0 = p++;
                while (p < fn_slice.size() && is_ident_char(fn_slice[p])) ++p;
                const std::string id(fn_slice.data() + s0, p - s0);
                if (auto bit = state.cached_borrow_all.find(id); bit != state.cached_borrow_all.end())
                    slim_borrow.emplace(bit->first, bit->second);
                if (auto tit = state.cached_type_all.find(id); tit != state.cached_type_all.end())
                    slim_type.emplace(tit->first, tit->second);
            } else {
                ++p;
            }
        }
    }

    std::vector<Diagnostic> fn_diags;
    try {
        run_check_passes(fn_diags, mini_prog, parsed_fn.stray, state.imports,
                         slim_borrow, slim_type);
        // If an undeclared identifier (`B0101`) occurred, re-run with full `cached_borrow_all`
        // so `did you mean \`...\`?` searches every function in the module.
        bool has_b0101 = false;
        for (const Diagnostic &d : fn_diags) {
            if (d.code == "B0101") { has_b0101 = true; break; }
        }
        if (has_b0101 && slim_borrow.size() < state.cached_borrow_all.size()) {
            fn_diags.clear();
            run_check_passes(fn_diags, mini_prog, parsed_fn.stray, state.imports,
                             state.cached_borrow_all, slim_type);
        }
    } catch (...) {
        return false;
    }

    std::vector<Diagnostic> updated;
    updated.reserve(state.diagnostics.size() + fn_diags.size());
    for (const Diagnostic &d : state.diagnostics)
        if (outside_fn(d)) updated.push_back(d);
    shift_diags(updated);
    for (const Diagnostic &d : fn_diags) {
        if (d.line >= sp.start_line && d.line <= new_last_line)
            updated.push_back(d);
    }
    sort_diagnostics(updated);

    // Update cached AST, stubs, spans, and symbols in-place.
    const size_t moved_idx = placed ? sp.non_fn_idx : (mini_prog.size() - 1);
    state.ast_body[sp.stub_idx] = std::move(mini_prog[moved_idx]);
    if (sp.non_fn_idx != static_cast<size_t>(-1)) {
        state.non_fn_stubs[sp.non_fn_idx] = clone_stmt_header(*state.ast_body[sp.stub_idx]);
    }
    if (line_delta != 0) {
        for (size_t i = sp.stub_idx + 1; i < state.ast_body.size(); ++i) {
            if (state.ast_body[i]->line > old_last_line) state.ast_body[i]->line += line_delta;
            if (state.ast_body[i]->k == SK::Pub && !state.ast_body[i]->body.empty() &&
                state.ast_body[i]->body.front()->line > old_last_line) {
                state.ast_body[i]->body.front()->line += line_delta;
            }
        }
        for (size_t i = 0; i < state.non_fn_stubs.size(); ++i) {
            if (i == sp.non_fn_idx) continue;
            if (state.non_fn_stubs[i]->line > old_last_line)
                state.non_fn_stubs[i]->line += line_delta;
        }
    }
    state.fn_spans[span_idx].end_line = new_last_line;
    state.fn_spans[span_idx].end_off = new_finish;
    for (size_t i = span_idx + 1; i < state.fn_spans.size(); ++i) {
        state.fn_spans[i].start_line += line_delta;
        state.fn_spans[i].end_line += line_delta;
        state.fn_spans[i].start_off = static_cast<size_t>(
            static_cast<ptrdiff_t>(state.fn_spans[i].start_off) + byte_delta);
        state.fn_spans[i].end_off = static_cast<size_t>(
            static_cast<ptrdiff_t>(state.fn_spans[i].end_off) + byte_delta);
    }

    state.text = std::move(new_text);
    state.lines = std::move(new_lines);
    state.diagnostics = std::move(updated);
    state.ast_valid = true;
    state.decls_dirty = true;

    if (sp.is_impl) {
        extract_doc_symbols(state.text, state.lines, state.ast_body, state.symbols);
    } else if (line_delta != 0) {
        for (DocSymbol &sym : state.symbols) {
            if (sym.line >= old_last_line) sym.line += line_delta;
        }
    }
    return true;
}

std::string format_fn_sig(const FnDecl &fn, bool is_pub = false) {
    std::string s = is_pub ? "pub fn " : "fn ";
    s += fn.name.value_or("<anon>") + "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) s += ", ";
        if (fn.params[i].mode == Mode::Mut) s += "mut ";
        else if (fn.params[i].mode == Mode::Move) s += "move ";
        s += fn.params[i].name;
        if (fn.params[i].ann) s += ": " + *fn.params[i].ann;
    }
    s += ")";
    if (fn.ret) s += " -> " + *fn.ret;
    return s;
}

std::string c_abi_note(const std::string &ann) {
    if (ann == "i64" || ann == "u64") return "8B (int64_t / uint64_t)";
    if (ann == "i32" || ann == "u32" || ann == "c_int" || ann == "c_uint") return "4B (int32_t / uint32_t)";
    if (ann == "i16" || ann == "u16") return "2B";
    if (ann == "i8" || ann == "u8" || ann == "bool") return "1B";
    if (ann == "f64") return "8B IEEE-754 double";
    if (ann == "f32") return "4B IEEE-754 float";
    if (ann == "c_long" || ann == "c_ulong") return "8B on LP64, 4B on LLP64/ILP32";
    if (ann == "usize" || ann == "isize" || ann.find("ptr") != std::string::npos || ann == "str")
        return "8B on 64-bit, 4B on ILP32";
    return ann;
}

std::string format_extern_sig(const ExternDecl &ext) {
    std::string s = "extern ";
    if (ext.lib && !ext.lib->empty()) s += "\"" + *ext.lib + "\" ";
    s += "fn " + ext.name + "(";
    std::vector<std::string> abi_parts;
    for (size_t i = 0; i < ext.params.size(); ++i) {
        if (i) s += ", ";
        if (ext.params[i].mode == Mode::Move) s += "move ";
        s += ext.params[i].name;
        if (ext.params[i].ann) {
            s += ": " + *ext.params[i].ann;
            abi_parts.push_back(ext.params[i].name + ": " + c_abi_note(*ext.params[i].ann));
        }
    }
    s += ")";
    if (ext.ret) {
        s += " -> ";
        if (ext.owned) s += "move ";
        s += *ext.ret;
        abi_parts.push_back("ret: " + c_abi_note(*ext.ret));
    }
    for (const std::string &eff : ext.effects) {
        if (eff.rfind("reads:", 0) == 0) s += " reads(" + eff.substr(6) + ")";
        else if (eff.rfind("writes:", 0) == 0) s += " writes(" + eff.substr(7) + ")";
        else if (eff.rfind("borrows:", 0) == 0) s += " borrows(" + eff.substr(8) + ")";
        else s += " " + eff;
    }
    std::string md = "```brat\n" + s + "\n```";
    if (!abi_parts.empty()) {
        md += "\nC ABI layout: ";
        for (size_t i = 0; i < abi_parts.size(); ++i) {
            if (i) md += ", ";
            md += "`" + abi_parts[i] + "`";
        }
    }
    return md;
}

const std::map<std::string, std::string> &builtin_docs() {
    static const std::map<std::string, std::string> m = {
        {"print", "fn print(...): write formatted values to stdout"},
        {"eprint", "fn eprint(...): write formatted values to stderr"},
        {"len", "fn len(x) -> i64: length of str (codepoints), vec or map"},
        {"str", "fn str(x) -> str: convert value to UTF-8 string"},
        {"read_file", "fn read_file(path: str|vec[i64]) -> Result[str, str] (confined)"},
        {"write_file", "fn write_file(path: str|vec[i64], text: str) -> Result[i64, str] (confined)"},
        {"append_file", "fn append_file(path: str|vec[i64], text: str) -> Result[i64, str] (confined atomic append)"},
        {"read_bytes", "fn read_bytes(path: str|vec[i64]) -> Result[vec[i64], str] (confined)"},
        {"write_bytes", "fn write_bytes(path: str|vec[i64], data: vec[i64]) -> Result[i64, str] (confined)"},
        {"utf8_bytes", "fn utf8_bytes(s: str) -> vec[i64]: UTF-8 octets 0..255"},
        {"utf8_str", "fn utf8_str(data: vec[i64]) -> Result[str, str]: decode validated UTF-8"},
        {"fs_exists", "fn fs_exists(path: str|vec[i64]) -> bool (confined regular-file probe)"},
        {"fs_is_dir", "fn fs_is_dir(path: str|vec[i64]) -> bool (confined directory probe)"},
        {"fs_file_size", "fn fs_file_size(path: str|vec[i64]) -> Result[i64, str] (confined regular-file byte size)"},
        {"fs_mkdir_all", "fn fs_mkdir_all(path: str|vec[i64]) -> Result[i64, str] (confined recursive directory creation)"},
        {"fs_list_dir", "fn fs_list_dir(path: str|vec[i64]) -> Result[vec[str], str] (confined sorted directory entries)"},
        {"fs_remove_dir", "fn fs_remove_dir(path: str|vec[i64]) -> Result[i64, str] (confined empty-directory removal)"},
        {"fs_open", "fn fs_open(path: str|vec[i64]) -> Result[i64, str] (confined streaming reader)"},
        {"fs_read_line", "fn fs_read_line(handle: i64) -> Result[Option[str], str] (read one UTF-8 line up to 16 MiB)"},
        {"fs_close", "fn fs_close(handle: i64) -> Result[i64, str] (close streaming reader)"},
        {"stdin_read_line", "fn stdin_read_line() -> Result[Option[str], str] (read one UTF-8 line from stdin)"},
        {"fs_rename", "fn fs_rename(src: str|vec[i64], dst: str|vec[i64]) -> Result[i64, str] (confined atomic rename)"},
        {"fs_remove", "fn fs_remove(path: str|vec[i64]) -> Result[i64, str] (confined unlink)"},
        {"proc_run", "fn proc_run(prog: str, argv: vec[str|vec[i64]]) -> Result[map, str] (gated by BRAT_EXEC)"},
        {"re_match", "fn re_match(s: str, pat: str) -> bool: Pike VM regex search ({n,m} supported)"},
        {"re_find", "fn re_find(s: str, pat: str) -> Option[str]: first regex match"},
        {"re_captures", "fn re_captures(s: str, pat: str) -> Option[vec[str]]: full match + groups"},
        {"re_replace", "fn re_replace(s: str, pat: str, repl: str) -> str: global replace ($0..$9, $$)"},
        {"Ok", "fn Ok(value: T) -> Result[T, E]"},
        {"Err", "fn Err(err: E) -> Result[T, E]"},
        {"Some", "fn Some(value: T) -> Option[T]"},
        {"unwrap", "fn unwrap(res: Result[T, E] | Option[T]) -> T: extract payload or raise"},
        {"unwrap_or", "fn unwrap_or(res: Result[T, E] | Option[T], fallback: T) -> T: extract payload or evaluate fallback"},
        {"is_ok", "fn is_ok(x) -> bool"},
        {"is_err", "fn is_err(x) -> bool"},
        {"is_some", "fn is_some(x) -> bool"},
        {"is_none", "fn is_none(x) -> bool"},
        {"is_vec", "fn is_vec(x) -> bool"},
        {"is_str", "fn is_str(x) -> bool"},
        {"is_map", "fn is_map(x) -> bool"},
        {"is_bool", "fn is_bool(x) -> bool"},
        {"is_int", "fn is_int(x) -> bool"},
        {"is_float", "fn is_float(x) -> bool"},
        {"json_parse", "fn json_parse(s: str) -> Result[dyn, str]"},
        {"json_stringify", "fn json_stringify(v, indent: i64) -> str"},
        {"args", "fn args() -> vec[str]: command-line arguments (UTF-8 only)"},
        {"args_bytes", "fn args_bytes() -> vec[vec[i64]]: raw OS argument bytes"},
        {"env_get", "fn env_get(key: str) -> str | none"},
        {"env_get_bytes", "fn env_get_bytes(key: str) -> vec[i64] | none: raw OS environment bytes"},
        {"time_ms", "fn time_ms() -> i64: epoch milliseconds"},
        {"mono_ns", "fn mono_ns() -> i64: monotonic clock nanoseconds"},
        {"sleep_ms", "fn sleep_ms(ms: i64)"},
        {"sha256_bytes", "fn sha256_bytes(data: str | vec[i64]) -> Result[vec[i64], str]: FIPS 180-4 SHA-256"},
        {"random_bytes", "fn random_bytes(n: i64) -> Result[vec[i64], str]: CSPRNG octets"},
        {"https_request_raw", "fn https_request_raw(addr: str, sni: str, wire: str, ca: str) -> Result[str, str]"},
        {"sql_open", "fn sql_open(path: str|vec[i64]) -> Result[i64, str] (confined or :memory:)"},
        {"sql_close", "fn sql_close(db: i64) -> Result[i64, str]"},
        {"sql_exec", "fn sql_exec(db: i64, sql: str) -> Result[i64, str]"},
        {"sql_query", "fn sql_query(db: i64, sql: str, params: vec) -> Result[vec[map], str]"},
        {"lib_open", "fn lib_open(path: str) -> handle: load shared plugin (linear handle)"},
        {"lib_sym", "fn lib_sym(lib, name: str) -> sym: resolve plugin symbol"},
        {"lib_close", "fn lib_close(lib): release shared plugin handle"},
        {"call", "fn call(sym, sig: str, ...): invoke plugin symbol (verified against brat_plugin_abi_v1)"},
        {"ptr_offset", "fn ptr_offset(p: ptr, bytes: i64) -> ptr: offset pointer preserving borrow provenance"},
        {"ptr_read_struct", "fn ptr_read_struct(p: ptr, name: str) -> map: read C struct/union"},
        {"ptr_write_struct", "fn ptr_write_struct(p: ptr, val, name: str) -> i64: write C struct/union"},
    };
    return m;
}

std::string unwrap_hint_adt(const std::string &raw) {
    size_t b = raw.find_first_not_of(" \t");
    size_t z = raw.find_last_not_of(" \t");
    if (b == std::string::npos) return {};
    std::string a = raw.substr(b, z - b + 1);
    if (a.size() > 8 && a.rfind("Option[", 0) == 0 && a.back() == ']') {
        std::string p = a.substr(7, a.size() - 8);
        size_t pb = p.find_first_not_of(" \t");
        size_t pz = p.find_last_not_of(" \t");
        return pb == std::string::npos ? std::string() : p.substr(pb, pz - pb + 1);
    }
    if (a.size() > 8 && a.rfind("Result[", 0) == 0 && a.back() == ']') {
        std::string inner = a.substr(7, a.size() - 8);
        int depth = 0;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '[' || inner[i] == '(') ++depth;
            else if (inner[i] == ']' || inner[i] == ')') --depth;
            else if (inner[i] == ',' && depth == 0) {
                std::string p = inner.substr(0, i);
                size_t pb = p.find_first_not_of(" \t");
                size_t pz = p.find_last_not_of(" \t");
                return pb == std::string::npos ? std::string() : p.substr(pb, pz - pb + 1);
            }
        }
    }
    return {};
}

std::string format_enum_variant_sig(const Variant &v) {
    std::string s = v.name;
    if (!v.fields.empty()) {
        s += "(";
        for (size_t i = 0; i < v.fields.size(); ++i) {
            if (i) s += ", ";
            const std::string &fn = v.fields[i];
            const bool synth = fn.size() >= 2 && fn[0] == '_' && std::isdigit(static_cast<unsigned char>(fn[1]));
            const std::string fann = (i < v.field_anns.size() && v.field_anns[i] && !v.field_anns[i]->empty())
                                         ? *v.field_anns[i]
                                         : std::string();
            if (!synth && !fann.empty()) s += fn + ": " + fann;
            else if (!synth) s += fn;
            else if (!fann.empty()) s += fann;
            else s += "dyn";
        }
        s += ")";
    }
    return s;
}

std::string format_enum_hover(const Stmt &s, bool is_pub) {
    std::string md = std::string("```brat\n") + (is_pub ? "pub enum " : "enum ") + s.a + " {\n";
    for (const auto &v : s.variants) {
        md += "  " + format_enum_variant_sig(v) + ",\n";
    }
    md += "}\n```";
    return md;
}

std::string infer_expr_type_hint(const Expr &e, const std::vector<StmtP> &root_body,
                                 const std::map<std::string, std::string> *ext_rets = nullptr) {
    switch (e.k) {
    case EK::Num: return e.is_float ? "f64" : "i64";
    case EK::Str:
    case EK::RStr: return "str";
    case EK::Bool: return "bool";
    case EK::VecLit: return "vec";
    case EK::MapLit: return "map";
    case EK::StructLit: return e.a;
    case EK::Field: {
        if (e.kids.empty() || !e.kids[0]) return {};
        if (e.kids[0]->k == EK::Var) {
            for (const auto &sp : root_body) {
                if (!sp) continue;
                const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                if (s && s->k == SK::Enum && s->a == e.kids[0]->a) return s->a;
            }
        }
        const std::string base_ty = infer_expr_type_hint(*e.kids[0], root_body, ext_rets);
        if (!base_ty.empty()) {
            for (const auto &sp : root_body) {
                if (!sp) continue;
                const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                if (s && (s->k == SK::Struct || s->k == SK::ExternStruct) && s->a == base_ty) {
                    for (const auto &f : s->fields)
                        if (f.name == e.b && f.ann) return *f.ann;
                }
            }
        }
        return {};
    }
    case EK::Quest:
        if (!e.kids.empty())
            return unwrap_hint_adt(infer_expr_type_hint(*e.kids[0], root_body, ext_rets));
        return {};
    case EK::MCall: {
        if (e.b == "is_ok" || e.b == "is_err" || e.b == "is_some" || e.b == "is_none")
            return "bool";
        if (e.b == "parse_int") return "Result[i64, str]";
        if (e.b == "parse_float") return "Result[f64, str]";
        if (e.b == "copy" && !e.kids.empty())
            return infer_expr_type_hint(*e.kids[0], root_body, ext_rets);
        if (!e.kids.empty() && (e.b == "unwrap" || e.b == "unwrap_or")) {
            std::string u = unwrap_hint_adt(infer_expr_type_hint(*e.kids[0], root_body, ext_rets));
            if (!u.empty() && u != "dyn") return u;
            if (e.b == "unwrap_or" && e.kids.size() >= 2)
                return infer_expr_type_hint(*e.kids[1], root_body, ext_rets);
        }
        if (!e.kids.empty() &&
            ((e.kids[0]->k == EK::Call && e.kids[0]->kids.empty()) || e.kids[0]->k == EK::Var)) {
            const std::string &rname = e.kids[0]->a;
            for (const auto &sp : root_body) {
                if (!sp) continue;
                const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                if (s && s->k == SK::Impl && s->a == rname) {
                    for (const auto &fn : s->fns) {
                        if (fn.name && *fn.name == e.b && fn.ret) return *fn.ret;
                    }
                }
            }
        }
        if (ext_rets) {
            if (!e.kids.empty() &&
                ((e.kids[0]->k == EK::Call && e.kids[0]->kids.empty()) || e.kids[0]->k == EK::Var)) {
                auto sit = ext_rets->find(e.kids[0]->a + "." + e.b);
                if (sit != ext_rets->end()) return sit->second;
            }
            auto it = ext_rets->find(e.b);
            if (it != ext_rets->end()) return it->second;
        }
        return {};
    }
    case EK::Call: {
        if (e.a == "len" || e.a == "time_ms" || e.a == "mono_ns") return "i64";
        if (e.a == "str") return "str";
        if (e.a == "is_ok" || e.a == "is_err" || e.a == "is_some" || e.a == "is_none" ||
            e.a == "re_match" || e.a == "fs_exists" || e.a == "fs_is_dir") return "bool";
        if (e.a == "read_file") return "Result[str, str]";
        if (e.a == "fs_list_dir") return "Result[vec[str], str]";
        if (e.a == "fs_read_line" || e.a == "stdin_read_line")
            return "Result[Option[str], str]";
        if (e.a == "write_file" || e.a == "append_file" ||
            e.a == "fs_open" || e.a == "fs_close" ||
            e.a == "fs_file_size" || e.a == "fs_mkdir_all" || e.a == "fs_remove_dir")
            return "Result[i64, str]";
        if (e.a == "re_find") return "Option[str]";
        if (e.a == "Some" && e.kids.size() == 1) {
            std::string in = infer_expr_type_hint(*e.kids[0], root_body, ext_rets);
            return in.empty() ? "Option[dyn]" : "Option[" + in + "]";
        }
        if (e.a == "Ok" && e.kids.size() == 1) {
            std::string in = infer_expr_type_hint(*e.kids[0], root_body, ext_rets);
            return in.empty() ? "Result[dyn, dyn]" : "Result[" + in + ", dyn]";
        }
        if (e.a == "Err" && e.kids.size() == 1) {
            std::string in = infer_expr_type_hint(*e.kids[0], root_body, ext_rets);
            return in.empty() ? "Result[dyn, dyn]" : "Result[dyn, " + in + "]";
        }
        if (e.a == "unwrap" && e.kids.size() == 1)
            return unwrap_hint_adt(infer_expr_type_hint(*e.kids[0], root_body, ext_rets));
        if (e.a == "unwrap_or" && e.kids.size() == 2) {
            std::string u = unwrap_hint_adt(infer_expr_type_hint(*e.kids[0], root_body, ext_rets));
            if (!u.empty() && u != "dyn") return u;
            return infer_expr_type_hint(*e.kids[1], root_body, ext_rets);
        }
        for (const auto &sp : root_body) {
            if (!sp) continue;
            const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
            if (!s) continue;
            if ((s->k == SK::Fn || s->k == SK::ExportFn) && !s->fns.empty() &&
                s->fns.front().name == e.a && s->fns.front().ret) {
                return *s->fns.front().ret;
            }
            if (s->k == SK::Extern && s->ext.name == e.a && s->ext.ret) {
                return *s->ext.ret;
            }
            if (s->k == SK::Enum) {
                for (const auto &v : s->variants) {
                    if (v.name == e.a) return s->a;
                }
            }
        }
        if (ext_rets) {
            auto it = ext_rets->find(e.a);
            if (it != ext_rets->end()) return it->second;
        }
        return {};
    }
    default:
        return {};
    }
}

std::optional<std::string> find_let_hover(const std::vector<StmtP> &body, const std::string &name, int max_line) {
    std::optional<std::string> found;
    int best_line = -1;
    std::function<void(const std::vector<StmtP> &)> walk = [&](const std::vector<StmtP> &stmts) {
        for (const auto &sp : stmts) {
            if (!sp) continue;
            const Stmt &s = *sp;
            if (s.k == SK::Pub && !s.body.empty()) {
                walk(s.body);
                continue;
            }
            if (s.k == SK::Fn || s.k == SK::ExportFn || s.k == SK::Impl) {
                for (const FnDecl &fn : s.fns) {
                    for (const Param &p : fn.params) {
                        if (p.name == name && s.line - 1 <= max_line && s.line >= best_line) {
                            best_line = s.line;
                            std::string h = "parameter ";
                            if (p.mode == Mode::Mut) h += "mut ";
                            else if (p.mode == Mode::Move) h += "move ";
                            const std::string pty = p.ann ? *p.ann
                                                          : ((s.k == SK::Impl && p.name == "self") ? s.a : "dyn");
                            h += p.name + ": " + pty;
                            found = "```brat\n" + h + "\n```";
                        }
                    }
                    walk(fn.body);
                }
            } else if ((s.k == SK::Let || s.k == SK::LetTmp) && s.a == name &&
                       s.line - 1 <= max_line && s.line >= best_line) {
                best_line = s.line;
                std::string ty = s.ann.value_or("");
                if (ty.empty() && s.e1) ty = infer_expr_type_hint(*s.e1, body);
                std::string h = s.konst ? "const " : (s.mut ? "let mut " : "let ");
                h += s.a + ": " + (ty.empty() ? "dyn" : ty);
                found = "```brat\n" + h + "\n```";
            }
            walk(s.body);
            walk(s.body2);
            if (s.els) walk(s.els->body);
        }
    };
    walk(body);
    return found;
}

struct SymOccur {
    int line = 0;
    int start_col = 0;
    int end_col = 0;
    bool is_decl = false;
    bool is_write = false;
};

std::vector<SymOccur> find_symbol_occurrences_fast(std::string_view text,
                                                   const LineIndex &lines,
                                                   std::string_view name,
                                                   bool qualified_ok) {
    std::vector<SymOccur> out;
    if (name.empty() || text.find(name) == std::string_view::npos) return out;
    std::vector<Tok> toks;
    try { toks = lex(text); } catch (...) { return out; }
    for (size_t i = 0; i < toks.size(); ++i) {
        const Tok &t = toks[i];
        if (t.kind != TK::Id || t.text != name) continue;
        const bool after_dot = (i > 0 && toks[i - 1].text == ".");
        if (after_dot && !qualified_ok) continue;
        const int ln = std::max(0, static_cast<int>(t.line) - 1);
        if (static_cast<size_t>(ln) >= lines.line_count()) continue;
        const size_t byte_off = static_cast<size_t>(t.text.data() - text.data());
        const size_t line_start = lines.starts[static_cast<size_t>(ln)];
        if (byte_off < line_start) continue;
        const int scol = lines.byte_to_utf16_col(text, ln, byte_off - line_start);
        const int ecol = scol + LineIndex::utf16_len(t.text, lines.non_ascii[static_cast<size_t>(ln)] == 0);
        bool is_decl = false;
        if (i > 0) {
            const std::string_view prev = toks[i - 1].text;
            if (prev == "fn" || prev == "let" || prev == "mut" || prev == "const" ||
                prev == "enum" || prev == "struct" || prev == "union" || prev == "for")
                is_decl = true;
        }
        bool is_write = is_decl;
        if (!is_write && i + 1 < toks.size()) {
            const std::string_view nx = toks[i + 1].text;
            if (nx == "=" || nx == "+=" || nx == "-=" || nx == "*=" || nx == "/=" || nx == "%=")
                is_write = true;
        }
        if (!is_write && i > 0 && (toks[i - 1].text == "mut" || toks[i - 1].text == "move"))
            is_write = true;
        out.push_back({ln, scol, ecol, is_decl, is_write});
    }
    return out;
}

// Fuzzy symbol matcher: returns score > 0 when `query_low` matches `cand_low`
// (exact = 1000, prefix = 800, substring = 500, subsequence = 200), 0 on no match.
int fuzzy_score(std::string_view query_low, std::string_view cand_low) {
    if (query_low.empty()) return 1;
    if (cand_low == query_low) return 1000;
    if (cand_low.rfind(query_low, 0) == 0) return 800;
    if (cand_low.find(query_low) != std::string_view::npos) return 500;
    size_t qi = 0;
    for (size_t ci = 0; ci < cand_low.size() && qi < query_low.size(); ++ci) {
        if (cand_low[ci] == query_low[qi]) ++qi;
    }
    return qi == query_low.size() ? 200 : 0;
}

// Canonical fast lossless BRAT source formatter (2-space block indentation,
// preserves comments, strings, and empty-line spacing).
std::string format_brat_source(std::string_view text, const LineIndex &lines) {
    std::string out;
    out.reserve(text.size() + 64);
    int indent = 0;
    int consecutive_blank = 0;
    bool in_str = false, raw_str = false, esc = false, in_blk = false;
    auto scan_line = [&](std::string_view s) -> int {
        int net = 0;
        for (size_t k = 0; k < s.size(); ++k) {
            const char c = s[k];
            if (in_blk) {
                if (c == '*' && k + 1 < s.size() && s[k + 1] == '/') {
                    in_blk = false;
                    ++k;
                }
                continue;
            }
            if (in_str) {
                if (!raw_str && esc) esc = false;
                else if (!raw_str && c == '\\') esc = true;
                else if (c == '"') { in_str = false; raw_str = false; }
                continue;
            }
            if (c == '/' && k + 1 < s.size() && s[k + 1] == '/') break;
            if (c == '/' && k + 1 < s.size() && s[k + 1] == '*') {
                in_blk = true;
                ++k;
                continue;
            }
            if (c == '"') {
                in_str = true;
                raw_str = is_raw_str_open(s, k);
                esc = false;
            } else if (c == '{' || c == '(' || c == '[') ++net;
            else if (c == '}' || c == ')' || c == ']') --net;
        }
        if (in_str && !raw_str && esc) esc = false;
        return net;
    };
    for (size_t i = 0; i < lines.line_count(); ++i) {
        if (i + 1 == lines.line_count() && lines.line_view(text, static_cast<int>(i)).empty() &&
            !text.empty() && text.back() == '\n') {
            break;
        }
        std::string_view raw_sv = lines.line_view(text, static_cast<int>(i));
        if (in_str) {
            out.append(raw_sv.data(), raw_sv.size());
            out.push_back('\n');
            indent = std::max(0, indent + scan_line(raw_sv));
            continue;
        }
        std::string_view sv = raw_sv;
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
        if (sv.empty()) {
            if (++consecutive_blank <= 1 && !out.empty()) out.push_back('\n');
            continue;
        }
        consecutive_blank = 0;
        int lead_close = 0;
        if (!in_blk) {
            for (char c : sv) {
                if (c == '}' || c == ')' || c == ']') ++lead_close;
                else break;
            }
        }
        const int line_indent = std::max(0, indent - lead_close);
        out.append(static_cast<size_t>(line_indent) * 2, ' ');
        out.append(sv.data(), sv.size());
        out.push_back('\n');
        indent = std::max(0, indent + scan_line(sv));
    }
    return out;
}

// LSP server over the zero-copy wire and incremental Salsa query table.
class Server {
public:
    int run() {
        for (;;) {
            std::string_view frame_body;
            std::string hdr_err;
            const FrameStatus st = reader_.next_frame(frame_body, hdr_err);
            if (st == FrameStatus::Eof) return 0;
            if (st == FrameStatus::HeaderError) {
                std::fprintf(stderr, "brat-lsp: %s\n", hdr_err.c_str());
                return 0;
            }
            if (!tape_.parse(frame_body)) {
                send_error("null", -32700, std::string("parse error: ") + std::string(tape_.error()));
                continue;
            }
            const uint32_t root = tape_.root();
            if (!tape_.is_object(root)) {
                send_error("null", -32600, "invalid request");
                continue;
            }
            const uint32_t id_idx = tape_.get(root, "id");
            const bool has_id = (id_idx != 0);
            if (has_id && !tape_.is_null(id_idx) &&
                !tape_.is_string(id_idx) && !tape_.is_number(id_idx)) {
                send_error("null", -32600, "invalid request");
                continue;
            }
            const std::string_view raw_id = has_id ? tape_.raw_json_slice(id_idx) : std::string_view{"null"};
            const uint32_t jrpc_idx = tape_.get(root, "jsonrpc");
            const uint32_t method_idx = tape_.get(root, "method");
            if (!tape_.is_string(jrpc_idx) || tape_.str_view(jrpc_idx) != "2.0" ||
                !tape_.is_string(method_idx)) {
                send_error(has_id ? raw_id : std::string_view{"null"}, -32600, "invalid request");
                continue;
            }
            try {
                if (!handle(root, id_idx, has_id, raw_id)) return 0;
            } catch (const std::exception &e) {
                if (has_id && !tape_.is_null(id_idx)) {
                    send_error(raw_id, -32603, e.what());
                } else {
                    std::fprintf(stderr, "brat-lsp: notification failed: %s\n", e.what());
                }
            }
        }
    }

private:
    LspFrameReader reader_;
    LspJsonTape tape_;
    JsonWriter writer_;
    std::map<std::string, DocState> docs_;
    WorkspaceDB ws_db_;

    void send_error(std::string_view raw_id, int code, std::string_view message) {
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("error");
        writer_.begin_obj();
        writer_.key_lit("code"); writer_.int64(code);
        writer_.key_lit("message"); writer_.str(message);
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void send_null_result(std::string_view raw_id) {
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result"); writer_.null_val();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void send_log_error(std::string_view msg) {
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("method"); writer_.str("window/logMessage");
        writer_.key_lit("params");
        writer_.begin_obj();
        writer_.key_lit("type"); writer_.int64(1);
        writer_.key_lit("message"); writer_.str(msg);
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void publish_diagnostics(const std::string &uri, const DocState &state) {
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("method"); writer_.str("textDocument/publishDiagnostics");
        writer_.key_lit("params");
        writer_.begin_obj();
        writer_.key_lit("uri"); writer_.str(uri);
        writer_.key_lit("diagnostics");
        writer_.begin_arr();
        for (const Diagnostic &d : state.diagnostics) {
            const int ln = std::max(0, d.line - 1);
            const int end_col = state.lines.line_utf16_len(state.text, ln);
            writer_.begin_obj();
            writer_.key_lit("range"); writer_.lsp_range(ln, 0, ln, end_col);
            writer_.key_lit("severity"); writer_.int64(d.severity);
            writer_.key_lit("code"); writer_.str(d.code);
            writer_.key_lit("source"); writer_.str("brat");
            writer_.key_lit("message"); writer_.str(d.message);
            if (d.note) {
                const int nln = std::max(0, d.note->line - 1);
                writer_.key_lit("relatedInformation");
                writer_.begin_arr();
                writer_.begin_obj();
                writer_.key_lit("location");
                writer_.begin_obj();
                writer_.key_lit("uri"); writer_.str(uri);
                writer_.key_lit("range"); writer_.lsp_range(nln, 0, nln, 0);
                writer_.end_obj();
                writer_.key_lit("message"); writer_.str(d.note->msg);
                writer_.end_obj();
                writer_.end_arr();
            }
            writer_.end_obj();
        }
        writer_.end_arr();
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Re-check any open documents that import an edited open module whose exported signature changed.
    void propagate_signature_change(const std::string &changed_uri, uint64_t old_sig, uint64_t new_sig) {
        if (old_sig == new_sig) return;
        const std::string changed_path = path_from_uri(changed_uri);
        if (changed_path.empty()) return;
        const std::string stem = std::filesystem::path(changed_path).stem().string();
        for (auto &[other_uri, other_st] : docs_) {
            if (other_uri == changed_uri) continue;
            if (other_st.text.find("import ") == std::string::npos ||
                other_st.text.find(stem) == std::string::npos) {
                continue;
            }
            analyze_document_full(path_from_uri(other_uri), other_st, docs_, ws_db_);
            publish_diagnostics(other_uri, other_st);
        }
    }

    void on_disk_file_changed(const std::string &uri, int change_type) {
        const std::string changed_path = path_from_uri(uri);
        if (changed_path.empty()) return;
        if (change_type == 3) ws_db_.evict_file(changed_path);
        else ws_db_.invalidate_file(changed_path);
        const std::string stem = std::filesystem::path(changed_path).stem().string();
        if (stem.empty()) return;
        for (auto &[other_uri, other_st] : docs_) {
            if (other_uri == uri) continue;
            if (other_st.text.find("import ") == std::string::npos ||
                other_st.text.find(stem) == std::string::npos) {
                continue;
            }
            analyze_document_full(path_from_uri(other_uri), other_st, docs_, ws_db_);
            publish_diagnostics(other_uri, other_st);
        }
    }

    const DocState *find_doc(const std::string &uri) const {
        auto it = docs_.find(uri);
        return it == docs_.end() ? nullptr : &it->second;
    }

    void handle_initialize(std::string_view raw_id, uint32_t params) {
        const std::string_view root_uri = tape_.str_view(tape_.get(params, "rootUri"));
        if (!root_uri.empty()) {
            ws_db_.add_root(path_from_uri(root_uri));
        }
        const uint32_t wf = tape_.get(params, "workspaceFolders");
        if (tape_.is_array(wf)) {
            for (uint32_t item = tape_.at(wf).first_child; item != 0;
                 item = tape_.at(item).next_sibling) {
                const std::string_view furi = tape_.str_view(tape_.get(item, "uri"));
                if (!furi.empty()) ws_db_.add_root(path_from_uri(furi));
            }
        }
        const uint32_t td_caps = tape_.get(tape_.get(params, "capabilities"), "textDocument");
        const bool client_prepare_rename =
            tape_.bool_val(tape_.get(tape_.get(td_caps, "rename"), "prepareSupport"), false);

        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_obj();
        writer_.key_lit("capabilities");
        writer_.begin_obj();
        writer_.key_lit("textDocumentSync");
        writer_.begin_obj();
        writer_.key_lit("openClose"); writer_.boolean(true);
        writer_.key_lit("change"); writer_.int64(2);
        writer_.end_obj();
        writer_.key_lit("documentSymbolProvider"); writer_.boolean(true);
        writer_.key_lit("definitionProvider"); writer_.boolean(true);
        writer_.key_lit("hoverProvider"); writer_.boolean(true);
        writer_.key_lit("completionProvider");
        writer_.begin_obj();
        writer_.key_lit("triggerCharacters");
        writer_.begin_arr(); writer_.str("."); writer_.end_arr();
        writer_.end_obj();
        writer_.key_lit("signatureHelpProvider");
        writer_.begin_obj();
        writer_.key_lit("triggerCharacters");
        writer_.begin_arr(); writer_.str("("); writer_.str(","); writer_.end_arr();
        writer_.end_obj();
        writer_.key_lit("referencesProvider"); writer_.boolean(true);
        writer_.key_lit("renameProvider");
        if (client_prepare_rename) {
            writer_.begin_obj();
            writer_.key_lit("prepareProvider"); writer_.boolean(true);
            writer_.end_obj();
        } else {
            writer_.boolean(true);
        }
        writer_.key_lit("workspaceSymbolProvider"); writer_.boolean(true);
        writer_.key_lit("codeActionProvider");
        writer_.begin_obj();
        writer_.key_lit("codeActionKinds");
        writer_.begin_arr(); writer_.str("quickfix"); writer_.end_arr();
        writer_.end_obj();
        writer_.key_lit("semanticTokensProvider");
        writer_.begin_obj();
        writer_.key_lit("legend");
        writer_.begin_obj();
        writer_.key_lit("tokenTypes");
        writer_.begin_arr();
        for (const char *tt : {"namespace", "type", "enum", "struct", "parameter",
                               "variable", "property", "enumMember", "function",
                               "method", "keyword", "comment", "string", "number", "operator"}) {
            writer_.str(tt);
        }
        writer_.end_arr();
        writer_.key_lit("tokenModifiers");
        writer_.begin_arr();
        for (const char *tm : {"declaration", "readonly", "modification", "defaultLibrary"}) {
            writer_.str(tm);
        }
        writer_.end_arr();
        writer_.end_obj();
        writer_.key_lit("full"); writer_.boolean(true);
        writer_.end_obj();
        writer_.key_lit("inlayHintProvider"); writer_.boolean(true);
        writer_.key_lit("documentHighlightProvider"); writer_.boolean(true);
        writer_.key_lit("foldingRangeProvider"); writer_.boolean(true);
        writer_.key_lit("documentFormattingProvider"); writer_.boolean(true);
        writer_.end_obj();
        writer_.key_lit("serverInfo");
        writer_.begin_obj();
        writer_.key_lit("name"); writer_.str("brat-lsp");
        writer_.key_lit("version"); writer_.str("0.4");
        writer_.end_obj();
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_document_symbols(std::string_view raw_id, const std::string &uri) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();
        if (st && st->ast_valid) {
            for (const DocSymbol &sym : st->symbols) {
                writer_.begin_obj();
                writer_.key_lit("name"); writer_.str(sym.name);
                writer_.key_lit("kind"); writer_.int64(sym.kind);
                writer_.key_lit("range"); writer_.lsp_range(sym.line, 0, sym.line, sym.line_end_col);
                writer_.key_lit("selectionRange");
                writer_.lsp_range(sym.line, sym.sel_start, sym.line, sym.sel_end);
                writer_.end_obj();
            }
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Resolve imported module file AST, text, and LineIndex from open_docs or WorkspaceDB.
    struct ResolvedModule {
        std::string path;
        std::string ns;
        std::string_view text;
        const LineIndex *lines = nullptr;
        const std::vector<StmtP> *body = nullptr;
        const std::vector<Decl> *decls = nullptr;
    };

    std::vector<ResolvedModule> resolve_imports_cached(const std::string &path,
                                                       const std::string &text) {
        std::vector<ResolvedModule> out;
        if (path.empty()) return out;
        try {
            ModuleSet mods = load_modules(path, text);
            for (size_t i = 1; i < mods.files.size(); ++i) {
                const SourceFile &sf = mods.files[i];
                if (!sf.ns) continue;
                const std::string dep_uri = "file://" + sf.path;
                if (const DocState *od = find_doc(dep_uri); od && od->ast_valid) {
                    ensure_decls(*od);
                    out.push_back({sf.path, *sf.ns, od->text, &od->lines, &od->ast_body, &od->decls});
                } else if (const DiskFileCache *dc = ws_db_.get_file(sf.path); dc && dc->parse_ok) {
                    out.push_back({sf.path, *sf.ns, dc->text, &dc->lines, &dc->ast_body, &dc->decls});
                }
            }
        } catch (...) {}
        return out;
    }

    void write_definition(std::string_view raw_id, const std::string &uri, int line, int character) {
        const DocState *st = find_doc(uri);
        if (!st) { send_null_result(raw_id); return; }
        const std::string path = path_from_uri(uri);
        const Word word = word_at(st->text, st->lines, line, character);
        if (word.name.empty()) { send_null_result(raw_id); return; }

        if (line >= 0 && static_cast<size_t>(line) < st->lines.line_count()) {
            std::string_view cur_ln = st->lines.line_view(st->text, line);
            while (!cur_ln.empty() && (cur_ln.front() == ' ' || cur_ln.front() == '\t'))
                cur_ln.remove_prefix(1);
            if (cur_ln.rfind("import ", 0) == 0) {
                const auto mods = resolve_imports_cached(path, st->text);
                const std::string want_ns = word.full_qualifier.empty()
                    ? word.name : dots_to_ns(word.full_qualifier + "." + word.name);
                for (const ResolvedModule &rm : mods) {
                    const size_t tail = rm.ns.rfind("__");
                    if (rm.ns == want_ns ||
                        (tail != std::string::npos && rm.ns.substr(tail + 2) == word.name)) {
                        writer_.reset();
                        writer_.begin_obj();
                        writer_.key_lit("jsonrpc"); writer_.str("2.0");
                        writer_.key_lit("id"); writer_.raw_value(raw_id);
                        writer_.key_lit("result");
                        writer_.begin_obj();
                        writer_.key_lit("uri"); writer_.str("file://" + rm.path);
                        writer_.key_lit("range"); writer_.lsp_range(0, 0, 0, 0);
                        writer_.end_obj();
                        writer_.end_obj();
                        lsp_wire::send_frame(writer_.view());
                        return;
                    }
                }
            }
        }

        if (!st->ast_valid) { send_null_result(raw_id); return; }
        ensure_decls(*st);

        std::optional<Decl> best;
        if (word.qualifier.empty()) {
            for (const Decl &d : st->decls)
                if (d.name == word.name && d.line - 1 <= line && (!best || d.line > best->line))
                    best = d;
            if (!best) {
                for (const Decl &d : st->decls)
                    if (d.name == word.name) { best = d; break; }
            }
        }

        std::string target_path = path;
        std::string_view target_text = st->text;
        const LineIndex *target_lines = &st->lines;

        if (!best && !word.qualifier.empty()) {
            const auto mods = resolve_imports_cached(path, st->text);
            for (const ResolvedModule &rm : mods) {
                if (!ns_matches_word(rm.ns, word)) continue;
                bool exported = false;
                for (const Decl &d : *rm.decls) {
                    if (d.name == word.name && d.is_pub) {
                        best = d;
                        exported = true;
                        break;
                    }
                }
                if (exported && best) {
                    target_path = rm.path;
                    target_text = rm.text;
                    target_lines = rm.lines;
                    break;
                }
            }
        }

        if (!best && !word.qualifier.empty()) {
            for (const Decl &d : st->decls)
                if (d.name == word.name && !d.container.empty()) { best = d; break; }
        }
        if (!best || !target_lines) { send_null_result(raw_id); return; }

        const int tline = std::max(0, best->line - 1);
        if (static_cast<size_t>(tline) >= target_lines->line_count()) {
            send_null_result(raw_id);
            return;
        }
        const std::string_view ln_sv = target_lines->line_view(target_text, tline);
        const size_t bcol = ln_sv.find(best->name);
        const int col = (bcol == std::string_view::npos)
            ? 0 : target_lines->byte_to_utf16_col(target_text, tline, bcol);
        const int ecol = col + LineIndex::utf16_len(best->name, false);

        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_obj();
        writer_.key_lit("uri"); writer_.str("file://" + target_path);
        writer_.key_lit("range"); writer_.lsp_range(tline, col, tline, ecol);
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_hover(std::string_view raw_id, const std::string &uri, int line, int character) {
        const DocState *st = find_doc(uri);
        if (!st) { send_null_result(raw_id); return; }
        const Word word = word_at(st->text, st->lines, line, character);
        if (word.name.empty()) { send_null_result(raw_id); return; }

        auto send_md = [&](std::string_view md) {
            writer_.reset();
            writer_.begin_obj();
            writer_.key_lit("jsonrpc"); writer_.str("2.0");
            writer_.key_lit("id"); writer_.raw_value(raw_id);
            writer_.key_lit("result");
            writer_.begin_obj();
            writer_.key_lit("contents");
            writer_.begin_obj();
            writer_.key_lit("kind"); writer_.str("markdown");
            writer_.key_lit("value"); writer_.str(md);
            writer_.end_obj();
            writer_.key_lit("range"); writer_.lsp_range(line, word.start, line, word.end);
            writer_.end_obj();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
        };

        const std::string path = path_from_uri(uri);
        if (!word.qualifier.empty()) {
            const auto mods = resolve_imports_cached(path, st->text);
            for (const ResolvedModule &rm : mods) {
                if (!ns_matches_word(rm.ns, word)) continue;
                for (const auto &sp : *rm.body) {
                    if (!sp || sp->k != SK::Pub || sp->body.empty()) continue;
                    const Stmt &inner = *sp->body.front();
                    if ((inner.k == SK::Fn || inner.k == SK::ExportFn) && !inner.fns.empty() &&
                        inner.fns.front().name == word.name) {
                        std::string md = "```brat\n" + format_fn_sig(inner.fns.front(), true) + "\n```";
                        const std::string doc = extract_doc_comment(rm.text, *rm.lines, sp->line);
                        if (!doc.empty()) md += "\n\n" + doc;
                        std::filesystem::path bri = std::filesystem::path(rm.path);
                        bri.replace_extension(".bri");
                        if (std::filesystem::is_regular_file(bri))
                            md += "\nInterface contract: `" + bri.filename().string() + "`";
                        send_md(md);
                        return;
                    }
                    if (inner.k == SK::Struct && inner.a == word.name) {
                        std::string md = "```brat\npub struct " + inner.a + " {\n";
                        for (const auto &f : inner.fields)
                            md += "  " + f.name + ": " + f.ann.value_or("dyn") + ",\n";
                        md += "}\n```";
                        const std::string doc = extract_doc_comment(rm.text, *rm.lines, sp->line);
                        if (!doc.empty()) md += "\n\n" + doc;
                        send_md(md);
                        return;
                    }
                    if (inner.k == SK::Enum) {
                        if (inner.a == word.name) {
                            std::string md = format_enum_hover(inner, true);
                            const std::string doc = extract_doc_comment(rm.text, *rm.lines, sp->line);
                            if (!doc.empty()) md += "\n\n" + doc;
                            send_md(md);
                            return;
                        }
                        for (const auto &v : inner.variants) {
                            if (v.name == word.name) {
                                send_md("```brat\n" + format_enum_variant_sig(v) + " -> " + inner.a + "\n```");
                                return;
                            }
                        }
                    }
                }
            }
        }

        if (st->ast_valid) {
            for (const auto &sp : st->ast_body) {
                if (!sp) continue;
                const bool is_pub = (sp->k == SK::Pub && !sp->body.empty());
                const Stmt &s = is_pub ? *sp->body.front() : *sp;
                const int doc_line = sp->line > 0 ? sp->line : s.line;
                if ((s.k == SK::Fn || s.k == SK::ExportFn) && !s.fns.empty() &&
                    s.fns.front().name == word.name) {
                    std::string md = "```brat\n" + format_fn_sig(s.fns.front(), is_pub) + "\n```";
                    const std::string doc = extract_doc_comment(st->text, st->lines, doc_line);
                    if (!doc.empty()) md += "\n\n" + doc;
                    send_md(md);
                    return;
                }
                if (s.k == SK::Impl) {
                    for (const FnDecl &fn : s.fns) {
                        if (fn.name == word.name) {
                            send_md("```brat\nimpl " + s.a + " { " + format_fn_sig(fn) + " }\n```");
                            return;
                        }
                    }
                }
                if (s.k == SK::Extern && s.ext.name == word.name) {
                    std::string md = format_extern_sig(s.ext);
                    const std::string doc = extract_doc_comment(st->text, st->lines, doc_line);
                    if (!doc.empty()) md += "\n\n" + doc;
                    send_md(md);
                    return;
                }
                if (s.k == SK::ExternStruct && s.a == word.name) {
                    const bool is_union = (s.opt1 && *s.opt1 == "union");
                    std::string md = std::string("```brat\nextern ") +
                                     (is_union ? "union " : "struct ") + s.a + " {\n";
                    for (const auto &f : s.fields) {
                        md += "  " + f.name + ": " + f.ann.value_or("dyn") + "  // " +
                              c_abi_note(f.ann.value_or("dyn")) +
                              (is_union ? " @ offset 0" : "") + "\n";
                    }
                    md += "}\n```";
                    const std::string doc = extract_doc_comment(st->text, st->lines, doc_line);
                    if (!doc.empty()) md += "\n\n" + doc;
                    send_md(md);
                    return;
                }
                if (s.k == SK::Struct && s.a == word.name) {
                    std::string md = std::string("```brat\n") +
                                     (sp->k == SK::Pub ? "pub struct " : "struct ") + s.a + " {\n";
                    for (const auto &f : s.fields) {
                        md += "  " + f.name + ": " + f.ann.value_or("dyn") + ",\n";
                    }
                    md += "}\n```";
                    const std::string doc = extract_doc_comment(st->text, st->lines, doc_line);
                    if (!doc.empty()) md += "\n\n" + doc;
                    send_md(md);
                    return;
                }
                if (s.k == SK::Enum) {
                    if (s.a == word.name) {
                        std::string md = format_enum_hover(s, is_pub);
                        const std::string doc = extract_doc_comment(st->text, st->lines, doc_line);
                        if (!doc.empty()) md += "\n\n" + doc;
                        send_md(md);
                        return;
                    }
                    for (const auto &v : s.variants) {
                        if (v.name == word.name) {
                            send_md("```brat\n" + format_enum_variant_sig(v) + " -> " + s.a + "\n```");
                            return;
                        }
                    }
                }
            }
            if (auto lh = find_let_hover(st->ast_body, word.name, line)) {
                send_md(*lh);
                return;
            }
        }

        const auto &bdocs = builtin_docs();
        if (auto it = bdocs.find(word.name); it != bdocs.end()) {
            send_md("```brat\n" + it->second + "\n```");
            return;
        }
        send_null_result(raw_id);
    }

    void write_completions(std::string_view raw_id, const std::string &uri, int line, int character) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();

        if (!st || line < 0 || static_cast<size_t>(line) >= st->lines.line_count()) {
            writer_.end_arr();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
            return;
        }

        const std::string_view cur = st->lines.line_view(st->text, line);
        const size_t col_byte = st->lines.clamp_col_in_line(st->text, line, character);
        size_t id_start = col_byte;
        while (id_start > 0 && is_ident_char(cur[id_start - 1])) --id_start;
        const std::string_view prefix = cur.substr(id_start, col_byte - id_start);

        std::string qual;
        if (id_start > 0 && cur[id_start - 1] == '.') {
            size_t q = id_start - 1;
            while (q > 0 && (is_ident_char(cur[q - 1]) || cur[q - 1] == '.')) --q;
            qual.assign(cur.data() + q, id_start - 1 - q);
        }

        std::set<std::string> seen;
        auto add_item = [&](std::string_view label, int kind, std::string_view detail) {
            if (!prefix.empty() && label.rfind(prefix, 0) != 0) return;
            if (!seen.emplace(label).second) return;
            writer_.begin_obj();
            writer_.key_lit("label"); writer_.str(label);
            writer_.key_lit("kind"); writer_.int64(kind);
            writer_.key_lit("detail"); writer_.str(detail);
            writer_.end_obj();
        };

        if (qual == "std") {
            for (const char *m : {"bytes", "cli", "crypto", "csv", "env", "fs",
                                  "http", "json", "path", "proc", "sqlite", "time"}) {
                add_item(m, 9, std::string("std.") + m);
            }
            writer_.end_arr();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
            return;
        }

        if (!qual.empty()) {
            const auto mods = resolve_imports_cached(path_from_uri(uri), st->text);
            const std::string want_ns = dots_to_ns(qual);
            bool matched_mod = false;
            for (const ResolvedModule &rm : mods) {
                const size_t tail = rm.ns.rfind("__");
                if (rm.ns != want_ns && (tail == std::string::npos || rm.ns.substr(tail + 2) != qual))
                    continue;
                matched_mod = true;
                for (const auto &sp : *rm.body) {
                    if (!sp || sp->k != SK::Pub || sp->body.empty()) continue;
                    const Stmt &inner = *sp->body.front();
                    if ((inner.k == SK::Fn || inner.k == SK::ExportFn) && !inner.fns.empty() &&
                        inner.fns.front().name) {
                        add_item(*inner.fns.front().name, 3, format_fn_sig(inner.fns.front(), true));
                    } else if ((inner.k == SK::Let || inner.k == SK::LetTmp) && inner.konst) {
                        add_item(inner.a, 21, "pub const " + inner.a);
                    } else if (inner.k == SK::Struct) {
                        add_item(inner.a, 22, "pub struct " + inner.a);
                    } else if (inner.k == SK::Enum) {
                        add_item(inner.a, 13, "pub enum " + inner.a);
                        for (const auto &v : inner.variants)
                            add_item(v.name, 20, format_enum_variant_sig(v) + " -> " + inner.a);
                    }
                }
            }
            if (!matched_mod) {
                const std::vector<StmtP> *body_ptr = st->ast_valid ? &st->ast_body : nullptr;
                ParseOut fallback;
                if (!body_ptr) {
                    try { fallback = parse(st->text); body_ptr = &fallback.body; } catch (...) {
                        std::string patched = st->text;
                        const size_t loff = st->lines.line_start_offset(st->text, line);
                        const size_t dot_pos = loff + id_start - 1;
                        if (dot_pos < patched.size() && patched[dot_pos] == '.') {
                            for (size_t k = dot_pos; k < loff + col_byte && k < patched.size(); ++k)
                                patched[k] = ' ';
                            try { fallback = parse(patched); body_ptr = &fallback.body; } catch (...) {}
                        }
                    }
                }
                std::string recv_ty;
                bool static_struct = false;
                std::vector<std::string> qual_parts;
                {
                    size_t s0 = 0;
                    while (s0 <= qual.size()) {
                        size_t d = qual.find('.', s0);
                        if (d == std::string::npos) {
                            qual_parts.push_back(qual.substr(s0));
                            break;
                        }
                        qual_parts.push_back(qual.substr(s0, d - s0));
                        s0 = d + 1;
                    }
                }
                const std::string root_qual = qual_parts.empty() ? qual : qual_parts[0];
                auto check_static_decl = [&](const std::vector<StmtP> &b) {
                    if (qual_parts.size() != 1) return;
                    for (const auto &sp : b) {
                        if (!sp) continue;
                        const Stmt &s = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body.front() : *sp;
                        if ((s.k == SK::Struct || s.k == SK::ExternStruct || s.k == SK::Enum) && s.a == qual)
                            static_struct = true;
                    }
                };
                auto find_var_ty = [&](const std::vector<StmtP> &b) {
                    std::function<void(const std::vector<StmtP> &)> walk = [&](const std::vector<StmtP> &ss) {
                        for (const auto &sp : ss) {
                            if (!sp) continue;
                            const Stmt &s = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body.front() : *sp;
                            if (s.k == SK::Fn || s.k == SK::ExportFn || s.k == SK::Impl) {
                                for (const FnDecl &fn : s.fns) {
                                    for (const Param &p : fn.params) {
                                        if (p.name == root_qual && s.line - 1 <= line) {
                                            if (p.ann && !p.ann->empty()) recv_ty = *p.ann;
                                            else if (s.k == SK::Impl && p.name == "self") recv_ty = s.a;
                                        }
                                    }
                                    walk(fn.body);
                                }
                            } else if ((s.k == SK::Let || s.k == SK::LetTmp) && s.a == root_qual && s.line - 1 <= line) {
                                if (s.ann && !s.ann->empty()) recv_ty = *s.ann;
                                else if (s.e1) recv_ty = infer_expr_type_hint(*s.e1, b);
                            } else if (s.k == SK::ForRange && s.a == root_qual && s.line - 1 <= line) {
                                recv_ty = "i64";
                            } else if (s.k == SK::ForIter && s.a == root_qual && s.line - 1 <= line && s.e1) {
                                std::string cty;
                                if (s.e1->k == EK::Var) {
                                    const std::string saved_rq = recv_ty;
                                    // Check vector annotation if declared earlier.
                                    for (const auto &top : b) {
                                        if (!top) continue;
                                        const Stmt &ts = (top->k == SK::Pub && !top->body.empty()) ? *top->body.front() : *top;
                                        if (ts.k == SK::Fn || ts.k == SK::ExportFn || ts.k == SK::Impl) {
                                            for (const FnDecl &fn : ts.fns) {
                                                for (const Param &p : fn.params)
                                                    if (p.name == s.e1->a && p.ann) cty = *p.ann;
                                                for (const auto &bs : fn.body)
                                                    if (bs && (bs->k == SK::Let || bs->k == SK::LetTmp) &&
                                                        bs->a == s.e1->a && bs->ann)
                                                        cty = *bs->ann;
                                            }
                                        }
                                    }
                                    (void)saved_rq;
                                }
                                if (cty.rfind("vec[", 0) == 0 && cty.back() == ']')
                                    recv_ty = cty.substr(4, cty.size() - 5);
                            }
                            walk(s.body);
                            walk(s.body2);
                            if (s.els) walk(s.els->body);
                        }
                    };
                    walk(b);
                };
                if (body_ptr) {
                    check_static_decl(*body_ptr);
                    for (const ResolvedModule &rm : mods) check_static_decl(*rm.body);
                    if (!static_struct) find_var_ty(*body_ptr);
                }
                auto resolve_field_ty = [&](const std::string &sty, const std::string &fname) -> std::string {
                    auto scan_b = [&](const std::vector<StmtP> &b) -> std::string {
                        for (const auto &sp : b) {
                            if (!sp) continue;
                            const Stmt &s = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body.front() : *sp;
                            if ((s.k == SK::Struct || s.k == SK::ExternStruct) && s.a == sty) {
                                for (const auto &f : s.fields)
                                    if (f.name == fname && f.ann) return *f.ann;
                            }
                        }
                        return {};
                    };
                    if (body_ptr) {
                        std::string r = scan_b(*body_ptr);
                        if (!r.empty()) return r;
                    }
                    for (const ResolvedModule &rm : mods) {
                        std::string r = scan_b(*rm.body);
                        if (!r.empty()) return r;
                    }
                    return {};
                };
                if (!static_struct && !recv_ty.empty() && qual_parts.size() > 1) {
                    for (size_t qi = 1; qi < qual_parts.size() && !recv_ty.empty(); ++qi) {
                        recv_ty = resolve_field_ty(recv_ty, qual_parts[qi]);
                    }
                }
                bool matched_struct = false;
                bool is_native_struct = false;
                if (!static_struct && (recv_ty.rfind("Option", 0) == 0 || recv_ty.rfind("Result", 0) == 0)) {
                    matched_struct = true;
                    add_item("unwrap", 2, "extract payload or raise");
                    add_item("unwrap_or", 2, "extract payload or fallback");
                    if (recv_ty.rfind("Result", 0) == 0) {
                        add_item("is_ok", 2, "Result.is_ok() -> bool");
                        add_item("is_err", 2, "Result.is_err() -> bool");
                    } else {
                        add_item("is_some", 2, "Option.is_some() -> bool");
                        add_item("is_none", 2, "Option.is_none() -> bool");
                    }
                }
                auto emit_struct_members = [&](const std::vector<StmtP> &b) {
                    for (const auto &sp : b) {
                        if (!sp) continue;
                        const Stmt &s = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body.front() : *sp;
                        if (static_struct && s.k == SK::Enum && s.a == qual) {
                            matched_struct = true;
                            for (const auto &v : s.variants)
                                add_item(v.name, 20, format_enum_variant_sig(v) + " -> " + s.a);
                        } else if (static_struct && s.k == SK::Impl && s.a == qual) {
                            for (const FnDecl &fn : s.fns) {
                                const bool has_self = !fn.params.empty() && fn.params[0].name == "self";
                                if (fn.name && !has_self) {
                                    matched_struct = true;
                                    add_item(*fn.name, 3, "impl " + s.a + ": " + format_fn_sig(fn));
                                }
                            }
                        } else if (!recv_ty.empty()) {
                            if ((s.k == SK::Struct || s.k == SK::ExternStruct) && s.a == recv_ty) {
                                matched_struct = true;
                                if (s.k == SK::Struct) is_native_struct = true;
                                for (const auto &f : s.fields)
                                    add_item(f.name, 5, s.a + "." + f.name + ": " + f.ann.value_or("dyn"));
                            } else if (s.k == SK::Impl && s.a == recv_ty) {
                                matched_struct = true;
                                for (const FnDecl &fn : s.fns) {
                                    const bool has_self = !fn.params.empty() && fn.params[0].name == "self";
                                    if (fn.name && has_self)
                                        add_item(*fn.name, 2, "impl " + s.a + ": " + format_fn_sig(fn));
                                }
                            }
                        }
                    }
                };
                if (body_ptr && (static_struct || !recv_ty.empty())) {
                    emit_struct_members(*body_ptr);
                    for (const ResolvedModule &rm : mods) emit_struct_members(*rm.body);
                    if (is_native_struct && !static_struct) add_item("copy", 2, "shallow copy struct");
                }
                if (!matched_struct) {
                    if (body_ptr) {
                        for (const auto &sp : *body_ptr) {
                            if (!sp) continue;
                            const Stmt &s = (sp->k == SK::Pub && !sp->body.empty()) ? *sp->body.front() : *sp;
                            if (s.k == SK::Impl) {
                                for (const FnDecl &fn : s.fns)
                                    if (fn.name) add_item(*fn.name, 2, "impl " + s.a + ": " + format_fn_sig(fn));
                            } else if (s.k == SK::ExternStruct || s.k == SK::Struct) {
                                for (const auto &f : s.fields)
                                    add_item(f.name, 5, s.a + "." + f.name + ": " + f.ann.value_or("dyn"));
                            }
                        }
                    }
                    for (const char *m : {"push", "pop", "slice", "split", "join", "trim", "lower", "upper",
                                          "contains", "starts_with", "ends_with", "index_of", "replace",
                                          "lines", "chars", "parse_int", "parse_float", "keys", "has",
                                          "get", "set", "del", "map", "filter", "fold", "sort", "sort_by",
                                          "unwrap", "unwrap_or", "is_ok", "is_err", "is_some", "is_none"}) {
                        add_item(m, 2, "builtin method");
                    }
                }
            }
            writer_.end_arr();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
            return;
        }

        if (st->ast_valid) {
            ensure_decls(*st);
            for (const Decl &d : st->decls) {
                const int k = d.container == "type" ? 22 : (d.container == "extern" ? 3 : 6);
                add_item(d.name, k, d.container.empty() ? "binding/fn" : d.container);
            }
            for (const auto &sp : st->ast_body) {
                if (sp && sp->k == SK::Import && !sp->names.empty())
                    add_item(sp->names.back(), 9, "imported module");
            }
        }
        for (const auto &[bname, bdoc] : builtin_docs()) add_item(bname, 3, bdoc);
        add_item("std", 9, "BRAT standard library namespace");
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_signature_help(std::string_view raw_id, const std::string &uri, int line, int character) {
        const DocState *st = find_doc(uri);
        if (!st || line < 0 || static_cast<size_t>(line) >= st->lines.line_count()) {
            send_null_result(raw_id);
            return;
        }

        std::string prefix;
        prefix.reserve(512);
        for (int l = std::max(0, line - 8); l <= line; ++l) {
            const std::string_view sv = st->lines.line_view(st->text, l);
            if (l < line) {
                prefix.append(sv.data(), sv.size());
                prefix.push_back('\n');
            } else {
                const size_t cbyte = st->lines.clamp_col_in_line(st->text, l, std::max(0, character));
                prefix.append(sv.data(), cbyte);
            }
        }

        int depth = 0;
        int active_param = 0;
        int open_pos = -1;
        for (int i = static_cast<int>(prefix.size()) - 1; i >= 0; --i) {
            const char ch = prefix[i];
            if (ch == ')' || ch == ']' || ch == '}') depth++;
            else if (ch == '(') {
                if (depth == 0) { open_pos = i; break; }
                depth--;
            } else if ((ch == '[' || ch == '{') && depth > 0) {
                depth--;
            } else if (ch == ',' && depth == 0) {
                active_param++;
            }
        }
        if (open_pos <= 0) { send_null_result(raw_id); return; }

        int end_id = open_pos;
        while (end_id > 0 && std::isspace(static_cast<unsigned char>(prefix[end_id - 1]))) end_id--;
        int start_id = end_id;
        while (start_id > 0 && is_ident_char(prefix[start_id - 1])) start_id--;
        if (start_id == end_id) { send_null_result(raw_id); return; }

        const std::string fn_name = prefix.substr(start_id, end_id - start_id);
        std::string qual;
        if (start_id >= 2 && prefix[start_id - 1] == '.') {
            int qe = start_id - 1, qs = qe;
            while (qs > 0 && (is_ident_char(prefix[qs - 1]) || prefix[qs - 1] == '.')) qs--;
            qual = prefix.substr(qs, qe - qs);
        }

        auto emit_sig = [&](std::string_view label, const std::vector<std::string> &plabels,
                            std::string_view doc) {
            const int clamped = plabels.empty()
                ? 0 : std::min<int>(active_param, static_cast<int>(plabels.size()) - 1);
            writer_.reset();
            writer_.begin_obj();
            writer_.key_lit("jsonrpc"); writer_.str("2.0");
            writer_.key_lit("id"); writer_.raw_value(raw_id);
            writer_.key_lit("result");
            writer_.begin_obj();
            writer_.key_lit("signatures");
            writer_.begin_arr();
            writer_.begin_obj();
            writer_.key_lit("label"); writer_.str(label);
            writer_.key_lit("parameters");
            writer_.begin_arr();
            for (const std::string &pl : plabels) {
                writer_.begin_obj();
                writer_.key_lit("label"); writer_.str(pl);
                writer_.end_obj();
            }
            writer_.end_arr();
            if (!doc.empty()) {
                writer_.key_lit("documentation"); writer_.str(doc);
            }
            writer_.end_obj();
            writer_.end_arr();
            writer_.key_lit("activeSignature"); writer_.int64(0);
            writer_.key_lit("activeParameter"); writer_.int64(clamped);
            writer_.end_obj();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
        };

        auto emit_fndecl = [&](const FnDecl &fn, std::string_view doc) {
            std::vector<std::string> plabels;
            std::string label = "fn " + fn.name.value_or(fn_name) + "(";
            for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i) label += ", ";
                std::string pl;
                if (fn.params[i].mode == Mode::Mut) pl += "mut ";
                else if (fn.params[i].mode == Mode::Move) pl += "move ";
                pl += fn.params[i].name + ": " + fn.params[i].ann.value_or("dyn");
                plabels.push_back(pl);
                label += pl;
            }
            label += ") -> " + fn.ret.value_or("none");
            emit_sig(label, plabels, doc);
        };

        if (!qual.empty()) {
            Word w;
            w.qualifier = qual;
            w.name = fn_name;
            const auto mods = resolve_imports_cached(path_from_uri(uri), st->text);
            for (const ResolvedModule &rm : mods) {
                if (!ns_matches_word(rm.ns, w)) continue;
                for (const auto &sp : *rm.body) {
                    if (!sp) continue;
                    const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                    if (s && (s->k == SK::Fn || s->k == SK::ExportFn) && !s->fns.empty() &&
                        s->fns.front().name == fn_name) {
                        emit_fndecl(s->fns.front(), extract_doc_comment(rm.text, *rm.lines, sp->line));
                        return;
                    }
                }
            }
        } else {
            const std::vector<StmtP> *body_ptr = st->ast_valid ? &st->ast_body : nullptr;
            ParseOut fallback;
            if (!body_ptr) {
                try { fallback = parse(st->text); body_ptr = &fallback.body; } catch (...) {}
            }
            if (body_ptr) {
                for (const auto &sp : *body_ptr) {
                    if (!sp) continue;
                    const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                    if (!s) continue;
                    if ((s->k == SK::Fn || s->k == SK::ExportFn) && !s->fns.empty() &&
                        s->fns.front().name == fn_name) {
                        emit_fndecl(s->fns.front(), extract_doc_comment(st->text, st->lines, sp->line));
                        return;
                    }
                    if (s->k == SK::Extern && s->ext.name == fn_name) {
                        std::vector<std::string> plabels;
                        std::string label = "extern fn " + s->ext.name + "(";
                        for (size_t i = 0; i < s->ext.params.size(); ++i) {
                            if (i) label += ", ";
                            std::string pl = s->ext.params[i].name + ": " + s->ext.params[i].ann.value_or("dyn");
                            plabels.push_back(pl);
                            label += pl;
                        }
                        label += ") -> " + s->ext.ret.value_or("none");
                        emit_sig(label, plabels, "");
                        return;
                    }
                }
            }
            const auto &bdocs = builtin_docs();
            if (auto bit = bdocs.find(fn_name); bit != bdocs.end()) {
                const std::string &full = bit->second;
                const size_t lp = full.find('(');
                const size_t rp = full.find(')', lp == std::string::npos ? 0 : lp);
                std::vector<std::string> plabels;
                if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
                    const std::string inside = full.substr(lp + 1, rp - lp - 1);
                    size_t pos = 0;
                    while (pos < inside.size()) {
                        const size_t comma = inside.find(',', pos);
                        std::string part = inside.substr(
                            pos, comma == std::string::npos ? std::string::npos : comma - pos);
                        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                        if (!part.empty()) plabels.push_back(part);
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
                emit_sig(full, plabels, full);
                return;
            }
        }
        send_null_result(raw_id);
    }

    void write_references(std::string_view raw_id, const std::string &uri,
                          int line, int character, bool include_decl) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();

        if (st) {
            const Word word = word_at(st->text, st->lines, line, character);
            if (!word.name.empty()) {
                const bool qual_ok = !word.qualifier.empty();
                auto emit_occs = [&](std::string_view doc_uri, const std::vector<SymOccur> &occs) {
                    for (const SymOccur &occ : occs) {
                        if (!include_decl && occ.is_decl) continue;
                        writer_.begin_obj();
                        writer_.key_lit("uri"); writer_.str(doc_uri);
                        writer_.key_lit("range");
                        writer_.lsp_range(occ.line, occ.start_col, occ.line, occ.end_col);
                        writer_.end_obj();
                    }
                };
                emit_occs(uri, find_symbol_occurrences_fast(st->text, st->lines, word.name, qual_ok));
                std::set<std::string> visited_paths;
                const std::string self_path = path_from_uri(uri);
                if (!self_path.empty()) {
                    std::error_code ec;
                    visited_paths.insert(std::filesystem::weakly_canonical(self_path, ec).string());
                }
                for (const auto &[other_uri, other_st] : docs_) {
                    if (other_uri == uri) continue;
                    const std::string op = path_from_uri(other_uri);
                    if (op.empty()) continue;
                    std::error_code ec;
                    visited_paths.insert(std::filesystem::weakly_canonical(op, ec).string());
                    emit_occs(other_uri,
                              find_symbol_occurrences_fast(other_st.text, other_st.lines, word.name, true));
                }
                // Check indexed workspace files for top-level / exported symbols.
                bool is_top_sym = qual_ok;
                for (const DocSymbol &ds : st->symbols)
                    if (ds.name == word.name) { is_top_sym = true; break; }
                if (is_top_sym) {
                    ws_db_.ensure_roots_indexed();
                    for (const std::string &wp : ws_db_.workspace_files()) {
                        if (!visited_paths.insert(wp).second) continue;
                        const DiskFileCache *dc = ws_db_.get_file(wp);
                        if (!dc) continue;
                        emit_occs("file://" + wp,
                                  find_symbol_occurrences_fast(dc->text, dc->lines, word.name, true));
                    }
                }
            }
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_prepare_rename(std::string_view raw_id, const std::string &uri, int line, int character) {
        const DocState *st = find_doc(uri);
        if (!st) { send_null_result(raw_id); return; }
        const Word word = word_at(st->text, st->lines, line, character);
        if (word.name.empty() || is_keyword(word.name)) { send_null_result(raw_id); return; }
        if (find_symbol_occurrences_fast(st->text, st->lines, word.name, !word.qualifier.empty()).empty()) {
            send_null_result(raw_id);
            return;
        }
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_obj();
        writer_.key_lit("range"); writer_.lsp_range(line, word.start, line, word.end);
        writer_.key_lit("placeholder"); writer_.str(word.name);
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_rename(std::string_view raw_id, const std::string &uri,
                      int line, int character, std::string_view new_name) {
        const DocState *st = find_doc(uri);
        if (!st || !valid_brat_ident(new_name)) {
            send_error(raw_id, -32602, "invalid rename target or closed document");
            return;
        }
        const Word word = word_at(st->text, st->lines, line, character);
        if (word.name.empty() || is_keyword(word.name)) {
            send_null_result(raw_id);
            return;
        }
        const auto self_occs = find_symbol_occurrences_fast(
            st->text, st->lines, word.name, !word.qualifier.empty());
        if (self_occs.empty()) {
            send_null_result(raw_id);
            return;
        }

        // Cross-file rename when renaming a `pub` symbol.
        bool is_pub_sym = !word.qualifier.empty();
        for (const DocSymbol &ds : st->symbols) {
            if (ds.name == word.name && ds.is_pub) { is_pub_sym = true; break; }
        }

        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_obj();
        writer_.key_lit("changes");
        writer_.begin_obj();

        auto emit_file_edits = [&](std::string_view f_uri, const std::vector<SymOccur> &occs) {
            if (occs.empty()) return;
            writer_.key(f_uri);
            writer_.begin_arr();
            for (const SymOccur &occ : occs) {
                writer_.begin_obj();
                writer_.key_lit("range");
                writer_.lsp_range(occ.line, occ.start_col, occ.line, occ.end_col);
                writer_.key_lit("newText"); writer_.str(new_name);
                writer_.end_obj();
            }
            writer_.end_arr();
        };

        emit_file_edits(uri, self_occs);
        if (is_pub_sym) {
            std::set<std::string> visited_paths;
            const std::string self_path = path_from_uri(uri);
            if (!self_path.empty()) {
                std::error_code ec;
                visited_paths.insert(std::filesystem::weakly_canonical(self_path, ec).string());
            }
            for (const auto &[other_uri, other_st] : docs_) {
                if (other_uri == uri) continue;
                const std::string op = path_from_uri(other_uri);
                if (!op.empty()) {
                    std::error_code ec;
                    visited_paths.insert(std::filesystem::weakly_canonical(op, ec).string());
                }
                const auto o_occs = find_symbol_occurrences_fast(
                    other_st.text, other_st.lines, word.name, true);
                emit_file_edits(other_uri, o_occs);
            }
            std::error_code std_ec;
            const std::string std_canon =
                std::filesystem::weakly_canonical("lang/std", std_ec).string();
            ws_db_.ensure_roots_indexed();
            for (const std::string &wp : ws_db_.workspace_files()) {
                if (!visited_paths.insert(wp).second) continue;
                if (!std_canon.empty() && wp.rfind(std_canon, 0) == 0) continue;
                if (wp.find("/vendor/") != std::string::npos) continue;
                const DiskFileCache *dc = ws_db_.get_file(wp);
                if (!dc) continue;
                emit_file_edits("file://" + wp,
                                find_symbol_occurrences_fast(dc->text, dc->lines, word.name, true));
            }
        }

        writer_.end_obj();
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    void write_workspace_symbols(std::string_view raw_id, std::string_view query) {
        for (const auto &[uri, _] : docs_) {
            const std::string p = path_from_uri(uri);
            if (!p.empty()) {
                std::error_code ec;
                const auto cp = std::filesystem::weakly_canonical(p, ec);
                if (!ec && cp.has_parent_path()) ws_db_.add_root(cp.parent_path());
            }
        }
        ws_db_.ensure_roots_indexed();

        const std::string qlow = to_lower_ascii(query);
        struct Hit {
            int score;
            std::string uri;
            const DocSymbol *sym;
        };
        std::vector<Hit> hits;
        hits.reserve(256);

        std::set<std::string> open_canon;
        for (const auto &[uri, st] : docs_) {
            const std::string p = path_from_uri(uri);
            if (!p.empty()) {
                std::error_code ec;
                open_canon.insert(std::filesystem::weakly_canonical(p, ec).string());
            }
            for (const DocSymbol &sym : st.symbols) {
                const int sc = fuzzy_score(qlow, sym.lower_name);
                if (sc > 0) hits.push_back({sc, uri, &sym});
            }
        }

        for (const auto &[cpath, entry] : ws_db_.cache_map()) {
            if (!entry.parse_ok || open_canon.count(cpath)) continue;
            const std::string f_uri = "file://" + cpath;
            for (const DocSymbol &sym : entry.symbols) {
                const int sc = fuzzy_score(qlow, sym.lower_name);
                if (sc > 0) hits.push_back({sc, f_uri, &sym});
            }
        }

        std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.sym->name < b.sym->name;
        });
        if (hits.size() > 256) hits.resize(256);

        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();
        for (const Hit &h : hits) {
            writer_.begin_obj();
            writer_.key_lit("name"); writer_.str(h.sym->name);
            writer_.key_lit("kind"); writer_.int64(h.sym->kind);
            writer_.key_lit("location");
            writer_.begin_obj();
            writer_.key_lit("uri"); writer_.str(h.uri);
            writer_.key_lit("range");
            writer_.lsp_range(h.sym->line, h.sym->sel_start, h.sym->line, h.sym->sel_end);
            writer_.end_obj();
            writer_.end_obj();
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Quick-fix code actions (`textDocument/codeAction`)
    void write_code_actions(std::string_view raw_id, const std::string &uri,
                            int start_line, int end_line, uint32_t ctx_diags) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();

        if (st) {
            auto emit_action = [&](std::string_view title, int l1, int c1, int l2, int c2,
                                   std::string_view new_text, const Diagnostic &diag) {
                writer_.begin_obj();
                writer_.key_lit("title"); writer_.str(title);
                writer_.key_lit("kind"); writer_.str("quickfix");
                writer_.key_lit("isPreferred"); writer_.boolean(true);
                writer_.key_lit("diagnostics");
                writer_.begin_arr();
                const int dln = std::max(0, diag.line - 1);
                writer_.begin_obj();
                writer_.key_lit("range");
                writer_.lsp_range(dln, 0, dln, st->lines.line_utf16_len(st->text, dln));
                writer_.key_lit("severity"); writer_.int64(diag.severity);
                writer_.key_lit("code"); writer_.str(diag.code);
                writer_.key_lit("source"); writer_.str("brat");
                writer_.key_lit("message"); writer_.str(diag.message);
                writer_.end_obj();
                writer_.end_arr();
                writer_.key_lit("edit");
                writer_.begin_obj();
                writer_.key_lit("changes");
                writer_.begin_obj();
                writer_.key(uri);
                writer_.begin_arr();
                writer_.begin_obj();
                writer_.key_lit("range"); writer_.lsp_range(l1, c1, l2, c2);
                writer_.key_lit("newText"); writer_.str(new_text);
                writer_.end_obj();
                writer_.end_arr();
                writer_.end_obj();
                writer_.end_obj();
                writer_.end_obj();
            };

            auto extract_backtick = [](std::string_view s) -> std::string_view {
                const size_t a = s.find('`');
                if (a == std::string_view::npos) return {};
                const size_t b = s.find('`', a + 1);
                if (b == std::string_view::npos) return {};
                return s.substr(a + 1, b - a - 1);
            };

            std::vector<Diagnostic> merged = st->diagnostics;
            if (tape_.is_array(ctx_diags)) {
                uint32_t c_item = tape_.at(ctx_diags).first_child;
                while (c_item != 0) {
                    const std::string code(tape_.str_view(tape_.get(c_item, "code")));
                    const int cln = tape_.int_or(
                        tape_.get(tape_.get(tape_.get(c_item, "range"), "start"), "line"), -1) + 1;
                    if (!code.empty() && cln >= 1) {
                        bool dup = false;
                        for (const Diagnostic &ex : merged) {
                            if (ex.code == code && ex.line == cln) { dup = true; break; }
                        }
                        if (!dup) {
                            const int raw_sev = tape_.int_or(tape_.get(c_item, "severity"), 2);
                            const uint8_t sev = static_cast<uint8_t>(std::clamp(raw_sev, 1, 4));
                            merged.push_back({cln, sev, code,
                                              std::string(tape_.str_view(tape_.get(c_item, "message"))),
                                              std::nullopt});
                        }
                    }
                    c_item = tape_.at(c_item).next_sibling;
                }
            }

            for (const Diagnostic &d : merged) {
                const int dline = std::max(0, d.line - 1);
                if (dline < start_line || dline > end_line) continue;

                // 1. E0596: `assign to immutable \`x\`` -> insert `mut ` after `let ` on declaration line.
                if (d.code == "E0596") {
                    const std::string_view var = extract_backtick(d.message);
                    int decl_ln = (d.note && d.note->line > 0) ? (d.note->line - 1) : -1;
                    if (decl_ln < 0 && !var.empty()) {
                        const std::string needle = "let " + std::string(var);
                        for (int k = dline; k >= 0; --k) {
                            if (st->lines.line_view(st->text, k).find(needle) != std::string_view::npos) {
                                decl_ln = k;
                                break;
                            }
                        }
                    }
                    if (decl_ln >= 0 && static_cast<size_t>(decl_ln) < st->lines.line_count()) {
                        const std::string_view lsv = st->lines.line_view(st->text, decl_ln);
                        const size_t let_pos = lsv.find("let ");
                        if (let_pos != std::string_view::npos &&
                            lsv.substr(let_pos).rfind("let mut ", 0) != 0) {
                            const int col = st->lines.byte_to_utf16_col(st->text, decl_ln, let_pos + 4);
                            emit_action("Make `" + std::string(var.empty() ? "binding" : var) + "` mutable (`let mut`)",
                                        decl_ln, col, decl_ln, col, "mut ", d);
                        }
                    }
                }
                // 2. W0002: `unused mut` -> remove `mut ` from `let mut `
                else if (d.code == "W0002" && static_cast<size_t>(dline) < st->lines.line_count()) {
                    const std::string_view lsv = st->lines.line_view(st->text, dline);
                    const size_t lm = lsv.find("let mut ");
                    if (lm != std::string_view::npos) {
                        const int c1 = st->lines.byte_to_utf16_col(st->text, dline, lm + 4);
                        const int c2 = st->lines.byte_to_utf16_col(st->text, dline, lm + 8);
                        emit_action("Remove unnecessary `mut`", dline, c1, dline, c2, "", d);
                    }
                }
                // 3. W0001: `unused var \`x\`` -> prefix `_x`
                else if (d.code == "W0001" && static_cast<size_t>(dline) < st->lines.line_count()) {
                    const std::string_view var = extract_backtick(d.message);
                    if (!var.empty() && var.front() != '_') {
                        const std::string_view lsv = st->lines.line_view(st->text, dline);
                        size_t pos = lsv.find(var);
                        while (pos != std::string_view::npos) {
                            const bool left_ok = (pos == 0 || !is_ident_char(lsv[pos - 1]));
                            const bool right_ok = (pos + var.size() == lsv.size() ||
                                                   !is_ident_char(lsv[pos + var.size()]));
                            if (left_ok && right_ok) {
                                const int col = st->lines.byte_to_utf16_col(st->text, dline, pos);
                                emit_action("Prefix unused `" + std::string(var) + "` with `_`",
                                            dline, col, dline, col, "_", d);
                                break;
                            }
                            pos = lsv.find(var, pos + 1);
                        }
                    }
                }
                // 4. W0003: `unused import` -> remove unused import line
                else if (d.code == "W0003" && static_cast<size_t>(dline) < st->lines.line_count()) {
                    const int next_ln = (static_cast<size_t>(dline + 1) < st->lines.line_count())
                        ? (dline + 1) : dline;
                    const int end_col = (next_ln == dline)
                        ? st->lines.line_utf16_len(st->text, dline) : 0;
                    emit_action("Remove unused import", dline, 0, next_ln, end_col, "", d);
                }
                // 5. B0101: `use of undeclared var \`pritn\`` + `did you mean \`print\`?`
                else if (d.code == "B0101" && d.note &&
                         static_cast<size_t>(dline) < st->lines.line_count()) {
                    const std::string_view bad = extract_backtick(d.message);
                    const std::string_view sugg = extract_backtick(d.note->msg);
                    if (!bad.empty() && !sugg.empty()) {
                        const std::string_view lsv = st->lines.line_view(st->text, dline);
                        const size_t pos = lsv.find(bad);
                        if (pos != std::string_view::npos) {
                            const int c1 = st->lines.byte_to_utf16_col(st->text, dline, pos);
                            const int c2 = st->lines.byte_to_utf16_col(st->text, dline, pos + bad.size());
                            emit_action("Replace `" + std::string(bad) + "` with `" + std::string(sugg) + "`",
                                        dline, c1, dline, c2, sugg, d);
                        }
                    }
                }
            }
        }

        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Delta-encoded semantic tokens (`textDocument/semanticTokens/full`)
    void write_semantic_tokens(std::string_view raw_id, const std::string &uri) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_obj();
        writer_.key_lit("data");
        writer_.begin_arr();

        if (st && !st->text.empty()) {
            std::set<std::string_view> fn_names, type_names, param_names, mut_names;
            if (st->ast_valid) {
                ensure_decls(*st);
                for (const Decl &d : st->decls) {
                    if (d.container == "type") type_names.insert(d.name);
                    else if (d.container == "extern") fn_names.insert(d.name);
                }
                for (const DocSymbol &ds : st->symbols) {
                    if (ds.kind == 12 || ds.kind == 6) fn_names.insert(ds.name);
                    else if (ds.kind == 10 || ds.kind == 23 || ds.kind == 11) type_names.insert(ds.name);
                }
            }

            struct RawSem {
                int line;
                int col;
                int len;
                int type;
                int mods;
            };
            std::vector<RawSem> raw;
            raw.reserve(st->text.size() / 6 + 16);

            // Capture comment tokens per line + lexer tokens
            for (size_t ln = 0; ln < st->lines.line_count(); ++ln) {
                const std::string_view lsv = st->lines.line_view(st->text, static_cast<int>(ln));
                bool in_s = false, esc = false;
                for (size_t k = 0; k + 1 < lsv.size(); ++k) {
                    if (in_s) {
                        if (esc) esc = false;
                        else if (lsv[k] == '\\') esc = true;
                        else if (lsv[k] == '"') in_s = false;
                        continue;
                    }
                    if (lsv[k] == '"') in_s = true;
                    else if (lsv[k] == '/' && lsv[k + 1] == '/') {
                        const int c1 = st->lines.byte_to_utf16_col(st->text, static_cast<int>(ln), k);
                        const int c2 = st->lines.line_utf16_len(st->text, static_cast<int>(ln));
                        if (c2 > c1) raw.push_back({static_cast<int>(ln), c1, c2 - c1, 11, 0});
                        break;
                    }
                }
            }

            try {
                const auto toks = lex(st->text);
                for (size_t i = 0; i < toks.size(); ++i) {
                    const Tok &t = toks[i];
                    if (t.kind == TK::Eof || t.text.empty()) continue;
                    // Multi-line strings skipped from single-line token delta to preserve LSP spec validity
                    if (t.text.find('\n') != std::string_view::npos) continue;
                    const int ln = std::max(0, static_cast<int>(t.line) - 1);
                    if (static_cast<size_t>(ln) >= st->lines.line_count()) continue;
                    const size_t boff = static_cast<size_t>(t.text.data() - st->text.data());
                    const size_t lstart = st->lines.starts[static_cast<size_t>(ln)];
                    if (boff < lstart) continue;
                    const int col = st->lines.byte_to_utf16_col(st->text, ln, boff - lstart);
                    const int ulen = LineIndex::utf16_len(
                        t.text, st->lines.non_ascii[static_cast<size_t>(ln)] == 0);
                    if (ulen <= 0) continue;

                    int ttype = -1, tmods = 0;
                    if (t.kind == TK::Kw) {
                        ttype = 10;  // keyword
                    } else if (t.kind == TK::Num) {
                        ttype = 13;  // number
                    } else if (t.kind == TK::Str) {
                        ttype = 12;  // string
                    } else if (t.kind == TK::Id) {
                        const std::string_view prev = i > 0 ? toks[i - 1].text : std::string_view{};
                        const std::string_view next = (i + 1 < toks.size()) ? toks[i + 1].text : std::string_view{};
                        if (prev == "import") ttype = 0;
                        else if (type_names.count(t.text) || prev == "struct" || prev == "union" ||
                                 prev == "enum" || prev == "trait") {
                            ttype = 3;
                        } else if (prev == "." && next == "(") {
                            ttype = 9;  // method
                        } else if (prev == ".") {
                            ttype = 6;  // property
                        } else if (fn_names.count(t.text) || next == "(") {
                            ttype = 8;  // function
                            if (builtin_docs().count(std::string(t.text))) tmods |= (1 << 3);
                            if (prev == "fn") tmods |= (1 << 0);
                        } else {
                            ttype = 5;  // variable
                            if (prev == "let" || prev == "const") {
                                tmods |= (1 << 0);
                                tmods |= (1 << 1);  // readonly
                            } else if (prev == "mut") {
                                tmods |= (1 << 0);
                                tmods |= (1 << 2);  // modification
                            }
                        }
                    }
                    if (ttype >= 0) raw.push_back({ln, col, ulen, ttype, tmods});
                }
            } catch (...) {}

            std::sort(raw.begin(), raw.end(), [](const RawSem &a, const RawSem &b) {
                if (a.line != b.line) return a.line < b.line;
                return a.col < b.col;
            });

            int prev_line = 0, prev_col = 0;
            for (const RawSem &r : raw) {
                const int dl = r.line - prev_line;
                const int dc = (dl == 0) ? (r.col - prev_col) : r.col;
                if (dl < 0 || dc < 0) continue;
                writer_.int64(dl);
                writer_.int64(dc);
                writer_.int64(r.len);
                writer_.int64(r.type);
                writer_.int64(r.mods);
                prev_line = r.line;
                prev_col = r.col;
            }
        }

        writer_.end_arr();
        writer_.end_obj();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Type and parameter inlay hints (`textDocument/inlayHint`)
    void write_inlay_hints(std::string_view raw_id, const std::string &uri,
                           int start_line, int end_line) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();

        if (st && st->ast_valid) {
            std::map<std::string, const std::vector<Param> *> fn_params;
            std::map<std::string, std::string> ext_rets;
            const auto mods = resolve_imports_cached(path_from_uri(uri), st->text);
            for (const ResolvedModule &rm : mods) {
                const size_t tail = rm.ns.rfind("__");
                const std::string short_mod =
                    (tail == std::string::npos) ? rm.ns : rm.ns.substr(tail + 2);
                for (const auto &sp : *rm.body) {
                    if (!sp || sp->k != SK::Pub || sp->body.empty()) continue;
                    const Stmt &in = *sp->body.front();
                    if ((in.k == SK::Fn || in.k == SK::ExportFn) && !in.fns.empty() &&
                        in.fns.front().name) {
                        const std::string &fnm = *in.fns.front().name;
                        fn_params[fnm] = &in.fns.front().params;
                        fn_params[short_mod + "." + fnm] = &in.fns.front().params;
                        if (in.fns.front().ret) ext_rets[fnm] = *in.fns.front().ret;
                    }
                }
            }
            for (const auto &sp : st->ast_body) {
                if (!sp) continue;
                const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
                if (!s) continue;
                if ((s->k == SK::Fn || s->k == SK::ExportFn) && !s->fns.empty() && s->fns.front().name) {
                    fn_params[*s->fns.front().name] = &s->fns.front().params;
                } else if (s->k == SK::Extern) {
                    fn_params[s->ext.name] = &s->ext.params;
                }
            }

            std::function<void(const std::vector<StmtP> &)> walk = [&](const std::vector<StmtP> &stmts) {
                for (const auto &sp : stmts) {
                    if (!sp) continue;
                    const Stmt &s = *sp;
                    if (s.k == SK::Pub && !s.body.empty()) {
                        walk(s.body);
                        continue;
                    }
                    const int ln = std::max(0, s.line - 1);
                    if ((s.k == SK::Let || s.k == SK::LetTmp) && !s.ann && s.e1 &&
                        ln >= start_line && ln <= end_line &&
                        static_cast<size_t>(ln) < st->lines.line_count()) {
                        const std::string ty = infer_expr_type_hint(*s.e1, st->ast_body, &ext_rets);
                        if (!ty.empty()) {
                            const std::string_view lsv = st->lines.line_view(st->text, ln);
                            const size_t vpos = lsv.find(s.a);
                            if (vpos != std::string_view::npos) {
                                const int col = st->lines.byte_to_utf16_col(
                                    st->text, ln, vpos + s.a.size());
                                writer_.begin_obj();
                                writer_.key_lit("position");
                                writer_.begin_obj();
                                writer_.key_lit("line"); writer_.int64(ln);
                                writer_.key_lit("character"); writer_.int64(col);
                                writer_.end_obj();
                                writer_.key_lit("label"); writer_.str(": " + ty);
                                writer_.key_lit("kind"); writer_.int64(1);  // Type
                                writer_.end_obj();
                            }
                        }
                    }
                    for (const FnDecl &fn : s.fns) walk(fn.body);
                    walk(s.body);
                    walk(s.body2);
                    if (s.els) walk(s.els->body);
                }
            };
            walk(st->ast_body);

            // Parameter name hints on multi-arg calls inside `[start_line, end_line]`
            try {
                const auto toks = lex(st->text);
                for (size_t i = 0; i + 2 < toks.size(); ++i) {
                    if (toks[i].kind != TK::Id || toks[i + 1].text != "(") continue;
                    const int ln = std::max(0, static_cast<int>(toks[i].line) - 1);
                    if (ln < start_line || ln > end_line) continue;
                    if (i > 0 && toks[i - 1].text == "fn") continue;
                    std::string lookup_key(toks[i].text);
                    if (i >= 2 && toks[i - 1].text == "." && toks[i - 2].kind == TK::Id) {
                        lookup_key = std::string(toks[i - 2].text) + "." + std::string(toks[i].text);
                    } else if (i > 0 && toks[i - 1].text == ".") {
                        continue;
                    }
                    auto pit = fn_params.find(lookup_key);
                    if (pit == fn_params.end() || pit->second->size() < 2) continue;
                    const auto &params = *pit->second;
                    size_t p_idx = 0;
                    int depth = 1;
                    size_t k = i + 2;
                    bool at_arg_start = true;
                    while (k < toks.size() && depth > 0) {
                        const Tok &tk = toks[k];
                        if (at_arg_start && tk.text != ")" && p_idx < params.size()) {
                            // Omit hint when argument identifier already equals parameter name.
                            if (!(tk.kind == TK::Id && tk.text == params[p_idx].name)) {
                                const int aln = std::max(0, static_cast<int>(tk.line) - 1);
                                if (static_cast<size_t>(aln) < st->lines.line_count()) {
                                    const size_t boff = static_cast<size_t>(tk.text.data() - st->text.data());
                                    const size_t lstart = st->lines.starts[static_cast<size_t>(aln)];
                                    if (boff >= lstart) {
                                        const int col = st->lines.byte_to_utf16_col(
                                            st->text, aln, boff - lstart);
                                        writer_.begin_obj();
                                        writer_.key_lit("position");
                                        writer_.begin_obj();
                                        writer_.key_lit("line"); writer_.int64(aln);
                                        writer_.key_lit("character"); writer_.int64(col);
                                        writer_.end_obj();
                                        writer_.key_lit("label");
                                        writer_.str(params[p_idx].name + ":");
                                        writer_.key_lit("kind"); writer_.int64(2);  // Parameter
                                        writer_.key_lit("paddingRight"); writer_.boolean(true);
                                        writer_.end_obj();
                                    }
                                }
                            }
                            at_arg_start = false;
                        }
                        if (tk.text == "(" || tk.text == "[" || tk.text == "{") depth++;
                        else if (tk.text == ")" || tk.text == "]" || tk.text == "}") {
                            depth--;
                        } else if (tk.text == "," && depth == 1) {
                            ++p_idx;
                            at_arg_start = true;
                        }
                        ++k;
                    }
                }
            } catch (...) {}
        }

        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Document highlights (`textDocument/documentHighlight`)
    void write_document_highlight(std::string_view raw_id, const std::string &uri,
                                  int line, int character) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();
        if (st) {
            const Word word = word_at(st->text, st->lines, line, character);
            if (!word.name.empty() && !is_keyword(word.name)) {
                for (const SymOccur &occ : find_symbol_occurrences_fast(
                         st->text, st->lines, word.name, !word.qualifier.empty())) {
                    writer_.begin_obj();
                    writer_.key_lit("range");
                    writer_.lsp_range(occ.line, occ.start_col, occ.line, occ.end_col);
                    writer_.key_lit("kind"); writer_.int64(occ.is_write ? 3 : 2);  // 3=Write, 2=Read
                    writer_.end_obj();
                }
            }
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Folding ranges (`textDocument/foldingRange`)
    void write_folding_ranges(std::string_view raw_id, const std::string &uri) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();
        if (st) {
            std::vector<int> brace_stack;
            int comment_start = -1;
            for (size_t ln = 0; ln < st->lines.line_count(); ++ln) {
                const std::string_view sv = st->lines.line_view(st->text, static_cast<int>(ln));
                std::string_view trim = sv;
                while (!trim.empty() && (trim.front() == ' ' || trim.front() == '\t'))
                    trim.remove_prefix(1);
                if (trim.rfind("//", 0) == 0) {
                    if (comment_start < 0) comment_start = static_cast<int>(ln);
                    continue;
                }
                if (comment_start >= 0) {
                    if (static_cast<int>(ln) - 1 > comment_start) {
                        writer_.begin_obj();
                        writer_.key_lit("startLine"); writer_.int64(comment_start);
                        writer_.key_lit("endLine"); writer_.int64(static_cast<int>(ln) - 1);
                        writer_.key_lit("kind"); writer_.str("comment");
                        writer_.end_obj();
                    }
                    comment_start = -1;
                }
                bool in_s = false, esc = false;
                for (size_t k = 0; k < sv.size(); ++k) {
                    const char c = sv[k];
                    if (in_s) {
                        if (esc) esc = false;
                        else if (c == '\\') esc = true;
                        else if (c == '"') in_s = false;
                        continue;
                    }
                    if (c == '/' && k + 1 < sv.size() && sv[k + 1] == '/') break;
                    if (c == '"') in_s = true;
                    else if (c == '{') brace_stack.push_back(static_cast<int>(ln));
                    else if (c == '}' && !brace_stack.empty()) {
                        const int s_ln = brace_stack.back();
                        brace_stack.pop_back();
                        if (static_cast<int>(ln) > s_ln) {
                            writer_.begin_obj();
                            writer_.key_lit("startLine"); writer_.int64(s_ln);
                            writer_.key_lit("endLine"); writer_.int64(static_cast<int>(ln));
                            writer_.key_lit("kind"); writer_.str("region");
                            writer_.end_obj();
                        }
                    }
                }
            }
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    // Document formatting (`textDocument/formatting`)
    void write_formatting(std::string_view raw_id, const std::string &uri) {
        const DocState *st = find_doc(uri);
        writer_.reset();
        writer_.begin_obj();
        writer_.key_lit("jsonrpc"); writer_.str("2.0");
        writer_.key_lit("id"); writer_.raw_value(raw_id);
        writer_.key_lit("result");
        writer_.begin_arr();
        if (st && !st->text.empty()) {
            const std::string formatted = format_brat_source(st->text, st->lines);
            if (formatted != st->text) {
                const int last_ln = std::max(0, static_cast<int>(st->lines.line_count()) - 1);
                const int last_col = st->lines.line_utf16_len(st->text, last_ln);
                writer_.begin_obj();
                writer_.key_lit("range"); writer_.lsp_range(0, 0, last_ln, last_col);
                writer_.key_lit("newText"); writer_.str(formatted);
                writer_.end_obj();
            }
        }
        writer_.end_arr();
        writer_.end_obj();
        lsp_wire::send_frame(writer_.view());
    }

    static constexpr size_t kMaxOpenDocs = 2048;

    bool handle(uint32_t root, uint32_t id_idx, bool has_id, std::string_view raw_id) {
        const std::string_view method = tape_.str_view(tape_.get(root, "method"));
        const uint32_t params = tape_.get(root, "params");

        if (method == "initialize") {
            handle_initialize(raw_id, params);
        } else if (method == "initialized" || method == "$/cancelRequest") {
            return true;
        } else if (method == "shutdown") {
            send_null_result(raw_id);
        } else if (method == "exit") {
            return false;
        } else if (method == "textDocument/didOpen") {
            const uint32_t td = tape_.get(params, "textDocument");
            const std::string uri(tape_.str_view(tape_.get(td, "uri")));
            const std::string path = path_from_uri(uri);
            if (uri.empty() || path.empty()) return true;
            const std::string_view init_text = tape_.str_view(tape_.get(td, "text"));
            if (init_text.size() > lsp_wire::kMaxFrameBytes) {
                send_log_error("brat-lsp: document exceeds 16 MiB limit");
                return true;
            }
            if (!docs_.contains(uri) && docs_.size() >= kMaxOpenDocs) {
                send_log_error("brat-lsp: maximum open document limit reached");
                return true;
            }
            DocState state;
            state.text.assign(init_text.data(), init_text.size());
            state.lines.build(state.text);
            const int ver = tape_.int_or(tape_.get(td, "version"), -1);
            if (ver >= 0) state.version = ver;
            {
                std::error_code ec;
                const auto cp = std::filesystem::weakly_canonical(path, ec);
                if (!ec && cp.has_parent_path()) ws_db_.add_root(cp.parent_path());
            }
            analyze_document_full(path, state, docs_, ws_db_);
            const uint64_t new_sig = state.sig_hash;
            docs_[uri] = std::move(state);
            publish_diagnostics(uri, docs_[uri]);
            propagate_signature_change(uri, 0, new_sig);
        } else if (method == "textDocument/didChange") {
            const uint32_t td = tape_.get(params, "textDocument");
            const std::string uri(tape_.str_view(tape_.get(td, "uri")));
            if (uri.empty() || path_from_uri(uri).empty()) return true;
            const uint32_t changes = tape_.get(params, "contentChanges");
            if (tape_.is_array(changes) && tape_.at(changes).count > 0) {
                const int ver = tape_.int_or(tape_.get(td, "version"), -1);
                auto it = docs_.find(uri);
                if (it == docs_.end() && docs_.size() >= kMaxOpenDocs) {
                    send_log_error("brat-lsp: maximum open document limit reached");
                    return true;
                }
                if (it != docs_.end() && ver >= 0 && it->second.version && ver <= *it->second.version)
                    return true;

                std::string updated = (it == docs_.end()) ? std::string{} : it->second.text;
                LineIndex new_lines = (it == docs_.end()) ? LineIndex{} : it->second.lines;
                if (it == docs_.end()) new_lines.build(updated);

                std::string err;
                EditSpanHint hint;
                if (!apply_content_changes_tape(tape_, changes, updated, new_lines, err, &hint)) {
                    send_log_error("brat-lsp: " + err);
                    return true;
                }

                if (it == docs_.end()) {
                    DocState state;
                    state.text = std::move(updated);
                    state.lines = std::move(new_lines);
                    if (ver >= 0) state.version = ver;
                    analyze_document_full(path_from_uri(uri), state, docs_, ws_db_);
                    docs_[uri] = std::move(state);
                    publish_diagnostics(uri, docs_[uri]);
                } else {
                    const uint64_t old_sig = it->second.sig_hash;
                    if (!incremental_analyze_fast(updated, new_lines, it->second, hint)) {
                        it->second.text = std::move(updated);
                        it->second.lines = std::move(new_lines);
                        analyze_document_full(path_from_uri(uri), it->second, docs_, ws_db_);
                    }
                    if (ver >= 0) it->second.version = ver;
                    publish_diagnostics(uri, it->second);
                    propagate_signature_change(uri, old_sig, it->second.sig_hash);
                }
            }
        } else if (method == "textDocument/didSave") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            if (!uri.empty()) on_disk_file_changed(uri, 2);
        } else if (method == "workspace/didChangeWatchedFiles") {
            const uint32_t changes = tape_.get(params, "changes");
            if (tape_.is_array(changes)) {
                for (uint32_t item = tape_.at(changes).first_child; item != 0;
                     item = tape_.at(item).next_sibling) {
                    const std::string uri(tape_.str_view(tape_.get(item, "uri")));
                    const int ctype = tape_.int_or(tape_.get(item, "type"), 2);
                    if (!uri.empty()) on_disk_file_changed(uri, ctype);
                }
            }
        } else if (method == "textDocument/didClose") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            docs_.erase(uri);
            writer_.reset();
            writer_.begin_obj();
            writer_.key_lit("jsonrpc"); writer_.str("2.0");
            writer_.key_lit("method"); writer_.str("textDocument/publishDiagnostics");
            writer_.key_lit("params");
            writer_.begin_obj();
            writer_.key_lit("uri"); writer_.str(uri);
            writer_.key_lit("diagnostics"); writer_.begin_arr(); writer_.end_arr();
            writer_.end_obj();
            writer_.end_obj();
            lsp_wire::send_frame(writer_.view());
        } else if (method == "textDocument/documentSymbol") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            write_document_symbols(raw_id, uri);
        } else if (method == "textDocument/definition") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_definition(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                             tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/hover") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_hover(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                        tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/completion") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_completions(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                              tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/signatureHelp") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_signature_help(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                                 tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/references") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            const uint32_t inc = tape_.get(tape_.get(params, "context"), "includeDeclaration");
            write_references(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                             tape_.int_or(tape_.get(pos, "character")),
                             tape_.bool_val(inc, true));
        } else if (method == "textDocument/prepareRename") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_prepare_rename(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                                 tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/rename") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            const std::string_view new_name = tape_.str_view(tape_.get(params, "newName"));
            write_rename(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                         tape_.int_or(tape_.get(pos, "character")), new_name);
        } else if (method == "workspace/symbol") {
            write_workspace_symbols(raw_id, tape_.str_view(tape_.get(params, "query")));
        } else if (method == "textDocument/codeAction") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t rng = tape_.get(params, "range");
            const int s_ln = tape_.int_or(tape_.get(tape_.get(rng, "start"), "line"), 0);
            const int e_ln = tape_.int_or(tape_.get(tape_.get(rng, "end"), "line"), s_ln);
            const uint32_t ctx_diags = tape_.get(tape_.get(params, "context"), "diagnostics");
            write_code_actions(raw_id, uri, s_ln, e_ln, ctx_diags);
        } else if (method == "textDocument/semanticTokens/full") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            write_semantic_tokens(raw_id, uri);
        } else if (method == "textDocument/inlayHint") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t rng = tape_.get(params, "range");
            const int s_ln = tape_.int_or(tape_.get(tape_.get(rng, "start"), "line"), 0);
            const int e_ln = tape_.int_or(tape_.get(tape_.get(rng, "end"), "line"), 1000000);
            write_inlay_hints(raw_id, uri, s_ln, e_ln);
        } else if (method == "textDocument/documentHighlight") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            const uint32_t pos = tape_.get(params, "position");
            write_document_highlight(raw_id, uri, tape_.int_or(tape_.get(pos, "line")),
                                     tape_.int_or(tape_.get(pos, "character")));
        } else if (method == "textDocument/foldingRange") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            write_folding_ranges(raw_id, uri);
        } else if (method == "textDocument/formatting") {
            const std::string uri(tape_.str_view(tape_.get(tape_.get(params, "textDocument"), "uri")));
            write_formatting(raw_id, uri);
        } else if (has_id && !tape_.is_null(id_idx) && !method.empty()) {
            send_error(raw_id, -32601, "unknown method `" + std::string(method) + "`");
        }
        return true;
    }
};

}  // namespace

int run_lsp() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return Server().run();
}
