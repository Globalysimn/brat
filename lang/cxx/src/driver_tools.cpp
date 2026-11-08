// CLI subcommands: fmt, doc, test, new, explain, check --json, emit-c, dist, ffi.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#include <io.h>
#else
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "ast.h"
#include "borrow.h"
#include "check.h"
#include "cli_tools.h"
#include "driver_common.h"
#include "driver_pkg.h"
#include "driver_tools.h"
#include "emit.h"
#include "ffi_native.h"
#include "ir.h"
#include "lexer.h"
#include "lsp.h"
#include "lsp_wire.h"
#include "module.h"
#include "parser.h"
#include "pkg_archive.h"
#include "pkg_io.h"
#include "version.h"

static std::string json_esc(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

struct ExplainEntry {
    const char *code;
    const char *title;
    const char *what;
    const char *fix;
};

static const ExplainEntry kExplainTable[] = {
    {"B0000", "other error", "Unclassified diagnostic.", "See the message text above."},
    {"B0101", "use of undeclared var",
     "Name not declared in this scope (no `let`, parameter, `for`/`match` binding).",
     "Add `let <name> = ...` or import the name from another file."},
    {"B0521", "ref escapes via spawn",
     "A borrow captured by `spawn`; the frame is copied, the borrow hangs on dead storage: data race.",
     "Pass only Copy values or explicitly `move` into `spawn`; do not capture a shared non-Copy binding."},
    {"B0522", "data race between threads",
     "One value is shared with a running `spawn` and both sides touch it while both run; exactly one side may write until they join.",
     "Call `join_all()` (or `join(handle)`) before the second access, or pass the value through a channel with `send`/`recv`."},
    {"B0530", "plugin value used after close",
     "A symbol or a pointer made through a library handle belongs to that library; `lib_close` ends it, and the memory or the code behind it may already be gone.",
     "Move the `lib_close` after the last use, or open the library again before using the value."},
    {"B0531", "plugin signature mismatch",
     "The literal signature in `call` disagrees with the `extern fn` declaration of the same name, so the call would read its arguments through the wrong prototype.",
     "Make the string match the declaration, or let `brat bindgen` write it from the header."},
    {"E0382", "use of moved value",
     "Value was moved (`move`); the source location can no longer be used. Move is linear: one ownership.",
     "Use the variable the value was moved into, or copy it beforehand (Copy types only)."},
    {"E0499", "double mut borrow",
     "One location cannot have two mutable borrows at once (law of exclusivity).",
     "Keep one writer; write through the owner, not an alias copy."},
    {"E0502", "cannot mutate while iterating",
     "A `for` holds a view of the container it walks; writing into that container inside the loop changes the thing being read.",
     "Collect the writes in a second container and apply them after the loop, or iterate a copy (`for x in v.copy()`)."},
    {"E0505", "cannot move while borrowed",
     "Location is held by a live borrow; cannot move until the borrow ends.",
     "Wait for the last use of the borrower, then move."},
    {"E0507", "cannot move out of borrowed parameter",
     "A non-Copy parameter is a view of the caller's storage; moving it would transfer data the function does not own.",
     "Declare the parameter `move` to take ownership before consuming it."},
    {"E0515", "ref escapes via return",
     "A borrow of local storage escapes the function via `return`; the storage dies with the frame.",
     "Return a copy/value, not a borrow; borrowing a parameter is fine: its storage belongs to the caller."},
    {"E0596", "assign to immutable",
     "Assignment to an immutable binding or through an immutable receiver.",
     "Declare `let mut ...` (or a `mut ...` parameter) to write."},
    {"F0001", "unsupported FFI type",
     "A slot of an `extern` declaration names a type the C emitter cannot spell, so a call would read or write memory through a prototype that does not exist.",
     "Use i8..u64, usize, f32, f64, str, ptr, ptr[T], fn(...) or an `extern struct`; `brat bindgen` writes the BRAT name for a header."},
    {"P0001", "syntax error",
     "The file does not parse; nothing after this point was checked.",
     "Fix the syntax first: everything else is reported after it parses."},
    {"T0001", "type error",
     "A value's type does not match what the position requires.",
     "Check the annotation and the expression: an i64 position does not take a str, and a `?` needs a Result."},
    {"W0001", "unused variable",
     "The binding is never read. Not an error; the build goes on.",
     "Remove the binding, or prefix work with `_` if it is intentional."},
    {"W0002", "unused mut",
     "`mut` is declared and never written.",
     "Drop `mut`: an immutable binding is enough here."},
    {"W0003", "unused import",
     "The module is imported and nothing from it is used.",
     "Remove the `import`, or use what it exports."},
    {"W0004", "string placeholder",
     "A `{...}` placeholder looks cut short by a bare quote.",
     "Bind the value first, then interpolate the binding."},
};

static const ExplainEntry *find_explain(std::string_view code) {
    for (const auto &e : kExplainTable) {
        if (code == e.code) return &e;
    }
    return nullptr;
}

static std::string classify_diag_msg(const std::string &m) {
    auto has = [&](const char *sub) { return m.find(sub) != std::string::npos; };
    if (has("use of moved")) return "E0382";
    if (has("cannot move borrowed parameter")) return "E0507";
    if (has("while borrowed") || has("cannot pass") || has("cannot use") || has("cannot assign"))
        return "E0505";
    if (has("while iterating it")) return "E0502";
    if (has("double mut borrow") || has("law of exclusivity")) return "E0499";
    if (has("escapes via return")) return "E0515";
    if (has("escapes via spawn") || has("return escapes spawn")) return "B0521";
    if (has("inside `spawn`") || has("spawn` launched in a loop")) return "B0522";
    if (has("is from library") || has("is closed twice") || has("is closed on line"))
        return "B0530";
    if (has("the signature says") || has("the signature has")) return "B0531";
    if (has("assign to immutable") || has("cannot mutate immutable") ||
        has("pass immutable") || has("assign to const") || has("write through"))
        return "E0596";
    if (has("unsupported FFI type")) return "F0001";
    if (has("use of undeclared var")) return "B0101";
    if (has("unused var")) return "W0001";
    if (has("unused mut")) return "W0002";
    if (has("unused import")) return "W0003";
    if (has("looks cut short")) return "W0004";
    return "B0000";
}

struct JsonDiag {
    std::string file;
    int line = 0;
    bool has_line = false;
    std::string severity;
    std::string code;
    std::string message;
    bool has_note = false;
    int note_line = 0;
    std::string note_message;
};

static std::string render_diags_json(const std::string &path, std::vector<JsonDiag> diags) {
    std::stable_sort(diags.begin(), diags.end(), [](const JsonDiag &a, const JsonDiag &b) {
        const int la = a.has_line ? a.line : 0;
        const int lb = b.has_line ? b.line : 0;
        if (la != lb) return la < lb;
        return a.severity < b.severity;
    });
    std::string out = "{\n  \"version\": 1,\n  \"file\": \"" + json_esc(path) + "\",\n";
    if (diags.empty()) {
        out += "  \"diagnostics\": []\n}\n";
        return out;
    }
    out += "  \"diagnostics\": [\n";
    for (size_t i = 0; i < diags.size(); ++i) {
        const JsonDiag &d = diags[i];
        const ExplainEntry *ex = find_explain(d.code);
        out += "    {\n";
        out += "      \"file\": \"" + json_esc(d.file) + "\",\n";
        if (d.has_line) out += "      \"line\": " + std::to_string(d.line) + ",\n";
        else out += "      \"line\": null,\n";
        out += "      \"severity\": \"" + json_esc(d.severity) + "\",\n";
        out += "      \"code\": \"" + json_esc(d.code) + "\",\n";
        out += "      \"message\": \"" + json_esc(d.message) + "\",\n";
        if (d.has_note) {
            out += "      \"note\": {\n";
            out += "        \"line\": " + std::to_string(d.note_line) + ",\n";
            out += "        \"message\": \"" + json_esc(d.note_message) + "\"\n";
            out += "      },\n";
        } else {
            out += "      \"note\": null,\n";
        }
        if (ex && ex->fix && ex->fix[0]) {
            out += "      \"helps\": [\n        \"" + json_esc(ex->fix) + "\"\n      ],\n";
        } else {
            out += "      \"helps\": [],\n";
        }
        out += "      \"fix\": null\n";
        out += (i + 1 < diags.size()) ? "    },\n" : "    }\n";
    }
    out += "  ]\n}\n";
    return out;
}

int cmd_check_json(const std::vector<std::string> &files) {
    if (files.empty()) {
        print_cli_usage();
        return 2;
    }
    int rc = 0;
    for (const std::string &raw_path : files) {
        std::error_code ec;
        std::string abs_path = std::filesystem::weakly_canonical(
            std::filesystem::absolute(raw_path), ec).string();
        if (ec || abs_path.empty())
            abs_path = std::filesystem::absolute(raw_path).lexically_normal().string();
        ModuleSet loaded;
        try {
            loaded = load_modules(raw_path);
            mangle_modules(loaded);
        } catch (const ParseError &e) {
            std::string msg = e.what();
            if (e.line > 0) msg += " at line " + std::to_string(e.line);
            JsonDiag d{abs_path, static_cast<int>(e.line), e.line > 0, "error", "P0001", msg, false, 0, ""};
            std::string js = render_diags_json(abs_path, {d});
            std::fwrite(js.data(), 1, js.size(), stdout);
            rc = 1;
            continue;
        } catch (const ModuleError &e) {
            JsonDiag d{abs_path, 0, false, "error", "B0000", e.what(), false, 0, ""};
            std::string js = render_diags_json(abs_path, {d});
            std::fwrite(js.data(), 1, js.size(), stdout);
            rc = 1;
            continue;
        } catch (const std::exception &e) {
            std::string msg = e.what();
            const char *code = (msg.rfind("cannot read", 0) == 0) ? "P0001" : "B0000";
            JsonDiag d{abs_path, 0, false, "error", code, msg, false, 0, ""};
            std::string js = render_diags_json(abs_path, {d});
            std::fwrite(js.data(), 1, js.size(), stdout);
            rc = 1;
            continue;
        }

        for (size_t i = 0; i < loaded.files.size(); ++i) {
            const SourceFile &sf = loaded.files[i];
            std::map<std::string, std::vector<ParamMode>> extra;
            std::set<std::string> extra_consts;
            std::set<std::string> extra_scalar;
            std::map<std::string, std::vector<size_t>> extra_borrows;
            Sigs merged_ty;
            std::map<std::string, Ty> merged_ty_consts;
            std::map<std::string, std::map<std::string, Ty>> merged_ty_structs;
            std::set<std::string> extra_owned;
            std::map<std::string, std::map<std::string, std::string>> extra_sfields;
            std::map<std::string, std::string> extra_frets;
            std::set<std::string> module_heads = imported_module_heads(sf);
            for (size_t j = 0; j < loaded.files.size(); ++j) {
                if (j == i) continue;
                FnTable ps = pub_sigs(loaded.files[j].body, exported_fn_names(loaded.files[j]));
                for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
                module_heads.insert(ps.heads.begin(), ps.heads.end());
                extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
                extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
                for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
                extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
                for (auto &kv : ps.struct_fields) extra_sfields[kv.first] = kv.second;
                for (auto &kv : ps.fn_rets) extra_frets[kv.first] = kv.second;
                collect_exports(loaded.files[j].body, merged_ty_consts, merged_ty_structs);
                collect_sigs(loaded.files[j].body, merged_ty);
            }
            BorrowOut bout = borrow_check(sf.body, extra, sf.stray, module_heads,
                                          extra_consts, extra_scalar, extra_borrows, extra_owned,
                                          extra_sfields, extra_frets);
            std::vector<TypeErr> terrs = type_check(sf.body, merged_ty, merged_ty_consts, merged_ty_structs);

            std::vector<JsonDiag> diags;
            for (const BorrowDiag &bd : bout.errs) {
                JsonDiag jd;
                jd.file = sf.path;
                jd.line = bd.line;
                jd.has_line = bd.line > 0;
                jd.severity = "error";
                jd.code = classify_diag_msg(bd.msg);
                jd.message = bd.msg;
                if (bd.note) {
                    jd.has_note = true;
                    jd.note_line = bd.note->line;
                    jd.note_message = bd.note->msg;
                }
                diags.push_back(std::move(jd));
            }
            for (const BorrowDiag &bw : bout.warns) {
                JsonDiag jd;
                jd.file = sf.path;
                jd.line = bw.line;
                jd.has_line = bw.line > 0;
                jd.severity = "warning";
                jd.code = classify_diag_msg(bw.msg);
                jd.message = bw.msg;
                diags.push_back(std::move(jd));
            }
            for (const TypeErr &te : terrs) {
                JsonDiag jd;
                jd.file = sf.path;
                jd.line = te.line;
                jd.has_line = te.line > 0;
                jd.severity = "error";
                std::string c = classify_diag_msg(te.msg);
                jd.code = (c == "B0000") ? "T0001" : c;
                jd.message = te.msg;
                diags.push_back(std::move(jd));
            }
            for (const auto &d : diags) {
                if (d.severity == "error") rc = 1;
            }
            if (i == 0 || !diags.empty()) {
                std::string js = render_diags_json(sf.path, std::move(diags));
                std::fwrite(js.data(), 1, js.size(), stdout);
            }
        }
    }
    return rc;
}

static bool fmt_is_raw_open(std::string_view line, size_t quote_idx) {
    if (quote_idx == 0 || line[quote_idx - 1] != 'r') return false;
    if (quote_idx >= 2) {
        const unsigned char prev = static_cast<unsigned char>(line[quote_idx - 2]);
        if (std::isalnum(prev) || prev == '_') return false;
    }
    return true;
}

static std::string normalize_equals_line(std::string_view line, bool &in_blk) {
    std::string out;
    out.reserve(line.size() + 4);
    size_t i = 0;
    const size_t n = line.size();
    bool in_str = false, raw_str = false;
    while (i < n) {
        const char c = line[i];
        if (in_blk) {
            out.push_back(c);
            if (c == '*' && i + 1 < n && line[i + 1] == '/') {
                out.push_back('/');
                in_blk = false;
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (in_str) {
            out.push_back(c);
            if (!raw_str && c == '\\') {
                if (i + 1 < n) {
                    out.push_back(line[i + 1]);
                    i += 2;
                    continue;
                }
            } else if (c == '"') {
                in_str = false;
                raw_str = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            raw_str = fmt_is_raw_open(line, i);
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            out.append(line.substr(i));
            break;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            in_blk = true;
            out.append("/*");
            i += 2;
            continue;
        }
        if (c == '=') {
            const char prev = (i > 0) ? line[i - 1] : '\0';
            const char nxt = (i + 1 < n) ? line[i + 1] : '\0';
            if (nxt == '=' || nxt == '>' || prev == '=' || prev == '!' || prev == '<' ||
                prev == '>' || prev == '+' || prev == '-' || prev == '*' || prev == '/' ||
                prev == '%' || prev == '&' || prev == '|' || prev == '^') {
                out.push_back(c);
                ++i;
                continue;
            }
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
            size_t j = i + 1;
            while (j < n && (line[j] == ' ' || line[j] == '\t')) ++j;
            if (j >= n) {
                if (out.empty()) out = "=";
                else out += " =";
                i = j;
                continue;
            }
            if (out.empty()) out = "= ";
            else out += " = ";
            i = j;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

static void count_braces_line(std::string_view line, bool &in_str, bool &raw_str, bool &in_blk,
                              int &opens, int &closes) {
    opens = 0;
    closes = 0;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        const char c = line[i];
        if (in_blk) {
            if (c == '*' && i + 1 < n && line[i + 1] == '/') {
                in_blk = false;
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (in_str) {
            if (!raw_str && c == '\\') {
                i += (i + 1 < n) ? 2 : 1;
                continue;
            }
            if (c == '"') {
                in_str = false;
                raw_str = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            raw_str = fmt_is_raw_open(line, i);
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '/') break;
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            in_blk = true;
            i += 2;
            continue;
        }
        if (c == '{') ++opens;
        else if (c == '}') ++closes;
        ++i;
    }
}

static std::string trim_ws(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

static std::string fmt_src_canonical(std::string_view raw_src) {
    std::string norm_nl;
    norm_nl.reserve(raw_src.size());
    for (size_t i = 0; i < raw_src.size(); ++i) {
        if (raw_src[i] == '\r') {
            if (i + 1 < raw_src.size() && raw_src[i + 1] == '\n') continue;
            norm_nl.push_back('\n');
        } else {
            norm_nl.push_back(raw_src[i]);
        }
    }
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (pos < norm_nl.size()) {
            size_t nl = norm_nl.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(norm_nl.substr(pos));
                break;
            }
            lines.push_back(norm_nl.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }
    std::vector<std::string> out;
    int depth = 0;
    bool in_str = false, raw_str = false, in_blk = false;
    for (const std::string &raw : lines) {
        if (in_str) {
            out.push_back(raw);
            int o = 0, c = 0;
            count_braces_line(raw, in_str, raw_str, in_blk, o, c);
            depth += (o - c);
            if (depth < 0) depth = 0;
            continue;
        }
        std::string stripped = trim_ws(raw);
        if (stripped.empty()) {
            out.emplace_back("");
            continue;
        }
        bool blk_eq = in_blk;
        std::string norm = trim_ws(normalize_equals_line(stripped, blk_eq));
        if (norm.empty()) {
            out.emplace_back("");
            continue;
        }
        const int orig = depth;
        if (!in_blk && !norm.empty() && norm[0] == '}') depth = (depth > 0) ? (depth - 1) : 0;
        out.push_back(std::string(static_cast<size_t>(depth) * 2, ' ') + norm);
        int o = 0, c = 0;
        count_braces_line(norm, in_str, raw_str, in_blk, o, c);
        depth = orig + (o - c);
        if (depth < 0) depth = 0;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    std::string res;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) res.push_back('\n');
        res += out[i];
    }
    res.push_back('\n');
    return res;
}

int cmd_fmt(int argc, char **argv) {
    bool check_only = false;
    bool to_stdout = false;
    std::vector<std::string> files;
    bool after_ddash = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (!after_ddash && a == "--") { after_ddash = true; continue; }
        if (!after_ddash && (a == "--help" || a == "-h")) {
            print_cli_usage();
            return 0;
        }
        if (!after_ddash && a == "--check") { check_only = true; continue; }
        if (!after_ddash && a == "--stdout") { to_stdout = true; continue; }
        if (!after_ddash && !a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat fmt: unknown flag '%s'\n", a.c_str());
            return 2;
        }
        files.push_back(a);
    }
    if (files.empty()) {
        print_cli_usage();
        return 2;
    }
    int code = 0;
    for (const std::string &path : files) {
        std::string src;
        if (!read_file(path, src)) {
            std::fprintf(stderr, "brat fmt: cannot read %s\n", path.c_str());
            code = 1;
            continue;
        }
        uint32_t bad_line = 1;
        if (!utf8_valid(src, bad_line, 1)) {
            std::fprintf(stderr, "brat fmt: %s:%u: source is not valid UTF-8\n",
                         path.c_str(), bad_line);
            code = 1;
            continue;
        }
        if (src.find('\0') != std::string::npos) {
            std::fprintf(stderr, "brat fmt: %s contains a NUL byte\n", path.c_str());
            code = 1;
            continue;
        }
        const std::string formatted = fmt_src_canonical(src);
        if (to_stdout) {
#if defined(_WIN32)
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::fwrite(formatted.data(), 1, formatted.size(), stdout);
            continue;
        }
        if (check_only) {
            if (formatted != src) {
                std::fprintf(stderr, "%s: needs formatting\n", path.c_str());
                code = 1;
            }
            continue;
        }
#if defined(_WIN32)
        const std::string tmp = path + ".brat-fmt-" + std::to_string(_getpid());
        {
            FILE *fp = std::fopen(tmp.c_str(), "wbx");
            if (!fp) {
                std::fprintf(stderr, "brat fmt: cannot write %s: %s\n",
                             path.c_str(), std::strerror(errno));
                code = 1;
                continue;
            }
            const size_t wr = std::fwrite(formatted.data(), 1, formatted.size(), fp);
            const int fl = std::fflush(fp);
            const int cl = std::fclose(fp);
            if (wr != formatted.size() || fl != 0 || cl != 0) {
                std::fprintf(stderr, "brat fmt: cannot write %s\n", path.c_str());
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                code = 1;
                continue;
            }
        }
#else
        std::string tmp = path + ".brat-fmt-XXXXXX";
        {
            const int fd = ::mkstemp(tmp.data());
            if (fd < 0) {
                std::fprintf(stderr, "brat fmt: cannot write %s: %s\n",
                             path.c_str(), std::strerror(errno));
                code = 1;
                continue;
            }
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0) {
                (void)::fchmod(fd, st.st_mode & 0777);
            }
            FILE *fp = ::fdopen(fd, "wb");
            if (!fp) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                std::fprintf(stderr, "brat fmt: cannot write %s: %s\n",
                             path.c_str(), std::strerror(errno));
                code = 1;
                continue;
            }
            const size_t wr = std::fwrite(formatted.data(), 1, formatted.size(), fp);
            const int fl = std::fflush(fp);
            (void)::fsync(::fileno(fp));
            const int cl = std::fclose(fp);
            if (wr != formatted.size() || fl != 0 || cl != 0) {
                std::fprintf(stderr, "brat fmt: cannot write %s\n", path.c_str());
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                code = 1;
                continue;
            }
        }
#endif
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::fprintf(stderr, "brat fmt: cannot write %s: %s\n",
                         path.c_str(), ec.message().c_str());
            std::error_code ec2;
            std::filesystem::remove(tmp, ec2);
            code = 1;
        }
    }
    return code;
}

int cmd_explain(int argc, char **argv) {
    bool want_json = false;
    std::vector<std::string> codes;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--help" || a == "-h") { print_cli_usage(); return 0; }
        if (a == "--json") { want_json = true; continue; }
        codes.push_back(a);
    }
    const size_t total = sizeof(kExplainTable) / sizeof(kExplainTable[0]);
    if (codes.empty()) {
        if (want_json) {
            std::string js = "[\n";
            for (size_t i = 0; i < total; ++i) {
                js += "  {\n    \"code\": \"" + json_esc(kExplainTable[i].code) +
                      "\",\n    \"title\": \"" + json_esc(kExplainTable[i].title) + "\"\n  }";
                js += (i + 1 < total) ? ",\n" : "\n";
            }
            js += "]\n";
            std::fwrite(js.data(), 1, js.size(), stdout);
            return 0;
        }
        std::printf("%zu diagnostic codes (brat explain CODE for one)\n", total);
        for (const auto &e : kExplainTable)
            std::printf("  %-6s %s\n", e.code, e.title);
        std::printf("\nA code is stable: it means the same thing in every release.\n");
        return 0;
    }
    int rc = 0;
    for (size_t i = 0; i < codes.size(); ++i) {
        std::string key = trim_ws(codes[i]);
        for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const ExplainEntry *e = find_explain(key);
        if (!e) {
            std::fprintf(stderr, "brat explain: no code `%s` (brat explain lists them)\n",
                         codes[i].c_str());
            rc = 1;
            continue;
        }
        if (want_json) {
            std::string js = "{\n  \"code\": \"" + json_esc(e->code) +
                             "\",\n  \"title\": \"" + json_esc(e->title) +
                             "\",\n  \"what\": \"" + json_esc(e->what) +
                             "\",\n  \"fix\": \"" + json_esc(e->fix) + "\"\n}\n";
            std::fwrite(js.data(), 1, js.size(), stdout);
            continue;
        }
        if (i) std::printf("\n");
        std::printf("%s: %s\n\n  what: %s\n  fix:  %s\n", e->code, e->title, e->what, e->fix);
    }
    return rc;
}

static std::string replace_all_token(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

int cmd_new(int argc, char **argv) {
    static const char *const kMainService =
R"BRAT(// __NAME__ -- TCP service skeleton generated by `brat new`.
//
// Run it:  brat run __NAME__/main.brat
// Try it:  from another terminal, send one line:
//            printf 'ping\n' | nc 127.0.0.1 8088
//
// It listens on 127.0.0.1:8088 and answers every connection with "ok". It stops
// after max_requests answers so a demo run ends by itself; remove that limit
// for a service that should run until it is killed.

enum Res {
  Ok(v),
  Err(reason)
}

fn answer(conn: i64) -> Res {
  let got = tcp_recv(conn, 256)
  match got {
    Ok(text) => {
      if text == "" {
        tcp_close(conn)
        return Err("peer closed before sending")
      }
      log.info("request of {len(text)} bytes")
      let sent = tcp_send(conn, "ok\n")
      tcp_close(conn)
      match sent {
        Ok(n) => {
          return Ok(n)
        },
        Err(reason) => {
          return Err(reason)
        }
      }
    },
    Err(reason) => {
      tcp_close(conn)
      return Err(reason)
    }
  }
  return Err("unreachable")
}

fn main() -> i64 {
  let addr = "127.0.0.1:8088"
  let max_requests = 2
  let mut served = 0
  let mut failed = 0
  let srv = tcp_listen(addr)
  match srv {
    Ok(fd) => {
      log.info("listening on {addr}")
      while served < max_requests {
        let acc = tcp_accept(fd)
        match acc {
          Ok(conn) => {
            let r = answer(conn)
            match r {
              Ok(n) => {
                served = served + 1
              },
              Err(reason) => {
                failed = failed + 1
                log.warn("request failed: {reason}")
              }
            }
          },
          Err(reason) => {
            log.error("accept failed: {reason}")
            tcp_close(fd)
            return 1
          }
        }
      }
      tcp_close(fd)
      print("served={served} failed={failed}")
      return 0
    },
    Err(reason) => {
      log.error("cannot listen on {addr}: {reason}")
      return 1
    }
  }
  return 0
}
)BRAT";

    static const char *const kMainTool =
R"BRAT(// __NAME__ -- command-line tool generated by `brat new`.
//
// Run it:  brat run __NAME__/main.brat
// Try:     brat run __NAME__/main.brat -- notes.txt

enum Res {
  Ok(v),
  Err(e)
}

enum Entry {
  Field(key, val),
  Skip(raw),
  Bad(raw)
}

fn parse_line(line: str) -> Entry {
  let text = line.trim()
  if text.starts_with("#") || len(text) == 0 {
    return Skip(text)
  }
  let parts = text.split(":")
  if len(parts) < 2 {
    return Bad(text)
  }
  return Field(parts[0].trim(), parts[1].trim())
}

fn load(path: str) -> Res {
  let got = read_file(path)
  match got {
    Ok(text) => {
      log.info("read {path} ({len(text)} bytes)")
      return Ok(text.lines())
    },
    Err(reason) => {
      log.warn("cannot read {path}: {reason}")
      return Err(reason)
    }
  }
  return Err("unreachable")
}

fn main() -> i64 {
  let mut lines = ["# comment", "host: 127.0.0.1", "port: 9001", "broken line"]
  let mut given = args()
  if len(given) > 0 {
    let got = load(given[0])
    match got {
      Ok(from_file) => {
        lines = from_file
      },
      Err(reason) => {
        log.warn("keeping the built-in sample: {reason}")
      }
    }
  }

  let mut fields = {}
  let mut skipped = 0
  let mut broken = 0
  for line in lines {
    let e = parse_line(line)
    match e {
      Field(key, val) => {
        fields.set(key, val)
        log.info("field {key}={val}")
      },
      Skip(raw) => {
        skipped = skipped + 1
      },
      Bad(raw) => {
        broken = broken + 1
        log.warn("bad line: {raw}")
      }
    }
  }

  print("fields={len(fields.keys())} skipped={skipped} broken={broken}")
  print("host={fields.get("host")} port={fields.get("port")}")
  return 0
}
)BRAT";

    static const char *const kReadmeTpl =
R"MD(# __NAME__

A BRAT __BLURB__, scaffolded by `brat new`. Zero dependencies: it also
demonstrates "{expr}" interpolation, log.info, match on
Ok/Err, and vec/map -- the pieces every BRAT program reuses.

Run:   brat run __NAME__/main.brat
Check: brat check __NAME__/main.brat
Build: brat build __NAME__/main.brat
Next:  lang/examples/ has ~70 working programs, lang/spec/SPEC.md is the law.
)MD";

    std::string name;
    bool has_name = false;
    std::string kind = "service";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--help" || a == "-h") { print_cli_usage(); return 0; }
        if (a == "--kind" && i + 1 < argc) {
            kind = argv[++i];
            continue;
        }
        if (a.rfind("--kind=", 0) == 0) {
            kind = a.substr(7);
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat new: unknown option '%s'\n", a.c_str());
            print_cli_usage();
            return 2;
        }
        if (has_name) {
            std::fprintf(stderr, "brat new: unexpected argument '%s'\n", a.c_str());
            print_cli_usage();
            return 2;
        }
        name = a;
        has_name = true;
    }
    if (!has_name) {
        std::fprintf(stderr, "brat new: NAME is required\n");
        print_cli_usage();
        return 2;
    }
    if (kind != "service" && kind != "tool") {
        std::fprintf(stderr, "brat new: unknown kind '%s' (want service|tool)\n", kind.c_str());
        print_cli_usage();
        return 2;
    }
    bool has_ctrl = false;
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f) { has_ctrl = true; break; }
    }
    if (name.empty() || has_ctrl || name != trim_ws(name) || name == "." || name == ".." ||
        name[0] == '-' || name[0] == '.' ||
        name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
        std::fprintf(stderr, "brat new: bad project name '%s'\n", name.c_str());
        return 1;
    }
    if (name.size() > 64) {
        std::fprintf(stderr, "brat new: project name too long (%zu chars)\n", name.size());
        return 1;
    }
    std::error_code ec;
    const auto target = std::filesystem::current_path(ec) / name;
    if (std::filesystem::exists(std::filesystem::symlink_status(target, ec))) {
        std::fprintf(stderr, "brat new: %s already exists\n", target.string().c_str());
        return 1;
    }
    if (!std::filesystem::create_directories(target, ec) && ec) {
        std::fprintf(stderr, "brat new: %s\n", ec.message().c_str());
        return 1;
    }
    const std::string main_text = replace_all_token(
        kind == "service" ? kMainService : kMainTool, "__NAME__", name);
    const std::string blurb = (kind == "service")
                                  ? "TCP service skeleton"
                                  : "tool for parsing `key: value` lines";
    const std::string readme_text = replace_all_token(
        replace_all_token(kReadmeTpl, "__NAME__", name), "__BLURB__", blurb);

    auto write_lf = [&](const std::filesystem::path &p, const std::string &txt) -> bool {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(txt.data(), static_cast<std::streamsize>(txt.size()));
        return static_cast<bool>(out);
    };
    if (!write_lf(target / "main.brat", main_text) ||
        !write_lf(target / "README.md", readme_text)) {
        std::fprintf(stderr, "brat new: cannot write project files in %s\n",
                     target.string().c_str());
        return 1;
    }
    std::printf("new %s (%s)\n  %s/main.brat\n  %s/README.md\n\nnext:\n  cd %s\n  brat run main.brat\n",
                name.c_str(), kind.c_str(), name.c_str(), name.c_str(), name.c_str());
    return 0;
}

static std::string leading_doc_comment(const std::vector<std::string> &lines, int one_based_line) {
    if (one_based_line <= 1 || static_cast<size_t>(one_based_line) > lines.size() + 1) return "";
    std::vector<std::string> rev;
    for (int idx = one_based_line - 2; idx >= 0; --idx) {
        const std::string raw = trim_ws(lines[static_cast<size_t>(idx)]);
        if (raw.rfind("///", 0) == 0) rev.push_back(trim_ws(raw.substr(3)));
        else if (raw.rfind("//", 0) == 0) rev.push_back(trim_ws(raw.substr(2)));
        else break;
    }
    std::reverse(rev.begin(), rev.end());
    std::string joined;
    for (size_t i = 0; i < rev.size(); ++i) {
        if (i) joined.push_back('\n');
        joined += rev[i];
    }
    return trim_ws(joined);
}

int cmd_doc(int argc, char **argv) {
    bool want_json = false;
    std::string out_path;
    std::vector<std::string> targets;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--help" || a == "-h") { print_cli_usage(); return 0; }
        if (a == "--json") { want_json = true; continue; }
        if (a == "-o" && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat doc: unknown flag '%s'\n", a.c_str());
            return 2;
        }
        targets.push_back(a);
    }
    if (targets.empty()) {
        std::fprintf(stderr, "usage: brat doc <file_or_dir> [--json] [-o OUT]\n");
        return 2;
    }
    std::vector<std::string> brat_files;
    for (const std::string &t : targets) {
        std::error_code ec;
        if (std::filesystem::is_directory(t, ec)) {
            std::vector<std::string> found;
            for (auto it = std::filesystem::recursive_directory_iterator(t, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
                const std::string fn = it->path().filename().string();
                if (it->is_directory(ec)) {
                    if (fn == ".git" || fn == "__pycache__" || fn == "bin")
                        it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec) && fn.size() > 5 &&
                    fn.compare(fn.size() - 5, 5, ".brat") == 0) {
                    found.push_back(it->path().string());
                }
            }
            std::sort(found.begin(), found.end());
            for (auto &f : found) brat_files.push_back(std::move(f));
        } else {
            brat_files.push_back(t);
        }
    }

    struct DocItem {
        std::string kind;
        std::string name;
        bool pub = true;
        bool has_lib = false;
        std::string lib;
        bool has_packed = false;
        bool packed = false;
        std::string signature;
        std::vector<std::string> effects;
        std::vector<std::string> contracts;
        int line = 0;
        std::string doc;
    };
    struct DocMod {
        std::string file;
        std::string mod;
        bool has_iface = false;
        std::vector<DocItem> items;
    };

    std::vector<DocMod> modules;
    for (const std::string &bf : brat_files) {
        std::string src;
        if (!read_file(bf, src)) {
            std::fprintf(stderr, "brat doc: %s: cannot read %s\n", bf.c_str(), bf.c_str());
            return 1;
        }
        uint32_t bad_line = 1;
        if (!utf8_valid(src, bad_line, 1)) {
            std::fprintf(stderr, "brat doc: %s: source is not valid UTF-8 (line %u)\n",
                         bf.c_str(), bad_line);
            return 1;
        }
        ParseOut parsed;
        try {
            parsed = parse(src);
        } catch (const ParseError &e) {
            std::fprintf(stderr, "brat doc: %s: %s at line %u\n",
                         bf.c_str(), e.what(), e.line);
            return 1;
        }
        const std::vector<std::string> lines = split_lines(src);
        bool has_bri = false;
        std::set<std::string> bri_fns;
        if (bf.size() > 5 && bf.compare(bf.size() - 5, 5, ".brat") == 0) {
            const std::string bri_path = bf.substr(0, bf.size() - 5) + ".bri";
            std::string bri_src;
            if (read_file(bri_path, bri_src)) {
                try {
                    const auto toks = lex(bri_src);
                    has_bri = true;
                    for (size_t ti = 0; ti + 1 < toks.size(); ++ti) {
                        if (toks[ti].kind == TK::Kw && toks[ti].text == "fn" &&
                            toks[ti + 1].kind == TK::Id) {
                            bri_fns.emplace(toks[ti + 1].text);
                        }
                    }
                } catch (...) {
                    has_bri = false;
                }
            }
        }

        std::error_code ec;
        std::string abs_bf = std::filesystem::weakly_canonical(
            std::filesystem::absolute(bf), ec).string();
        if (ec || abs_bf.empty())
            abs_bf = std::filesystem::absolute(bf).lexically_normal().string();
        DocMod dm;
        dm.file = abs_bf;
        dm.mod = std::filesystem::path(bf).stem().string();
        dm.has_iface = has_bri;

        auto fmt_params = [](const std::vector<Param> &ps) {
            std::string s;
            for (size_t pi = 0; pi < ps.size(); ++pi) {
                if (pi) s += ", ";
                std::string piece;
                if (ps[pi].mode != Mode::Ref) {
                    piece += mode_str(ps[pi].mode);
                    piece.push_back(' ');
                }
                piece += ps[pi].name + ": " + ps[pi].ann.value_or("dyn");
                s += piece;
            }
            return s;
        };

        for (const auto &sp : parsed.body) {
            if (!sp) continue;
            const bool is_pub = (sp->k == SK::Pub);
            const Stmt *node = (is_pub && !sp->body.empty() && sp->body[0]) ? sp->body[0].get() : sp.get();
            const int ln = sp->line ? sp->line : node->line;
            const std::string doc = leading_doc_comment(lines, ln);
            if (node->k == SK::Fn && !node->fns.empty()) {
                const FnDecl &fn = node->fns[0];
                const std::string fname = fn.name.value_or("");
                const bool exported = has_bri ? (bri_fns.count(fname) > 0) : is_pub;
                if (!exported && has_bri) continue;
                DocItem it;
                it.kind = "fn";
                it.name = fname;
                it.pub = exported || !has_bri;
                it.signature = "fn " + fname + "(" + fmt_params(fn.params) + ") -> " +
                               fn.ret.value_or("none");
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            } else if (node->k == SK::Extern) {
                const ExternDecl &ex = node->ext;
                DocItem it;
                it.kind = "extern_fn";
                it.name = ex.name;
                it.pub = true;
                it.has_lib = true;
                it.lib = ex.lib.value_or("");
                it.signature = "extern fn " + ex.name + "(" + fmt_params(ex.params) + ") -> " +
                               ex.ret.value_or("none");
                it.effects = ex.effects;
                it.contracts = ex.contracts;
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            } else if (node->k == SK::ExternStruct) {
                const bool is_union = (node->opt1 == "union");
                const bool is_packed = (node->opt1 == "packed");
                std::string flds;
                for (size_t fi = 0; fi < node->fields.size(); ++fi) {
                    if (fi) flds += ", ";
                    flds += node->fields[fi].name + ": " + node->fields[fi].ann.value_or("dyn");
                }
                const std::string kw = is_union ? "union" : (is_packed ? "packed struct" : "struct");
                DocItem it;
                it.kind = is_union ? "extern_union" : "extern_struct";
                it.name = node->a;
                it.pub = true;
                it.has_packed = true;
                it.packed = is_packed;
                it.signature = "extern " + kw + " " + node->a + " { " + flds + " }";
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            } else if (node->k == SK::Struct && is_pub) {
                std::string flds;
                for (size_t fi = 0; fi < node->fields.size(); ++fi) {
                    if (fi) flds += ", ";
                    flds += node->fields[fi].name + ": " + node->fields[fi].ann.value_or("dyn");
                }
                DocItem it;
                it.kind = "struct";
                it.name = node->a;
                it.pub = true;
                it.signature = "pub struct " + node->a + " { " + flds + " }";
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            } else if (node->k == SK::Enum && is_pub) {
                std::string vs;
                for (size_t vi = 0; vi < node->variants.size(); ++vi) {
                    if (vi) vs += ", ";
                    vs += node->variants[vi].name;
                    if (!node->variants[vi].fields.empty()) {
                        vs.push_back('(');
                        for (size_t vfi = 0; vfi < node->variants[vi].fields.size(); ++vfi) {
                            if (vfi) vs += ", ";
                            const std::string &fnm = node->variants[vi].fields[vfi];
                            const bool has_ann =
                                vfi < node->variants[vi].field_anns.size() &&
                                node->variants[vi].field_anns[vfi] &&
                                !node->variants[vi].field_anns[vfi]->empty();
                            if (has_ann && fnm == "_" + std::to_string(vfi)) {
                                vs += *node->variants[vi].field_anns[vfi];
                            } else if (has_ann) {
                                vs += fnm + ": " + *node->variants[vi].field_anns[vfi];
                            } else {
                                vs += fnm;
                            }
                        }
                        vs.push_back(')');
                    }
                }
                DocItem it;
                it.kind = "enum";
                it.name = node->a;
                it.pub = true;
                it.signature = "pub enum " + node->a + " { " + vs + " }";
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            } else if (node->k == SK::Let && node->konst && is_pub) {
                DocItem it;
                it.kind = "const";
                it.name = node->a;
                it.pub = true;
                it.signature = "pub const " + node->a + ": " + node->ann.value_or("dyn");
                it.line = ln;
                it.doc = doc;
                dm.items.push_back(std::move(it));
            }
        }
        modules.push_back(std::move(dm));
    }

    std::string rendered;
    if (want_json) {
        rendered = "{\n  \"version\": 1,\n  \"modules\": [\n";
        for (size_t mi = 0; mi < modules.size(); ++mi) {
            const DocMod &m = modules[mi];
            rendered += "    {\n";
            rendered += "      \"file\": \"" + json_esc(m.file) + "\",\n";
            rendered += "      \"module\": \"" + json_esc(m.mod) + "\",\n";
            rendered += std::string("      \"has_interface\": ") + (m.has_iface ? "true" : "false") + ",\n";
            if (m.items.empty()) {
                rendered += "      \"items\": []\n";
            } else {
                rendered += "      \"items\": [\n";
                for (size_t ii = 0; ii < m.items.size(); ++ii) {
                    const DocItem &it = m.items[ii];
                    rendered += "        {\n";
                    rendered += "          \"kind\": \"" + json_esc(it.kind) + "\",\n";
                    rendered += "          \"name\": \"" + json_esc(it.name) + "\",\n";
                    rendered += std::string("          \"public\": ") + (it.pub ? "true" : "false") + ",\n";
                    if (it.has_lib) {
                        if (it.lib.empty()) rendered += "          \"lib\": null,\n";
                        else rendered += "          \"lib\": \"" + json_esc(it.lib) + "\",\n";
                    }
                    if (it.has_packed) {
                        rendered += std::string("          \"packed\": ") + (it.packed ? "true" : "false") + ",\n";
                    }
                    rendered += "          \"signature\": \"" + json_esc(it.signature) + "\",\n";
                    if (it.kind == "extern_fn") {
                        auto arr = [&](const char *k, const std::vector<std::string> &v) {
                            if (v.empty()) {
                                rendered += std::string("          \"") + k + "\": [],\n";
                            } else {
                                rendered += std::string("          \"") + k + "\": [\n";
                                for (size_t vi = 0; vi < v.size(); ++vi) {
                                    rendered += "            \"" + json_esc(v[vi]) + "\"";
                                    rendered += (vi + 1 < v.size()) ? ",\n" : "\n";
                                }
                                rendered += "          ],\n";
                            }
                        };
                        arr("effects", it.effects);
                        arr("contracts", it.contracts);
                    }
                    rendered += "          \"line\": " + std::to_string(it.line) + ",\n";
                    rendered += "          \"doc\": \"" + json_esc(it.doc) + "\"\n";
                    rendered += (ii + 1 < m.items.size()) ? "        },\n" : "        }\n";
                }
                rendered += "      ]\n";
            }
            rendered += (mi + 1 < modules.size()) ? "    },\n" : "    }\n";
        }
        rendered += "  ]\n}\n";
    } else {
        std::vector<std::string> chunks;
        for (const DocMod &m : modules) {
            chunks.push_back("# Module `" + m.mod + "`");
            if (m.has_iface) chunks.push_back("_Interface verified via `.bri`._");
            chunks.emplace_back("");
            if (m.items.empty()) {
                chunks.emplace_back("_No documented declarations._\n");
                continue;
            }
            for (const DocItem &it : m.items) {
                chunks.push_back("## `" + it.signature + "`");
                if (!it.effects.empty() || !it.contracts.empty()) {
                    std::string tags;
                    for (const auto &ef : it.effects) {
                        if (!tags.empty()) tags += "`, `";
                        tags += ef;
                    }
                    for (const auto &ct : it.contracts) {
                        if (!tags.empty()) tags += "`, `";
                        tags += ct;
                    }
                    chunks.push_back("- Contracts: `" + tags + "`");
                }
                if (!it.doc.empty()) chunks.push_back(it.doc);
                chunks.emplace_back("");
            }
        }
        for (size_t ci = 0; ci < chunks.size(); ++ci) {
            if (ci) rendered.push_back('\n');
            rendered += chunks[ci];
        }
        while (!rendered.empty() && std::isspace(static_cast<unsigned char>(rendered.back())))
            rendered.pop_back();
        rendered.push_back('\n');
    }

    if (!out_path.empty()) {
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "brat doc: cannot write %s\n", out_path.c_str());
            return 1;
        }
        out.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
        out.flush();
        if (!out) {
            std::fprintf(stderr, "brat doc: cannot write %s\n", out_path.c_str());
            return 1;
        }
    } else {
        std::fwrite(rendered.data(), 1, rendered.size(), stdout);
    }
    return 0;
}

static int run_captured(const std::vector<std::string> &args, std::string &combined) {
    BuildStage stage(std::filesystem::temp_directory_path() / "brat-test-log");
    const auto log_file = stage.path / "out.log";
#if defined(_WIN32)
    (void)log_file;
    combined.clear();
    return run_process(args);
#else
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        FILE *fp = std::fopen(log_file.string().c_str(), "wb");
        if (fp) {
            ::dup2(::fileno(fp), STDOUT_FILENO);
            ::dup2(::fileno(fp), STDERR_FILENO);
            std::fclose(fp);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 127;
    read_file(log_file.string(), combined);
    return WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
#endif
}

int cmd_test(int argc, char **argv) {
    std::vector<std::string> inputs;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--help" || a == "-h") { print_cli_usage(); return 0; }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat test: unknown flag '%s'\n", a.c_str());
            return 2;
        }
        inputs.push_back(a);
    }
    if (inputs.empty()) {
        print_cli_usage();
        return 2;
    }
    std::vector<std::string> files;
    for (const std::string &in : inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(in, ec)) {
            std::vector<std::string> found;
            for (auto it = std::filesystem::recursive_directory_iterator(in, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
                const std::string fn = it->path().filename().string();
                if (it->is_directory(ec)) {
                    if (fn == ".git" || fn == "__pycache__" || fn == "bin" ||
                        fn == "vendor" || fn == ".brat-cache")
                        it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec) && fn.rfind("test_", 0) == 0 &&
                    fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".brat") == 0) {
                    found.push_back(it->path().string());
                }
            }
            std::sort(found.begin(), found.end());
            for (auto &f : found) files.push_back(std::move(f));
        } else {
            files.push_back(in);
        }
    }
    if (files.empty()) {
        std::printf("0 passed, 0 failed\n");
        return 0;
    }
    const std::string self_exe = self_executable_path(argv[0]).string();
    int failed = 0;
    for (const std::string &path : files) {
        std::string combined;
        const int rc = run_captured({self_exe, "run", path}, combined);
        if (rc == 0) {
            std::printf("ok %s\n", path.c_str());
        } else {
            ++failed;
            const std::string tail = combined.size() > 300
                                         ? combined.substr(combined.size() - 300)
                                         : combined;
            std::printf("FAIL %s exit=%d %s\n", path.c_str(), rc, tail.c_str());
        }
    }
    std::printf("%zu passed, %d failed\n", files.size() - static_cast<size_t>(failed), failed);
    return failed ? 1 : 0;
}

bool has_arena_block_tokens(std::string_view src) {
    try {
        const auto toks = lex(src);
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (toks[i].text == "arena" && toks[i + 1].text == "{") return true;
        }
    } catch (...) {
    }
    return false;
}

int cmd_emit_c(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) continue;
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_cli_usage();
            return 0;
        }
    }
    bool no_check = false;
    bool bundle = false;
    const char *tev = std::getenv("BRAT_TRACE");
    bool trace_lines = (tev && tev[0] && tev[0] != '0');
    bool unsafe_ffi = false;
    uint64_t mem = 0;
    bool has_mem = false;
    std::string target, ffi_header;
    std::vector<std::string> ffi_objects;
    int opt = 0;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--no-check") { no_check = true; continue; }
        if (a == "--bundle") { bundle = true; continue; }
        if (a == "--trace-lines") { trace_lines = true; continue; }
        if (a == "--unsafe-ffi") { unsafe_ffi = true; continue; }
        if (a == "--header" && i + 1 < argc) { ffi_header = argv[++i]; continue; }
        if (a.rfind("--header=", 0) == 0) { ffi_header = a.substr(9); continue; }
        if (a == "--object" && i + 1 < argc) { ffi_objects.push_back(argv[++i]); continue; }
        if (a.rfind("--object=", 0) == 0) { ffi_objects.push_back(a.substr(9)); continue; }
        if (a == "-O0") { opt = 0; continue; }
        if (a == "-O1") { opt = 1; continue; }
        if (a == "-O2") { opt = 2; continue; }
        if (a == "--target" && i + 1 < argc) { target = argv[++i]; continue; }
        if (a.rfind("--target=", 0) == 0) { target = a.substr(9); continue; }
        if (a == "--mem" || a.rfind("--mem=", 0) == 0) {
            const std::string val = (a == "--mem")
                                        ? ((i + 1 < argc) ? std::string(argv[++i]) : std::string())
                                        : a.substr(6);
            if (!parse_memory(val, mem) || mem == 0) {
                std::fprintf(stderr,
                             "brat emit-c: bad --mem '%s' (want e.g. 16777216, 512K, 16M, 1G)\n",
                             val.c_str());
                return 2;
            }
            has_mem = true;
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat emit-c: unknown flag '%s'\n", a.c_str());
            return 2;
        }
        files.push_back(a);
    }
    if (files.empty()) {
        print_cli_usage();
        return 2;
    }
    const std::string path = files[0];
    std::string src;
    if (!read_file(path, src)) {
        std::fprintf(stderr, "brat emit-c: cannot read %s\n", path.c_str());
        return 1;
    }
    uint32_t bad_line = 1;
    if (!utf8_valid(src, bad_line, 1)) {
        std::fprintf(stderr, "brat emit-c: %s:%u: source is not valid UTF-8\n",
                     path.c_str(), bad_line);
        return 1;
    }
    if (src.find('\0') != std::string::npos) {
        std::fprintf(stderr, "brat emit-c: %s contains a NUL byte\n", path.c_str());
        return 1;
    }
    if (no_check && has_arena_block_tokens(src)) {
        std::fprintf(stderr,
                     "brat emit-c: refusing --no-check for %s: arena blocks depend on the "
                     "escape rules, which only the checker applies.\n",
                     path.c_str());
        return 1;
    }
    try {
        if (no_check) {
            ModuleSet fresh = load_modules(path);
            for (const auto &sf : fresh.files) {
                if (has_arena_block_tokens(sf.src)) {
                    std::fprintf(stderr,
                                 "brat emit-c: refusing --no-check for %s: arena blocks depend on the "
                                 "escape rules, which only the checker applies.\n",
                                 path.c_str());
                    return 1;
                }
            }
        }
        if (!no_check) {
            ModuleSet fresh = load_modules(path);
            mangle_modules(fresh);
            g_report = stderr;
            std::size_t nerr = 0;
            for (std::size_t fi = 0; fi < fresh.files.size(); ++fi) {
                nerr += ownership_errors(fresh.files, fi);
                nerr += type_errors(fresh.files, fi, /*arena_only=*/true);
            }
            g_report = stdout;
            if (nerr != 0) {
                std::fprintf(stderr,
                             "brat emit-c: refusing to emit %s: the ownership/type checker "
                             "reported errors above. Emitted C skips every check `brat "
                             "build` applies.\nFix it, or pass --no-check to override "
                             "deliberately.\n",
                             path.c_str());
                return 1;
            }
        }
        ModuleSet set = load_modules(path);
        if (native_ffi::ffi_gate(path, module_source_pairs(set.files), ffi_header, ffi_objects,
                                 target, "brat emit-c", unsafe_ffi) != 0) {
            return 1;
        }
        flatten(set, true);
        std::string out = codegen::emit_program(set.flat, dir_of(path), opt, nullptr, target, trace_lines);
        if (bundle) {
            const auto engine_dir = locate_lang_dir(argv[0]) / "engine";
            std::string crt_text;
            if (!read_crt_bundle(engine_dir, crt_text)) {
                std::fprintf(stderr, "brat emit-c: cannot read runtime headers in %s\n",
                             engine_dir.string().c_str());
                return 1;
            }
            const std::string needle = "#include \"crt.h\"";
            const size_t at = out.find(needle);
            if (at != std::string::npos) out.replace(at, needle.size(), crt_text);
        }
        const std::string contract_snippet = build_ffi_contract_c(set.files, set.flat, target, ffi_header, ffi_objects);
        if (!contract_snippet.empty()) {
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            out += contract_snippet;
        }
        if (no_check) out = "/* BRAT: UNVERIFIED -- ownership not checked */\n" + out;
        if (has_mem) out = "#define BRAT_MEM_BYTES " + std::to_string(mem) + "\n" + out;
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    } catch (const ParseError &e) {
        print_parse_error(path, e);
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat emit-c: %s\n", e.what());
        return 1;
    }
}


static std::string trim_dist_target(std::string value) {
    const auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t first = 0;
    while (first < value.size() && ws(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && ws(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

static bool safe_dist_target(const std::string &target) {
    if (target.empty() || target == "." || target == "..") return false;
    for (unsigned char c : target) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static bool read_captured(FILE *file, std::string &out, std::string &error) {
    if (std::fseek(file, 0, SEEK_END) != 0) {
        error = "cannot seek captured output";
        return false;
    }
    const long end = std::ftell(file);
    if (end < 0) {
        error = "cannot size captured output";
        return false;
    }
    constexpr size_t limit = 4 * 1024 * 1024;
    const size_t total = static_cast<size_t>(end);
    const size_t start = total > limit ? total - limit : 0;
    if (std::fseek(file, static_cast<long>(start), SEEK_SET) != 0) {
        error = "cannot read captured output";
        return false;
    }
    out.resize(total - start);
    if (!out.empty() && std::fread(out.data(), 1, out.size(), file) != out.size()) {
        error = "short read from captured output";
        return false;
    }
    if (start) out.insert(0, "[earlier build output omitted]\n");
    return true;
}

static bool capture_native_cli(const std::vector<std::string> &args, int &status,
                               std::string &out, std::string &err, std::string &error) {
    FILE *out_file = std::tmpfile();
    FILE *err_file = std::tmpfile();
    if (!out_file || !err_file) {
        if (out_file) std::fclose(out_file);
        if (err_file) std::fclose(err_file);
        error = "cannot create temporary output capture";
        return false;
    }
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();
#if defined(_WIN32)
    const int out_fd = _fileno(stdout);
    const int err_fd = _fileno(stderr);
    const int saved_out = _dup(out_fd);
    const int saved_err = _dup(err_fd);
#else
    const int out_fd = fileno(stdout);
    const int err_fd = fileno(stderr);
    const int saved_out = dup(out_fd);
    const int saved_err = dup(err_fd);
#endif
    bool out_redirected = false;
    bool err_redirected = false;
#if defined(_WIN32)
    if (saved_out >= 0 && saved_err >= 0 && _dup2(_fileno(out_file), out_fd) == 0) {
        out_redirected = true;
        if (_dup2(_fileno(err_file), err_fd) == 0) err_redirected = true;
    }
#else
    if (saved_out >= 0 && saved_err >= 0 && dup2(fileno(out_file), out_fd) >= 0) {
        out_redirected = true;
        if (dup2(fileno(err_file), err_fd) >= 0) err_redirected = true;
    }
#endif
    if (!out_redirected || !err_redirected) {
#if defined(_WIN32)
        if (out_redirected) _dup2(saved_out, out_fd);
        if (err_redirected) _dup2(saved_err, err_fd);
        if (saved_out >= 0) _close(saved_out);
        if (saved_err >= 0) _close(saved_err);
#else
        if (out_redirected) dup2(saved_out, out_fd);
        if (err_redirected) dup2(saved_err, err_fd);
        if (saved_out >= 0) close(saved_out);
        if (saved_err >= 0) close(saved_err);
#endif
        std::fclose(out_file);
        std::fclose(err_file);
        error = "cannot redirect output for native build";
        return false;
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    try {
        status = brat_cli_main(static_cast<int>(args.size()), argv.data());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat-cxx: internal build error: %s\n", e.what());
        status = 1;
    } catch (...) {
        std::fprintf(stderr, "brat-cxx: internal build error\n");
        status = 1;
    }
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();
#if defined(_WIN32)
    const bool restored_out = _dup2(saved_out, out_fd) == 0;
    const bool restored_err = _dup2(saved_err, err_fd) == 0;
    _close(saved_out);
    _close(saved_err);
#else
    const bool restored_out = dup2(saved_out, out_fd) >= 0;
    const bool restored_err = dup2(saved_err, err_fd) >= 0;
    close(saved_out);
    close(saved_err);
#endif
    std::string read_error;
    const bool read_out = read_captured(out_file, out, read_error);
    const bool read_err = read_captured(err_file, err, read_error);
    std::fclose(out_file);
    std::fclose(err_file);
    if (!restored_out || !restored_err) {
        error = "cannot restore process output after native build";
        return false;
    }
    if (!read_out || !read_err) {
        error = read_error;
        return false;
    }
    return true;
}

static bool write_dist_manifest(const std::filesystem::path &out_dir,
                                const std::string &text, std::string &error) {
    std::filesystem::path stage;
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    bool reserved = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        stage = out_dir / (".brat-dist-" + std::to_string(stamp) + "-" +
                           std::to_string(attempt));
        ec.clear();
        if (std::filesystem::create_directory(stage, ec)) {
            reserved = true;
            break;
        }
        if (ec) {
            error = "cannot create manifest staging directory: " + ec.message();
            return false;
        }
    }
    if (!reserved) {
        error = "cannot reserve manifest staging directory";
        return false;
    }
    const auto temp_file = stage / "manifest.json";
    {
        std::ofstream file(temp_file, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot create staged manifest";
            std::filesystem::remove_all(stage, ec);
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            error = "cannot write staged manifest";
            file.close();
            std::filesystem::remove_all(stage, ec);
            return false;
        }
    }
    const auto final_file = out_dir / "manifest.json";
#if defined(_WIN32)
    if (!MoveFileExW(temp_file.wstring().c_str(), final_file.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot install manifest (Windows error " +
                std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
        std::filesystem::remove_all(stage, ec);
        return false;
    }
#else
    std::filesystem::rename(temp_file, final_file, ec);
    if (ec) {
        error = "cannot install manifest: " + ec.message();
        std::filesystem::remove_all(stage, ec);
        return false;
    }
#endif
    std::filesystem::remove_all(stage, ec);
    return true;
}

static void print_dist_usage() {
    std::fprintf(stderr,
                 "usage: brat dist FILE [-o DIR] [--target TRIPLE[,TRIPLE...]] "
                 "[--static|--dynamic] [--shared] [--unsafe-ffi]\n");
}

static int refuse_unproven_dist(const std::string &path) {
    std::fprintf(stderr,
                 "brat dist: refusing %s: it calls C and no header was given.\n"
                 "  A prototype that disagrees with the library is undefined behaviour, not a\n"
                 "  warning. Prove it, or say you are not going to:\n"
                 "      brat dist %s --header FILE     the declarations compared to C\n"
                 "      brat dist %s --unsafe-ffi      build it anyway, on your head\n",
                 path.c_str(), path.c_str(), path.c_str());
    return 1;
}

int cmd_dist(int argc, char **argv) {
    if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
        print_dist_usage();
        return 0;
    }
    std::string out_arg = "dist";
    std::string target_arg, ffi_header;
    std::vector<std::string> files, ffi_objects;
    bool unsafe_ffi = false;
    bool shared = false;
    bool static_value = true;
    bool static_set = false;
    bool static_requested = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--" || arg == "--no-check") continue;
        if (arg == "--unsafe-ffi") {
            unsafe_ffi = true;
        } else if (arg == "-o" && i + 1 < argc) {
            out_arg = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            target_arg = argv[++i];
        } else if (arg.rfind("--target=", 0) == 0) {
            target_arg = arg.substr(9);
        } else if (arg == "--static") {
            static_value = true;
            static_set = true;
            static_requested = true;
        } else if (arg == "--dynamic") {
            static_value = false;
            static_set = true;
        } else if (arg == "--shared") {
            shared = true;
            static_value = false;
            static_set = true;
        } else if (arg == "--header" && i + 1 < argc) {
            ffi_header = argv[++i];
        } else if (arg.rfind("--header=", 0) == 0) {
            ffi_header = arg.substr(9);
        } else if (arg == "--object" && i + 1 < argc) {
            ffi_objects.push_back(argv[++i]);
        } else if (arg.rfind("--object=", 0) == 0) {
            ffi_objects.push_back(arg.substr(9));
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "brat dist: unknown option %s\n", arg.c_str());
            print_dist_usage();
            return 2;
        } else {
            files.push_back(arg);
        }
    }
    if (files.size() != 1) {
        print_dist_usage();
        return 2;
    }
    if (!static_set) static_value = true;
    if (target_arg.empty() && std::any_of(argv + 2, argv + argc, [](const char *arg) {
            return std::string(arg).rfind("--target=", 0) == 0;
        })) {
        std::fprintf(stderr, "brat dist: --target needs a triple\n");
        return 2;
    }

    std::vector<std::string> targets;
    if (target_arg.empty()) {
        targets.emplace_back();
    } else {
        size_t at = 0;
        while (at <= target_arg.size()) {
            const size_t comma = target_arg.find(',', at);
            const size_t end = comma == std::string::npos ? target_arg.size() : comma;
            const std::string target = trim_dist_target(target_arg.substr(at, end - at));
            if (!target.empty()) targets.push_back(target);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
    }
    if (targets.empty()) {
        std::fprintf(stderr, "brat dist: --target requires at least one triple\n");
        return 2;
    }
    for (const std::string &target : targets) {
        if (!target.empty() && !safe_dist_target(target)) {
            std::fprintf(stderr, "brat dist: unsafe target triple %s\n", target.c_str());
            return 2;
        }
    }
    std::string source_probe;
    if (!read_file(files.front(), source_probe)) {
        std::fprintf(stderr, "brat dist: refusing to dist %s: the check above failed.\n",
                     files.front().c_str());
        return 1;
    }

    const std::filesystem::path out_dir(out_arg);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec || !std::filesystem::is_directory(out_dir, ec) || ec) {
        std::fprintf(stderr, "brat dist: cannot create %s: %s\n", out_arg.c_str(),
                     ec ? ec.message().c_str() : "not a directory");
        return 1;
    }

    struct Artifact {
        std::string target;
        std::string name;
        std::string digest;
        uint64_t size = 0;
    };
    std::vector<Artifact> artifacts;
    bool failed = false;
    bool unsafe_warning_printed = false;
    const std::string source = files.front();
    std::string stem = std::filesystem::path(source).filename().stem().string();
    if (stem.empty()) stem = std::filesystem::path(source).filename().string();

    for (const std::string &target : targets) {
        bool source_has_libs = false;
        try {
            ModuleSet set = load_modules(source);
            if (ffi_header.empty() && ffi_objects.empty() && has_unproven_externs(set.files, target)) {
                if (!unsafe_ffi) {
                    refuse_unproven_dist(source);
                    failed = true;
                    continue;
                }
                if (!unsafe_warning_printed) {
                    std::fprintf(stderr, "brat dist: %s calls C without a proof (--unsafe-ffi)\n",
                                 source.c_str());
                    unsafe_warning_printed = true;
                }
            }
            flatten(set, true);
            source_has_libs = !codegen::extern_libs(set.flat).empty();
        } catch (const std::exception &) {
            // The build path reports load and parse failures with its normal diagnostics.
        }

        std::string name = stem;
        if (!target.empty()) name += "-" + target;
        const std::string pkind = platform_kind(target);
        std::string lower_target = target;
        std::transform(lower_target.begin(), lower_target.end(), lower_target.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (shared) name += pkind == "macos" ? ".dylib" : (pkind == "windows" ? ".dll" : ".so");
        else if (!target.empty() && lower_target.find("windows") != std::string::npos) name += ".exe";
        const std::filesystem::path artifact_path = out_dir / name;

        std::vector<std::string> build_args{argv[0], "build", source, "-o", artifact_path.string()};
        if (!target.empty()) {
            build_args.push_back("--target");
            build_args.push_back(target);
        }
        const bool build_static = static_value &&
                                  (static_requested || !source_has_libs);
        if (build_static) build_args.push_back("--static");
        else build_args.push_back("--dynamic");
        if (shared) build_args.push_back("--shared");
        if (unsafe_ffi) build_args.push_back("--unsafe-ffi");
        if (!ffi_header.empty()) { build_args.push_back("--header"); build_args.push_back(ffi_header); }
        for (const auto &fo : ffi_objects) { build_args.push_back("--object"); build_args.push_back(fo); }
        int build_status = 1;
        std::string build_out, build_err, capture_error;
        if (!capture_native_cli(build_args, build_status, build_out, build_err, capture_error)) {
            std::fprintf(stderr, "brat dist: build %s: %s\n", target.empty() ? "host" : target.c_str(),
                         capture_error.c_str());
            failed = true;
            continue;
        }
        if (build_status != 0) {
            std::string tail = build_out + build_err;
            if (tail.size() > 2000) tail.erase(0, tail.size() - 2000);
            std::fprintf(stderr, "brat dist: cc %s failed:\n%s%s\n",
                         target.empty() ? "host" : target.c_str(), tail.c_str(),
                         tail.empty() || tail.back() == '\n' ? "" : "\n");
            failed = true;
            continue;
        }

        Artifact artifact;
        artifact.target = target.empty() ? "host" : target;
        artifact.name = name;
        std::string hash_error;
        if (!sha256_file(artifact_path, artifact.digest, artifact.size, hash_error)) {
            std::fprintf(stderr, "brat dist: hash %s: %s\n", artifact_path.string().c_str(),
                         hash_error.c_str());
            failed = true;
            continue;
        }
        artifacts.push_back(std::move(artifact));
    }

    std::string absolute_source;
    try {
        absolute_source = std::filesystem::absolute(source).lexically_normal().string();
    } catch (const std::exception &) {
        absolute_source = source;
    }
    std::ostringstream manifest;
    manifest << "{\n  \"source\": \"" << json_esc(absolute_source)
             << "\",\n  \"artifacts\": [";
    for (size_t i = 0; i < artifacts.size(); ++i) {
        const Artifact &a = artifacts[i];
        manifest << (i ? "," : "") << "\n    {\n"
                 << "      \"target\": \"" << json_esc(a.target) << "\",\n"
                 << "      \"artifact\": \"" << json_esc(a.name) << "\",\n"
                 << "      \"sha256\": \"" << a.digest << "\",\n"
                 << "      \"size\": " << a.size << "\n"
                 << "    }";
    }
    if (!artifacts.empty()) manifest << "\n  ";
    manifest << "]\n}\n";
    if (failed && artifacts.empty()) {
        std::fprintf(stderr, "brat dist: some targets failed\n");
        return 1;
    }
    std::string manifest_error;
    if (!write_dist_manifest(out_dir, manifest.str(), manifest_error)) {
        std::fprintf(stderr, "brat dist: write manifest: %s\n", manifest_error.c_str());
        return 1;
    }
    if (failed) {
        std::fprintf(stderr, "brat dist: some targets failed\n");
        return 1;
    }
    std::printf("dist ok %s (%zu artifact(s))\n", out_arg.c_str(), artifacts.size());
    return 0;
}

int cmd_ffi_native(int argc, char **argv) {
    if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
        print_cli_usage();
        return 0;
    }
    return native_ffi::cmd_ffi(argc - 2, argv + 2);
}

