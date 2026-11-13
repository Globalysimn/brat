// BRAT compiler and CLI driver (`brat` / `brat-cxx`).

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

bool read_file(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool read_crt_bundle(const std::filesystem::path &engine_dir, std::string &out) {
    std::string top;
    if (!read_file((engine_dir / "crt.h").string(), top)) return false;
    out.clear();
    out.reserve(380000);
    std::istringstream iss(top);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string pfx = "#include \"crt/";
        if (line.rfind(pfx, 0) == 0 && line.size() > pfx.size() + 1 && line.back() == '"') {
            const std::string sub = line.substr(pfx.size(), line.size() - pfx.size() - 1);
            std::string part;
            if (!read_file((engine_dir / "crt" / sub).string(), part)) return false;
            out += part;
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
        } else {
            out += line;
            out.push_back('\n');
        }
    }
    return true;
}

std::string dir_of(const std::string &path) {
    return std::filesystem::absolute(path).parent_path().lexically_normal().string();
}

std::vector<std::string> split_lines(const std::string &src) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string rstrip(const std::string &s) {
    size_t n = s.size();
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return s.substr(0, n);
}

// The same shape as diag.to_human: `file:line: severity: message` over the
// source line, then the note the message points at. A diagnostic whose line is
// unknown prints as `file: severity: message`.
// `check` prints its bill on stdout, where the user asked for it. `cc` must not:
// stdout is the C, so a refusal has to go to stderr and leave the pipe empty.
FILE *g_report = stdout;

// A snippet is an aid, not a dump. Generated programs carry lines with tens of
// thousands of terms, and echoing one verbatim buries the message it belongs to
// (and can be megabytes). Snippets are clipped cleanly at `kSnippetMax`
// UTF-8 codepoints.
static constexpr size_t kSnippetMax = 200;

static size_t utf8_sequence_len(const std::string &text, size_t i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) return 1;
    size_t len = 0;
    uint32_t min_cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        min_cp = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        min_cp = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        min_cp = 0x10000;
    } else {
        return 0;
    }
    if (i + len > text.size()) return 0;
    uint32_t cp = c & (0xFFu >> (len + 1));
    for (size_t k = 1; k < len; k++) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (next & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

static std::string clip(const std::string &text) {
    // `kSnippetMax` counts UTF-8 codepoints. Counting
    // bytes here both clipped non-ASCII lines at another place and could
    // split a character, leaving a diagnostic that was not UTF-8 itself.
    // Invalid input bytes are shown as U+FFFD for the same reason.
    std::string out;
    out.reserve(std::min(text.size(), kSnippetMax * 4));
    size_t chars = 0;
    size_t cut = 0;
    for (size_t i = 0; i < text.size();) {
        if (chars == kSnippetMax) {
            out.resize(cut);
            out += "...";
            return out;
        }
        if (chars == kSnippetMax - 3) cut = out.size();
        const size_t len = utf8_sequence_len(text, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            i++;
        } else {
            out.append(text, i, len);
            i += len;
        }
        chars++;
    }
    return out;
}

void emit_diag_line(const std::string &path, const std::vector<std::string> &lines,
                 const char *severity, int line, const std::string &msg) {
    std::fprintf(g_report, "%s",
                 (path + ":" + std::to_string(line) + ": " + severity + ": " + msg + "\n").c_str());
    if (line > 0 && static_cast<size_t>(line) <= lines.size()) {
        std::fprintf(g_report, "  | %s\n", clip(rstrip(lines[line - 1])).c_str());
    }
}

static void report(const std::string &path, const std::string &src,
                   const std::vector<TypeErr> &errs) {
    std::vector<std::string> lines = split_lines(src);
    for (const TypeErr &e : errs) {
        if (e.line > 0 && static_cast<size_t>(e.line) <= lines.size()) {
            emit_diag_line(path, lines, "error", e.line, e.msg);
        } else {
            std::fprintf(g_report, "%s: error: %s\n", path.c_str(), e.msg.c_str());
        }
    }
}

static void report(const std::string &path, const std::string &src,
                   const std::vector<BorrowDiag> &diags, const char *severity) {
    std::vector<std::string> lines = split_lines(src);
    for (const BorrowDiag &d : diags) {
        if (d.line > 0) {
            emit_diag_line(path, lines, severity, d.line, d.msg);
        } else {
            std::fprintf(g_report, "%s: %s: %s\n", path.c_str(), severity, d.msg.c_str());
        }
        if (d.note) {
            if (d.note->line > 0) {
                emit_diag_line(path, lines, "note", d.note->line, d.note->msg);
            } else {
                std::fprintf(g_report, "%s: note: %s\n", path.c_str(),
                             d.note->msg.c_str());
            }
        }
    }
}

std::set<std::string> imported_module_heads(const SourceFile &file) {
    std::set<std::string> heads;
    for (const auto &[name, target] : file.imports) {
        (void)target;
        heads.insert(name.substr(0, name.find('.')));
        size_t tail = name.rfind('.');
        heads.insert(name.substr(tail == std::string::npos ? 0 : tail + 1));
    }
    return heads;
}


std::set<std::string> exported_fn_names(const SourceFile &sf) {
    std::set<std::string> out;
    for (const auto &n : sf.pub) {
        auto it = sf.functions.find(n);
        if (it != sf.functions.end()) out.insert(it->second);
    }
    return out;
}

BorrowOut check_sibling_borrows(const std::vector<SourceFile> &files, std::size_t i) {
    std::map<std::string, std::vector<ParamMode>> extra;
    std::set<std::string> extra_consts;
    std::set<std::string> extra_scalar;
    std::map<std::string, std::vector<size_t>> extra_borrows;
    std::set<std::string> extra_owned;
    std::map<std::string, std::map<std::string, std::string>> extra_sfields;
    std::map<std::string, std::string> extra_frets;
    std::set<std::string> module_heads = imported_module_heads(files[i]);
    for (std::size_t j = 0; j < files.size(); ++j) {
        if (j == i) continue;
        FnTable ps = pub_sigs(files[j].body, exported_fn_names(files[j]));
        for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
        module_heads.insert(ps.heads.begin(), ps.heads.end());
        extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
        extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
        for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
        extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
        for (auto &kv : ps.struct_fields) extra_sfields[kv.first] = kv.second;
        for (auto &kv : ps.fn_rets) extra_frets[kv.first] = kv.second;
    }
    return borrow_check(files[i].body, extra, files[i].stray, module_heads,
                        extra_consts, extra_scalar, extra_borrows, extra_owned,
                        extra_sfields, extra_frets);
}

std::vector<TypeErr> check_sibling_types(const std::vector<SourceFile> &files, std::size_t i) {
    Sigs merged;
    std::map<std::string, Ty> merged_consts;
    std::map<std::string, std::map<std::string, Ty>> merged_structs;
    for (std::size_t j = 0; j < files.size(); ++j) {
        if (j == i) continue;
        collect_exports(files[j].body, merged_consts, merged_structs);
        collect_sigs(files[j].body, merged);
    }
    return type_check(files[i].body, merged, merged_consts, merged_structs);
}

int check_programs(const std::vector<SourceFile> &progs, bool quiet) {
    int bad = 0;
    for (size_t i = 0; i < progs.size(); i++) {
        BorrowOut out = check_sibling_borrows(progs, i);
        // Quiet on success: `run` is not `check`, and a program that runs must
        // not print a report first.
        if (!quiet || !out.errs.empty() || !out.warns.empty())
            std::fprintf(g_report, "%s: %d error(s), %d warning(s)\n", progs[i].path.c_str(),
                         static_cast<int>(out.errs.size()), static_cast<int>(out.warns.size()));
        report(progs[i].path, progs[i].src, out.errs, "error");
        report(progs[i].path, progs[i].src, out.warns, "warning");
        if (!out.errs.empty()) bad++;
    }

    for (size_t i = 0; i < progs.size(); i++) {
        const SourceFile &f = progs[i];
        std::vector<TypeErr> errs = check_sibling_types(progs, i);
        if (!quiet || !errs.empty())
            std::fprintf(g_report, "%s: %d type error(s)\n", f.path.c_str(),
                         static_cast<int>(errs.size()));
        report(f.path, f.src, errs);
        if (!errs.empty()) bad++;
    }
    return bad;
}

std::filesystem::path executable_path(const char *arg0) {
    return pkg_io::resolve_exe_path(arg0, true);
}

std::filesystem::path self_executable_path(const char *arg0) {
    return pkg_io::self_exe_path(arg0, true);
}

std::filesystem::path locate_lang_dir(const char *arg0) {
    const auto exe = self_executable_path(arg0);
    const auto dir = exe.parent_path();
    const std::filesystem::path cands[] = {
        dir.parent_path().parent_path(),
        dir.parent_path() / "lang",
        dir.parent_path(),
    };
    for (const auto &c : cands) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c / "engine" / "crt.h", ec) ||
            std::filesystem::is_directory(c / "std", ec)) {
            return c;
        }
    }
    return dir.parent_path().parent_path();
}

bool executable_on_path(const std::string &name) {
    const char *env_path = std::getenv("PATH");
    if (!env_path) return false;
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::string paths(env_path);
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(separator, start);
        if (end == std::string::npos) end = paths.size();
        auto dir = std::filesystem::path(paths.substr(start, end - start));
        std::error_code ec;
#if defined(_WIN32)
        if (std::filesystem::path(name).extension().empty() &&
            std::filesystem::is_regular_file(dir / (name + ".exe"), ec)) {
            return true;
        }
#endif
        if (std::filesystem::is_regular_file(dir / name, ec)) return true;
        if (end == paths.size()) break;
        start = end + 1;
    }
    return false;
}


// Native run caches assume a stable installed system toolchain.

std::string sha256_hex(const std::string &data) {
    return pkg_io::sha256_hex(data);
}

bool sha256_file(const std::filesystem::path &path, std::string &digest,
                 uint64_t &size, std::string &error) {
    return pkg_io::sha256_file_sized(path, digest, size, error);
}

std::filesystem::path cache_root() {
    auto try_dir = [](const std::filesystem::path &cand) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(cand, ec);
        if (ec || !std::filesystem::is_directory(cand, ec)) return false;
#if !defined(_WIN32)
        if (::access(cand.c_str(), W_OK | X_OK) != 0) return false;
#endif
        return true;
    };
    if (const char *env = std::getenv("BRAT_CACHE"); env && *env) {
        std::filesystem::path p(env);
        if (try_dir(p)) return p;
    }
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        auto p = std::filesystem::path(xdg) / "brat";
        if (try_dir(p)) return p;
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        auto p = std::filesystem::path(home) / ".cache" / "brat";
        if (try_dir(p)) return p;
    }
#if defined(_WIN32)
    auto fb = std::filesystem::temp_directory_path() / "brat-cache";
    std::error_code ec;
    std::filesystem::create_directories(fb, ec);
    return fb;
#else
    auto tmp = std::filesystem::temp_directory_path();
    auto fb = tmp / ("brat-cache-" + std::to_string(static_cast<unsigned long>(getuid())));
    ::mkdir(fb.c_str(), 0700);
    struct stat st {};
    if (::lstat(fb.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
        st.st_uid == getuid()) {
        (void)::chmod(fb.c_str(), 0700);
        return fb;
    }
    std::string tmpl = (tmp / ("brat-cache-" + std::to_string(static_cast<unsigned long>(getuid())) + "-XXXXXX")).string();
    if (char *made = ::mkdtemp(tmpl.data())) return std::filesystem::path(made);
    return fb;
#endif
}

int main_cc_timeout_ms() {
    return pkg_io::cc_timeout_ms();
}

int run_process(const std::vector<std::string> &args, int timeout_ms, bool *timed_out) {
    pkg_io::ProcOptions opts;
    opts.timeout_ms = timeout_ms;
    opts.search_path = true;
    opts.capture_output = false;
    opts.null_stdin = false;
    opts.perror_on_exec_fail = true;
    const pkg_io::ProcResult res = pkg_io::run_proc(args, opts);
    if (timed_out) *timed_out = res.timed_out;
    return res.exit_code;
}


static std::string compiler_cache_context(const std::string &compiler) {
    const auto parts = split_compiler_cmd(compiler);
    if (parts.empty()) return {};
    const auto resolved = executable_path(parts[0].c_str());
    std::string digest, error;
    uint64_t size = 0;
    if (!sha256_file(resolved, digest, size, error)) return {};
    std::vector<std::string> variables;
#if defined(_WIN32)
    char **environment = _environ;
#elif defined(__APPLE__)
    char **environment = *_NSGetEnviron();
#else
    char **environment = environ;
#endif
    for (char **entry = environment; entry && *entry; ++entry)
        variables.emplace_back(*entry);
    std::sort(variables.begin(), variables.end());
    std::string material = resolved.string() + "|" + digest + "|" +
                           std::filesystem::current_path().string();
    for (size_t i = 1; i < parts.size(); ++i)
        material += "|arg=" + std::to_string(parts[i].size()) + ":" + parts[i];
    for (const auto &entry : variables)
        material += "|" + std::to_string(entry.size()) + ":" + entry;
    return sha256_hex(material);
}

static bool default_compile_environment(const std::string &compiler) {
    std::ifstream executable(executable_path(compiler.c_str()), std::ios::binary);
    char prefix[2]{};
    executable.read(prefix, sizeof(prefix));
    if (!executable || (prefix[0] == '#' && prefix[1] == '!')) return false;
    for (const char *name : {"BRAT_CC", "CC", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
                             "OBJC_INCLUDE_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX",
                             "LIBRARY_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"}) {
        if (std::getenv(name)) return false;
    }
    const char *disabled = std::getenv("BRAT_NATIVE_CACHE_DISABLE");
    return !disabled || std::strcmp(disabled, "1") != 0;
}

static int bundle_driver(const char *arg0, const std::filesystem::path &output) {
    try {
        const auto executable = self_executable_path(arg0);
        const auto lang = locate_lang_dir(arg0);
        const auto repository = lang.parent_path();
        const auto runtime = lang / "engine" / "crt.h";
        if (!std::filesystem::is_regular_file(runtime))
            throw std::runtime_error("runtime header not found next to brat-cxx");
        if (std::filesystem::exists(std::filesystem::symlink_status(output)))
            throw std::runtime_error("output already exists: " + output.string());

        BuildStage stage(output);
        const auto root = stage.path / "bundle";
        const auto binary_dir = root / "lang" / "cxx" / "bin";
        const auto engine_dir = root / "lang" / "engine";
        std::filesystem::create_directories(binary_dir);
        std::filesystem::create_directories(engine_dir);
        const auto bundled_binary = binary_dir / executable.filename();
        std::filesystem::copy_file(executable, bundled_binary);
        std::filesystem::permissions(bundled_binary,
                                     std::filesystem::status(executable).permissions());
        std::filesystem::copy_file(runtime, engine_dir / "crt.h");
        const auto crt_parts = lang / "engine" / "crt";
        if (std::filesystem::is_directory(crt_parts)) {
            std::filesystem::copy(crt_parts, engine_dir / "crt",
                                  std::filesystem::copy_options::recursive);
        }
        const auto std_src = lang / "std";
        if (std::filesystem::is_directory(std_src)) {
            std::filesystem::copy(std_src, root / "lang" / "std",
                                  std::filesystem::copy_options::recursive);
        }
        for (const char *name : {"README.md", "LICENSE"}) {
            const auto source = repository / name;
            if (std::filesystem::is_regular_file(source))
                std::filesystem::copy_file(source, root / name);
        }
        const auto top_bin = root / "bin";
        std::filesystem::create_directories(top_bin);
#if defined(_WIN32)
        {
            std::ofstream cmd_out(top_bin / "brat.cmd", std::ios::binary | std::ios::trunc);
            cmd_out << "@echo off\r\n\"%~dp0..\\lang\\cxx\\bin\\"
                    << executable.filename().string() << "\" %*\r\n";
        }
#else
        {
            std::error_code sym_ec;
            std::filesystem::create_symlink(
                std::filesystem::path("..") / "lang" / "cxx" / "bin" / executable.filename(),
                top_bin / "brat", sym_ec);
        }
#endif
        std::filesystem::rename(root, output);
        std::printf("bundle ok %s\n", output.string().c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat bundle: %s\n", e.what());
        return 1;
    }
}

static bool expr_has_plugin_call(const Expr *e);
static bool stmts_have_plugin_call(const std::vector<StmtP> &body);

static bool expr_has_plugin_call(const Expr *e) {
    if (!e) return false;
    if (e->k == EK::Call &&
        (e->a == "lib_open" || e->a == "lib_sym" || e->a == "lib_close" || e->a == "call"))
        return true;
    for (const auto &k : e->kids)
        if (expr_has_plugin_call(k.get())) return true;
    if (e->fn && stmts_have_plugin_call(e->fn->body)) return true;
    return false;
}

static bool stmts_have_plugin_call(const std::vector<StmtP> &body) {
    for (const auto &sp : body) {
        if (!sp) continue;
        const Stmt &s = *sp;
        if (expr_has_plugin_call(s.e1.get()) || expr_has_plugin_call(s.e2.get()) ||
            expr_has_plugin_call(s.e3.get()))
            return true;
        if (stmts_have_plugin_call(s.body) || stmts_have_plugin_call(s.body2)) return true;
        for (const Stmt *el = s.els.get(); el != nullptr; el = el->els.get()) {
            if (expr_has_plugin_call(el->e1.get()) || stmts_have_plugin_call(el->body))
                return true;
        }
        for (const auto &f : s.fns)
            if (stmts_have_plugin_call(f.body)) return true;
    }
    return false;
}

bool has_extern_decls(const std::vector<StmtP> &body) {
    for (const auto &s : body) {
        if (!s) continue;
        if (s->k == SK::Extern || s->k == SK::ExternStruct) return true;
        if (s->k == SK::Pub && has_extern_decls(s->body)) return true;
    }
    return stmts_have_plugin_call(body);
}

static std::string ffi_data_model(const std::string &triple) {
    if (triple.empty()) return "lp64";
    std::string t;
    t.reserve(triple.size());
    for (unsigned char c : triple) t.push_back(static_cast<char>(std::tolower(c)));
    if (t.find("windows") != std::string::npos || t.find("win32") != std::string::npos ||
        t.find("msvc") != std::string::npos || t.find("mingw") != std::string::npos)
        return "llp64";
    static const char *const ilp32_pfx[] = {
        "i386", "i486", "i586", "i686", "arm", "armv", "thumb",
        "wasm32", "mips", "riscv32", "powerpc-", "x86-64-32"
    };
    for (const char *p : ilp32_pfx)
        if (t.rfind(p, 0) == 0) return "ilp32";
    if (t.find("-gnux32") != std::string::npos || t.find("-x32") != std::string::npos ||
        t.find("ilp32") != std::string::npos)
        return "ilp32";
    return "lp64";
}

bool is_safe_pkg_ident(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_safe_flat_filename(std::string_view s) {
    if (s.empty() || s == "." || s == ".." || s.size() > 255) return false;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '\0' || static_cast<unsigned char>(c) < 32)
            return false;
    }
    return true;
}

bool is_safe_header_filename(std::string_view s) {
    if (!is_safe_flat_filename(s) || s.size() < 3 || s.substr(s.size() - 2) != ".h")
        return false;
    static const std::set<std::string_view> reserved = {
        "crt.h", "stdio.h", "stdlib.h", "stdint.h", "stddef.h", "stdbool.h",
        "string.h", "errno.h", "math.h", "limits.h", "inttypes.h", "unistd.h",
        "pthread.h", "fcntl.h", "signal.h", "time.h", "ctype.h", "assert.h"
    };
    return reserved.count(s) == 0;
}

bool confined_subpath(const std::filesystem::path &base_dir,
                             std::string_view rel,
                             std::filesystem::path &out_path) {
    if (rel.empty() || rel.front() == '/' || rel.find('\\') != std::string_view::npos ||
        rel.find('\0') != std::string_view::npos)
        return false;
    size_t pos = 0;
    while (pos < rel.size()) {
        const size_t sl = rel.find('/', pos);
        const std::string_view seg = (sl == std::string_view::npos) ? rel.substr(pos)
                                                                    : rel.substr(pos, sl - pos);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (sl == std::string_view::npos) break;
        pos = sl + 1;
    }
    std::error_code ec;
    const auto canon_base = std::filesystem::weakly_canonical(base_dir, ec);
    if (ec) return false;
    const auto candidate = base_dir / std::string(rel);
    if (std::filesystem::is_symlink(candidate, ec)) return false;
    const auto canon_cand = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) return false;
    const std::string bs = canon_base.generic_string();
    const std::string cs = canon_cand.generic_string();
    if (cs.size() <= bs.size() || cs.compare(0, bs.size(), bs) != 0 || cs[bs.size()] != '/')
        return false;
    out_path = canon_cand;
    return true;
}

bool verify_bratbind_bytes(const SourceFile &sf, const std::string &target,
                                  std::string_view raw_bind) {
    std::filesystem::path bp(sf.path);
    bp.replace_extension(".bratbind");
    std::map<std::string, std::string> sibling_objs;
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::absolute(bp, ec).parent_path();
    for (const auto &ent : std::filesystem::directory_iterator(parent, ec)) {
        if (ent.is_symlink(ec) || !ent.is_regular_file(ec)) continue;
        const std::string ext = ent.path().extension().string();
        if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib" ||
            ext == ".so" || ext == ".dylib" || ext == ".dll")
            sibling_objs[ent.path().filename().string()] = ent.path().string();
    }
    std::string err;
    return native_ffi::verify_bratbind_bytes(sf.src, raw_bind, target, sibling_objs, bp.string(), err);
}

bool module_has_valid_bratbind(const SourceFile &sf, const std::string &target) {
    std::filesystem::path bp(sf.path);
    bp.replace_extension(".bratbind");
    std::string raw_bind;
    if (!read_file(bp.string(), raw_bind)) return false;
    return verify_bratbind_bytes(sf, target, raw_bind);
}


bool has_unproven_externs(const std::vector<SourceFile> &files, const std::string &target) {
    for (const auto &sf : files) {
        if (!has_extern_decls(sf.body) && !native_ffi::has_any_ffi_surface(sf.body)) continue;
        if (!module_has_valid_bratbind(sf, target)) return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> module_source_pairs(const std::vector<SourceFile> &files) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(files.size());
    for (const auto &sf : files) out.emplace_back(sf.path, sf.src);
    return out;
}

std::string host_default_triple() {
#if defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64-windows";
#else
    return "x86_64-windows";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "aarch64-darwin";
#else
    return "x86_64-darwin";
#endif
#else
#if defined(__aarch64__)
    return "aarch64-linux";
#elif defined(__i386__)
    return "i686-linux";
#else
    return "x86_64-linux";
#endif
#endif
}

static std::pair<int, int> ffi_field_shape(
    const std::string &ty, int ptr_bytes, int a8_bytes,
    const std::map<std::string, std::pair<int, int>> &resolved) {
    std::string base = ty;
    int mult = 1;
    const size_t lb = ty.find('[');
    if (lb != std::string::npos && ty.back() == ']' && ty.rfind("ptr[", 0) != 0) {
        base = ty.substr(0, lb);
        int cnt = 0;
        for (size_t i = lb + 1; i + 1 < ty.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(ty[i]))) { cnt = -1; break; }
            cnt = cnt * 10 + (ty[i] - '0');
        }
        if (cnt <= 0) return {-1, -1};
        mult = cnt;
    }
    int sz = -1, al = -1;
    if (base == "i8" || base == "u8" || base == "bool") { sz = 1; al = 1; }
    else if (base == "i16" || base == "u16") { sz = 2; al = 2; }
    else if (base == "i32" || base == "u32" || base == "f32") { sz = 4; al = 4; }
    else if (base == "i64" || base == "u64" || base == "f64") { sz = 8; al = a8_bytes; }
    else if (base == "str" || base == "ptr" || base.rfind("ptr[", 0) == 0 ||
             base.rfind("fn(", 0) == 0) {
        sz = ptr_bytes; al = ptr_bytes;
    } else {
        auto it = resolved.find(base);
        if (it != resolved.end()) { sz = it->second.first; al = it->second.second; }
    }
    if (sz < 0) return {-1, -1};
    return {sz * mult, al};
}

std::string build_ffi_contract_c(const std::vector<SourceFile> &files,
                                        const std::vector<StmtP> &flat,
                                        const std::string &target,
                                        const std::string &cli_header,
                                        const std::vector<std::string> &cli_objects) {
    struct RawFn {
        std::string name;
        std::optional<std::string> lib;
        std::optional<std::string> ret;
        std::vector<std::optional<std::string>> params;
        std::optional<std::string> varargs;
    };
    std::vector<RawFn> raw_fns;
    std::map<std::string, std::vector<StructField>> structs;
    std::set<std::string> packed_set;
    std::set<std::string> union_set;

    for (const auto &sp : flat) {
        if (!sp) continue;
        const Stmt *s = (sp->k == SK::Pub && !sp->body.empty()) ? sp->body.front().get() : sp.get();
        if (!s) continue;
        if (s->k == SK::Extern) {
            RawFn rf;
            rf.name = s->ext.name;
            rf.lib = s->ext.lib;
            rf.ret = s->ext.ret;
            rf.varargs = s->ext.varargs;
            for (const Param &p : s->ext.params)
                rf.params.push_back(p.ann);
            raw_fns.push_back(std::move(rf));
        } else if (s->k == SK::ExternStruct) {
            structs[s->a] = s->fields;
            if (s->mut || (s->opt1 && *s->opt1 == "packed")) packed_set.insert(s->a);
            if (s->opt1 && *s->opt1 == "union") union_set.insert(s->a);
        }
    }
    if (raw_fns.empty() && structs.empty()) return "";

    std::sort(raw_fns.begin(), raw_fns.end(),
              [](const RawFn &a, const RawFn &b) { return a.name < b.name; });

    const std::string triple = target.empty() ? host_default_triple() : target;
    const std::string model = ffi_data_model(triple);
    const int ptr_bits = (model == "ilp32") ? 32 : 64;
    const int long_bits = (model == "lp64") ? 64 : 32;
    std::string tl = triple;
    for (char &ch : tl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool i386_sysv =
        (model == "ilp32") &&
        (tl.rfind("i386", 0) == 0 || tl.rfind("i486", 0) == 0 ||
         tl.rfind("i586", 0) == 0 || tl.rfind("i686", 0) == 0) &&
        tl.find("win") == std::string::npos && tl.find("mingw") == std::string::npos;
    const int wide_align_bits = i386_sysv ? 32 : 64;
    const int ptr_bytes = ptr_bits / 8;
    const int long_bytes = long_bits / 8;
    const int a8_bytes = wide_align_bits / 8;

    std::map<std::string, std::pair<int, int>> resolved_sz;
    std::map<std::string, std::vector<int>> resolved_offs;
    bool progress = true;
    while (progress) {
        progress = false;
        for (const auto &[sname, flds] : structs) {
            if (resolved_sz.count(sname)) continue;
            const bool is_packed = packed_set.count(sname) > 0;
            const bool is_union = union_set.count(sname) > 0;
            int off = 0, max_sz = 0, max_al = 1;
            std::vector<int> offs;
            bool ok = true;
            for (const auto &f : flds) {
                if (!f.ann) { ok = false; break; }
                auto [fsz, fal] = ffi_field_shape(*f.ann, ptr_bytes, a8_bytes, resolved_sz);
                if (fsz < 0) { ok = false; break; }
                if (is_union) {
                    if (fal > max_al) max_al = fal;
                    if (fsz > max_sz) max_sz = fsz;
                    offs.push_back(0);
                } else {
                    const int eff_al = is_packed ? 1 : fal;
                    if (eff_al > max_al) max_al = eff_al;
                    if (off % eff_al != 0) off += eff_al - (off % eff_al);
                    offs.push_back(off);
                    off += fsz;
                }
            }
            if (!ok) continue;
            if (is_union) {
                int usz = max_sz;
                if (usz % max_al != 0) usz += max_al - (usz % max_al);
                resolved_sz[sname] = {usz, max_al};
            } else {
                if (!is_packed && off % max_al != 0) off += max_al - (off % max_al);
                resolved_sz[sname] = {off, max_al};
            }
            resolved_offs[sname] = std::move(offs);
            progress = true;
        }
    }

    auto write_opt_str = [](lsp_wire::JsonWriter &w, const std::optional<std::string> &v) {
        if (v) w.str(*v);
        else w.null_val();
    };

    struct ProvedObj {
        std::string file;
        std::string sha256;
    };
    struct ProvedInfo {
        bool present = false;
        std::string header;
        std::string sha256;
        std::vector<ProvedObj> objs;
    } proved;

    if (!cli_header.empty()) {
        std::string hbytes;
        if (read_file(cli_header, hbytes)) {
            proved.present = true;
            proved.header = std::filesystem::path(cli_header).filename().string();
            proved.sha256 = sha256_hex(hbytes);
            for (const auto &op : cli_objects) {
                std::string obytes;
                if (read_file(op, obytes))
                    proved.objs.push_back({std::filesystem::path(op).filename().string(), sha256_hex(obytes)});
            }
        }
    }

    for (const auto &sf : files) {
        if (proved.present) break;
        if (!has_extern_decls(sf.body)) continue;
        std::filesystem::path bp(sf.path);
        bp.replace_extension(".bratbind");
        std::string raw_bind;
        if (read_file(bp.string(), raw_bind)) {
            lsp_wire::LspJsonTape btape;
            if (btape.parse(raw_bind) && btape.is_object(btape.root())) {
                const uint32_t hdr = btape.get(btape.root(), "header");
                const std::string_view hname = btape.str_view(btape.get(hdr, "name"));
                if (btape.is_object(hdr) && !hname.empty()) {
                    proved.present = true;
                    proved.header = std::filesystem::path(std::string(hname)).filename().string();
                    const std::string_view fsha = btape.str_view(btape.get(hdr, "sha256"));
                    proved.sha256 = std::string(!fsha.empty() ? fsha : btape.str_view(btape.get(hdr, "text_sha256")));
                    const uint32_t b_objs = btape.get(btape.root(), "objects");
                    if (btape.is_array(b_objs)) {
                        for (uint32_t oe = btape.at(b_objs).first_child;
                             oe != 0; oe = btape.at(oe).next_sibling) {
                            const std::string onm(btape.str_view(btape.get(oe, "name")));
                            proved.objs.push_back({
                                std::filesystem::path(onm).filename().string(),
                                std::string(btape.str_view(btape.get(oe, "sha256")))
                            });
                        }
                    }
                    break;
                }
            }
        }
    }

    lsp_wire::JsonWriter jw;
    jw.begin_obj();

    jw.key_lit("fns");
    jw.begin_arr();
    for (const RawFn &rf : raw_fns) {
        jw.begin_obj();
        jw.key_lit("lib"); write_opt_str(jw, rf.lib);
        jw.key_lit("name"); jw.str(rf.name);
        jw.key_lit("params");
        jw.begin_arr();
        for (const auto &pa : rf.params) write_opt_str(jw, pa);
        jw.end_arr();
        jw.key_lit("ret"); write_opt_str(jw, rf.ret);
        jw.key_lit("varargs"); write_opt_str(jw, rf.varargs);
        jw.end_obj();
    }
    jw.end_arr();

    jw.key_lit("proved");
    if (!proved.present) {
        jw.null_val();
    } else {
        jw.begin_obj();
        jw.key_lit("header"); jw.str(proved.header);
        if (!proved.objs.empty()) {
            jw.key_lit("objects");
            jw.begin_arr();
            for (const auto &po : proved.objs) {
                jw.begin_obj();
                jw.key_lit("file"); jw.str(po.file);
                jw.key_lit("sha256"); jw.str(po.sha256);
                jw.end_obj();
            }
            jw.end_arr();
        }
        jw.key_lit("sha256"); jw.str(proved.sha256);
        jw.end_obj();
    }

    jw.key_lit("structs");
    jw.begin_arr();
    for (const auto &[sname, flds] : structs) {
        const bool is_packed = packed_set.count(sname) > 0;
        const bool is_union = union_set.count(sname) > 0;
        auto it = resolved_sz.find(sname);
        jw.begin_obj();
        jw.key_lit("align");
        if (it == resolved_sz.end()) jw.null_val();
        else jw.int64(it->second.second);
        jw.key_lit("fields");
        jw.begin_arr();
        if (it != resolved_sz.end()) {
            const auto &offs = resolved_offs[sname];
            for (size_t i = 0; i < flds.size(); ++i) {
                jw.begin_obj();
                jw.key_lit("name"); jw.str(flds[i].name);
                jw.key_lit("offset"); jw.int64(offs[i]);
                jw.key_lit("type"); write_opt_str(jw, flds[i].ann);
                jw.end_obj();
            }
        }
        jw.end_arr();
        jw.key_lit("name"); jw.str(sname);
        jw.key_lit("packed"); jw.boolean(is_packed);
        jw.key_lit("size");
        if (it == resolved_sz.end()) jw.null_val();
        else jw.int64(it->second.first);
        if (is_union) {
            jw.key_lit("union");
            jw.boolean(true);
        }
        jw.end_obj();
    }
    jw.end_arr();

    jw.key_lit("target");
    jw.begin_obj();
    jw.key_lit("long"); jw.int64(long_bytes);
    jw.key_lit("model"); jw.str(model);
    jw.key_lit("ptr"); jw.int64(ptr_bytes);
    jw.key_lit("triple");
    if (target.empty()) jw.null_val();
    else jw.str(target);
    jw.key_lit("wide_align"); jw.int64(a8_bytes);
    jw.end_obj();

    jw.key_lit("version"); jw.int64(1);
    jw.end_obj();

    const std::string payload = "BRATFFI1 " + std::string(jw.view()) + "BRATFFI1-END";
    std::string out = "/* BRAT FFI contract: the extern surface this binary was proved against.\n"
                      "   `brat ffi verify --bin FILE` re-proves it. */\n"
                      "static const char brat_ffi_contract[] __attribute__((used)) =\n";
    const size_t width = 180;
    for (size_t i = 0; i < payload.size(); i += width) {
        const std::string piece = payload.substr(i, width);
        out += "  \"";
        for (char ch : piece) {
            if (ch == '\\' || ch == '"') out.push_back('\\');
            out.push_back(ch);
        }
        out += (i + width >= payload.size()) ? "\";\n" : "\"\n";
    }
    return out;
}


static bool collect_vendored_c_objects(const std::vector<SourceFile> &files,
                                       const std::string &compiler,
                                       bool zig,
                                       const std::string &target,
                                       bool shared,
                                       std::vector<std::string> &out_objs,
                                       std::string &out_digest,
                                       std::string &out_err) {
    std::vector<std::filesystem::path> pkg_dirs;
    std::set<std::string> seen_pkgs;
    std::error_code ec;
    for (const auto &sf : files) {
        std::filesystem::path cur = std::filesystem::weakly_canonical(sf.path, ec).parent_path();
        while (!cur.empty()) {
            const auto mp = cur / "brat.pkg.json";
            if (std::filesystem::is_regular_file(mp, ec)) {
                const std::string k = cur.string();
                if (!seen_pkgs.count(k)) {
                    seen_pkgs.insert(k);
                    pkg_dirs.push_back(cur);
                }
                break;
            }
            const auto parent = cur.parent_path();
            if (parent == cur || cur.filename() == "vendor") break;
            cur = parent;
        }
    }
    if (pkg_dirs.empty()) return true;

    const auto obj_cache = cache_root() / "c_obj";
    std::filesystem::create_directories(obj_cache, ec);
    const std::string pkind = platform_kind(target);
    const std::string compiler_context = compiler_cache_context(compiler);

    for (const auto &pkg_dir : pkg_dirs) {
        const std::string pkg_name = pkg_dir.filename().string();
        if (!is_safe_pkg_ident(pkg_name)) {
            out_err = "invalid package directory name " + pkg_name;
            return false;
        }
        const auto vendor_dir = pkg_dir.parent_path();
        if (vendor_dir.filename() == "vendor") {
            const auto lock_p = vendor_dir.parent_path() / "brat.lock";
            if (std::filesystem::is_regular_file(lock_p, ec)) {
                std::string raw_lock;
                if (!read_file(lock_p.string(), raw_lock)) {
                    out_err = "cannot read " + lock_p.string();
                    return false;
                }
                lsp_wire::LspJsonTape lock_tape;
                if (!lock_tape.parse(raw_lock) || !lock_tape.is_array(lock_tape.root())) {
                    out_err = "cannot parse " + lock_p.string();
                    return false;
                }
                for (uint32_t it = lock_tape.at(lock_tape.root()).first_child;
                     it != 0; it = lock_tape.at(it).next_sibling) {
                    if (!lock_tape.is_object(it)) continue;
                    if (lock_tape.str_view(lock_tape.get(it, "archive")) != pkg_name) continue;
                    const uint32_t files_j = lock_tape.get(it, "files");
                    if (!lock_tape.is_object(files_j)) continue;
                    uint32_t fk = lock_tape.at(files_j).first_child;
                    while (fk != 0) {
                        const uint32_t fv = lock_tape.at(fk).next_sibling;
                        if (fv == 0) break;
                        const std::string krel(lock_tape.str_view(fk));
                        const std::string prefix = pkg_name + "/";
                        std::filesystem::path fp;
                        if (krel.rfind(prefix, 0) != 0 ||
                            !confined_subpath(pkg_dir, std::string_view(krel).substr(prefix.size()), fp)) {
                            out_err = "unsafe locked file path " + krel;
                            return false;
                        }
                        std::string fbytes;
                        if (!read_file(fp.string(), fbytes)) {
                            out_err = "missing locked file " + krel;
                            return false;
                        }
                        if (sha256_hex(fbytes) != lock_tape.str_view(fv)) {
                            out_err = "HASH MISMATCH in vendored package file " + krel;
                            return false;
                        }
                        fk = lock_tape.at(fv).next_sibling;
                    }
                }
            }
        }
        std::string raw_manifest;
        if (!read_file((pkg_dir / "brat.pkg.json").string(), raw_manifest)) {
            out_err = "cannot read " + (pkg_dir / "brat.pkg.json").string();
            return false;
        }
        lsp_wire::LspJsonTape mtape;
        if (!mtape.parse(raw_manifest) || !mtape.is_object(mtape.root())) {
            out_err = "invalid JSON in " + (pkg_dir / "brat.pkg.json").string();
            return false;
        }
        const uint32_t cs_arr = mtape.get(mtape.root(), "c_sources");
        if (!mtape.is_array(cs_arr) || mtape.at(cs_arr).first_child == 0) continue;

        std::string hdr_material;
        std::vector<std::pair<std::string, std::string>> staged_hdrs;
        for (const auto &dent : std::filesystem::recursive_directory_iterator(pkg_dir, ec)) {
            if (!dent.is_regular_file(ec) || dent.is_symlink(ec)) continue;
            const std::string ext = dent.path().extension().string();
            if (ext != ".h" && ext != ".bratbind") continue;
            std::string hbytes;
            if (read_file(dent.path().string(), hbytes)) {
                hdr_material += dent.path().filename().string() + ":" + sha256_hex(hbytes) + ";";
                if (ext == ".bratbind") {
                    lsp_wire::LspJsonTape btape;
                    if (!btape.parse(hbytes) || !btape.is_object(btape.root())) {
                        out_err = "invalid JSON in " + dent.path().string();
                        return false;
                    }
                    const uint32_t hdr = btape.get(btape.root(), "header");
                    if (btape.is_object(hdr)) {
                        const std::string raw_hname(btape.str_view(btape.get(hdr, "name")));
                        const std::string hname = std::filesystem::path(raw_hname).filename().string();
                        const std::string_view htext = btape.str_view(btape.get(hdr, "text"));
                        if (!raw_hname.empty() && !htext.empty()) {
                            if (!is_safe_header_filename(hname)) {
                                out_err = "unsafe header name in " + dent.path().string() + ": " + raw_hname;
                                return false;
                            }
                            staged_hdrs.emplace_back(hname, std::string(htext));
                        }
                    }
                }
            }
        }
        const auto inc_dir = obj_cache / ("inc_" + pkg_name + "_" + sha256_hex(hdr_material).substr(0, 16));
        std::filesystem::create_directories(inc_dir, ec);
        for (const auto &[hname, htext] : staged_hdrs) {
            const auto hdest = inc_dir / hname;
            if (!std::filesystem::is_regular_file(hdest, ec)) {
                const auto htmp = inc_dir / (hname + "." + std::to_string(static_cast<long long>(
#if defined(_WIN32)
                    GetCurrentProcessId()
#else
                    getpid()
#endif
                )) + ".tmp");
                std::ofstream oh(htmp, std::ios::binary | std::ios::trunc);
                oh.write(htext.data(), static_cast<std::streamsize>(htext.size()));
                oh.close();
                if (!oh) {
                    out_err = "failed writing staged header " + hname;
                    return false;
                }
                std::filesystem::rename(htmp, hdest, ec);
                if (ec) std::filesystem::remove(htmp, ec);
            }
        }
        for (uint32_t cs_item = mtape.at(cs_arr).first_child;
             cs_item != 0; cs_item = mtape.at(cs_item).next_sibling) {
            if (!mtape.is_string(cs_item)) {
                out_err = "non-string c_sources entry in " + pkg_name;
                return false;
            }
            std::string cs(mtape.str_view(cs_item));
            if (cs.rfind(pkg_name + "/", 0) == 0) cs = cs.substr(pkg_name.size() + 1);
            if (cs.size() < 3 || cs.substr(cs.size() - 2) != ".c") {
                out_err = "c_source must end in .c: " + cs;
                return false;
            }
            std::filesystem::path cpath;
            if (!confined_subpath(pkg_dir, cs, cpath)) {
                out_err = "c_source path escapes package directory: " + cs;
                return false;
            }
            std::string cbytes;
            if (!read_file(cpath.string(), cbytes)) {
                out_err = "missing c_source " + pkg_name + "/" + cs;
                return false;
            }
            BuildStage stage(obj_cache / "object");
            const auto snapshot = stage.path / "input.i";
            std::vector<std::string> flags{"-std=gnu11", "-O2", "-fwrapv", "-I", pkg_dir.string(),
                                           "-I", inc_dir.string()};
            if (!target.empty()) flags.insert(flags.end(), {"-target", target});
            if (shared && pkind != "windows") flags.push_back("-fPIC");
            const auto cc_parts = split_compiler_cmd(compiler);
            if (cc_parts.empty()) {
                out_err = "empty compiler command";
                return false;
            }
            std::vector<std::string> preprocess = cc_parts;
            if (zig) preprocess.push_back("cc");
            preprocess.insert(preprocess.end(), flags.begin(), flags.end());
            preprocess.insert(preprocess.end(), {"-E", cpath.string(), "-o", snapshot.string()});
            const int cc_timeout_ms = main_cc_timeout_ms();
            bool timed_out = false;
            if (run_process(preprocess, cc_timeout_ms, &timed_out) != 0) {
                out_err = "failed preprocessing " + pkg_name + "/" + cs +
                          (timed_out ? " (compiler timed out)" : "");
                return false;
            }
            std::string preprocessed;
            if (!read_file(snapshot.string(), preprocessed)) {
                out_err = "cannot read preprocessed source for " + pkg_name + "/" + cs;
                return false;
            }
            const bool reusable = !compiler_context.empty() &&
                                  preprocessed.find(".incbin") == std::string::npos &&
                                  !(std::getenv("BRAT_NATIVE_CACHE_DISABLE") &&
                                    std::strcmp(std::getenv("BRAT_NATIVE_CACHE_DISABLE"), "1") == 0);
            const std::string key = sha256_hex(
                "c-object-v2|" + preprocessed + "|" + compiler_context + "|" +
                (zig ? "zig|" : "") + target + "|" + (shared ? "pic" : "nopic") +
                (reusable ? "" : stage.path.string())).substr(0, 32);
            const auto obj_path = obj_cache / (key + ".o");
            if (!reusable || !std::filesystem::is_regular_file(obj_path, ec)) {
                const auto temporary = stage.path / "object.o";
                std::vector<std::string> command = cc_parts;
                if (zig) command.push_back("cc");
                command.insert(command.end(), flags.begin(), flags.end());
                // Assembler includes are not expanded by the C preprocessor.
                const auto &input = preprocessed.find(".incbin") == std::string::npos ? snapshot : cpath;
                command.insert(command.end(), {"-c", input.string(), "-o", temporary.string()});
                if (run_process(command, cc_timeout_ms, &timed_out) != 0) {
                    out_err = "failed compiling " + pkg_name + "/" + cs +
                              (timed_out ? " (compiler timed out)" : "");
                    return false;
                }
                std::filesystem::rename(temporary, obj_path, ec);
                if (ec) {
                    std::error_code existing_error;
                    if (!reusable || !std::filesystem::is_regular_file(obj_path, existing_error)) {
                        out_err = "failed staging object for " + pkg_name + "/" + cs + ": " + ec.message();
                        return false;
                    }
                }
            }
            std::string object_hash, hash_error;
            uint64_t object_size = 0;
            if (!sha256_file(obj_path, object_hash, object_size, hash_error)) {
                out_err = hash_error;
                return false;
            }
            out_digest += key + ":" + object_hash + ",";
            out_objs.push_back(obj_path.string());
        }
    }
    return true;
}

std::string platform_kind(const std::string &target) {
    if (!target.empty()) {
        std::string t = target;
        for (char &ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (t.find("windows") != std::string::npos || t.find("win32") != std::string::npos ||
            t.find("msvc") != std::string::npos || t.find("mingw") != std::string::npos)
            return "windows";
        if (t.find("macos") != std::string::npos || t.find("darwin") != std::string::npos ||
            t.find("apple") != std::string::npos)
            return "macos";
        if (t.find("linux") != std::string::npos && t.find("android") == std::string::npos)
            return "linux";
        return "posix";
    }
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

static std::string format_link_lib(const std::string &lib, const std::string &pkind) {
    if (std::filesystem::path(lib).is_absolute() || std::filesystem::exists(lib))
        return lib;
    if (pkind == "macos" || pkind == "windows") {
        std::string s = lib;
        if (s.rfind("lib", 0) == 0 && s.size() > 3) s = s.substr(3);
        for (const char *ext : {".dylib", ".dll", ".so", ".a", ".lib"}) {
            const size_t el = std::strlen(ext);
            if (s.size() > el && s.compare(s.size() - el, el, ext) == 0) {
                s.resize(s.size() - el);
                break;
            }
        }
        return "-l" + s;
    }
    if (lib.find('.') != std::string::npos)
        return "-l:" + lib;
    return "-l" + lib;
}

static void maybe_append_crt_pch(const std::string &compiler,
                                 const std::filesystem::path &engine,
                                 const std::string &emitted_c,
                                 bool static_mode,
                                 bool shared_pic,
                                 uint64_t memory_limit,
                                 std::vector<std::string> &args) {
    if (const char *ev = std::getenv("BRAT_NO_PCH"); ev && std::strcmp(ev, "1") == 0) return;
    if (const char *nc = std::getenv("BRAT_NATIVE_CACHE_DISABLE"); nc && std::strcmp(nc, "1") == 0)
        return;

    const auto cc_parts = split_compiler_cmd(compiler);
    if (cc_parts.empty()) return;

    const auto resolved = executable_path(cc_parts[0].c_str());
    if (resolved.empty()) return;
    {
        std::ifstream exe_in(resolved, std::ios::binary);
        char prefix[2]{};
        exe_in.read(prefix, sizeof(prefix));
        if (!exe_in || (prefix[0] == '#' && prefix[1] == '!')) return;
    }

    std::string fname = resolved.filename().string();
    for (char &ch : fname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool is_clang = fname.find("clang") != std::string::npos;
    bool is_gcc = !is_clang && fname.find("gcc") != std::string::npos;
#if defined(__APPLE__)
    if (!is_clang && !is_gcc && fname == "cc") is_clang = true;
#endif
    if (!is_gcc && !is_clang) return;

    std::vector<std::string> extra_cc_flags;
    for (size_t i = 1; i < cc_parts.size(); ++i) {
        const std::string &fl = cc_parts[i];
        if (fl == "cc") return;
        if (fl.rfind("-Wl,", 0) == 0 || fl.rfind("-l", 0) == 0 || fl.rfind("-L", 0) == 0)
            continue;
        extra_cc_flags.push_back(fl);
    }

    const std::string needle = "#include \"crt.h\"";
    const size_t inc_pos = emitted_c.find(needle);
    if (inc_pos == std::string::npos || inc_pos > 512) return;
    const std::string prelude = emitted_c.substr(0, inc_pos);

    const auto crt_file = engine / "crt.h";
    std::string cc_digest, crt_bundle, err;
    uint64_t sz = 0;
    if (!sha256_file(resolved, cc_digest, sz, err)) return;
    if (!read_crt_bundle(engine, crt_bundle)) return;
    const std::string crt_digest = sha256_hex(crt_bundle);

    std::vector<std::string> pch_flags = extra_cc_flags;
    if (is_gcc) {
        if (static_mode) pch_flags.push_back("-static");
        if (shared_pic) {
            pch_flags.push_back("-shared");
            pch_flags.push_back("-fPIC");
        }
    } else if (shared_pic) {
        pch_flags.push_back("-fPIC");
    }
    pch_flags.insert(pch_flags.end(), {"-std=gnu11", "-O2", "-fwrapv"});
    if (memory_limit)
        pch_flags.push_back("-DBRAT_MEM_BYTES=" + std::to_string(memory_limit));

    std::string mat = "pch2|" + resolved.string() + "|" + cc_digest + "|" + crt_digest + "|" + prelude;
    for (const auto &fl : pch_flags) mat += "|" + fl;
    for (const char *ev_name : {"CPATH", "C_INCLUDE_PATH", "COMPILER_PATH",
                                "GCC_EXEC_PREFIX", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"}) {
        if (const char *val = std::getenv(ev_name)) {
            mat += "|env:";
            mat += ev_name;
            mat += '=';
            mat += val;
        }
    }
    const std::string key = sha256_hex(mat).substr(0, 24);

    std::error_code ec;
    const auto pch_dir = cache_root() / "pch";
    std::filesystem::create_directories(pch_dir, ec);
    if (ec) return;

    const auto seen_path = pch_dir / ("crt_" + key + ".seen");
    const auto hdr_path = pch_dir / ("crt_" + key + ".h");
    const auto pch_path = pch_dir / ("crt_" + key + (is_clang ? ".h.pch" : ".h.gch"));
#if defined(_WIN32)
    const std::string pid_tag = std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
#else
    const std::string pid_tag = std::to_string(static_cast<unsigned long>(getpid()));
#endif

    // 2-hit cache admission: avoid paying a separate `cc -x c-header` invocation
    // on one-off subsystem combinations (such as a single all-stdlib bundle build).
    // First compile records `.seen`; the second and all subsequent builds with the
    // same prelude build and reuse the precompiled header.
    if (!std::filesystem::is_regular_file(pch_path, ec) &&
        !std::filesystem::is_regular_file(seen_path, ec)) {
        std::ofstream marker(seen_path, std::ios::binary | std::ios::trunc);
        return;
    }

    if (!std::filesystem::is_regular_file(hdr_path, ec)) {
        const auto hdr_tmp = pch_dir / ("crt_" + key + "." + pid_tag + ".h.tmp");
        {
            std::ofstream out(hdr_tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            const std::string body = prelude + "#include \"" + crt_file.generic_string() + "\"\n";
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!out) {
                std::filesystem::remove(hdr_tmp, ec);
                return;
            }
        }
        std::filesystem::rename(hdr_tmp, hdr_path, ec);
        if (ec) std::filesystem::remove(hdr_tmp, ec);
    }
    if (!std::filesystem::is_regular_file(hdr_path, ec)) return;

    if (!std::filesystem::is_regular_file(pch_path, ec)) {
        const auto pch_tmp = pch_dir / ("crt_" + key + "." + pid_tag + ".pch.tmp");
        std::vector<std::string> build_args{cc_parts[0]};
        build_args.insert(build_args.end(), pch_flags.begin(), pch_flags.end());
        build_args.insert(build_args.end(), {"-w", "-x", "c-header", hdr_path.string(), "-o", pch_tmp.string()});
        bool timed_out = false;
        const int rc = run_process(build_args, main_cc_timeout_ms(), &timed_out);
        if (rc != 0) {
            std::filesystem::remove(pch_tmp, ec);
            return;
        }
        std::filesystem::rename(pch_tmp, pch_path, ec);
        if (ec) std::filesystem::remove(pch_tmp, ec);
    }
    if (!std::filesystem::is_regular_file(pch_path, ec)) return;

    if (is_clang) {
        args.push_back("-include-pch");
        args.push_back(pch_path.string());
    } else {
        args.push_back("-include");
        args.push_back(hdr_path.string());
    }
}

static const char *no_check_forbidden_reason(const std::vector<SourceFile> &files) {
    for (const auto &sf : files) {
        if (has_arena_block_tokens(sf.src))
            return "arena blocks depend on the escape rules";
        for (const auto &tok : lex(sf.src)) {
            if (tok.text == "spawn")
                return "spawn blocks depend on the sharing rule";
            if (tok.text == "lib_open")
                return "plugin handles depend on the lifetime rule";
        }
    }
    return nullptr;
}

static int build_one(const char *arg0, const std::string &file,
                     const std::string &output, const std::string &target,
                     bool static_link, bool static_explicit, bool shared, int opt,
                     uint64_t memory_limit,
                     const std::vector<std::string> &link_flags = {},
                     bool unsafe_ffi = false,
                     bool trace_lines = false,
                     const std::string &ffi_header = {},
                     const std::vector<std::string> &ffi_objects = {},
                     bool no_check = false,
                     bool debug_info = false) {
    ModuleSet set = load_modules(file);
    mangle_modules(set);
    if (no_check) {
        if (const char *why = no_check_forbidden_reason(set.files)) {
            std::fprintf(stderr, "brat build: refusing --no-check for %s: %s, which only the checker applies.\n",
                         file.c_str(), why);
            return 1;
        }
    } else if (check_programs(set.files)) {
        std::fprintf(stderr, "brat build: refusing to build after checker errors\n");
        return 1;
    }
    if (native_ffi::ffi_gate(file, module_source_pairs(set.files), ffi_header, ffi_objects,
                             target, "brat build", unsafe_ffi) != 0) {
        return 1;
    }
    flatten(set, true);
    const std::string pkind = platform_kind(target);
    if (pkind != "linux" && static_link && !static_explicit) static_link = false;
    const auto libs = codegen::extern_libs(set.flat);
    if (!libs.empty() && static_link && !static_explicit) static_link = false;

    std::string compiler;
    if (const char *override = std::getenv("BRAT_CC"); override && *override)
        compiler = override;
    else if (const char *cc_env = std::getenv("CC"); cc_env && *cc_env && target.empty() && !static_explicit)
        compiler = cc_env;
    else if ((target.size() || static_explicit) && executable_on_path("zig"))
        compiler = "zig";
    else if (target.size()) {
        std::fprintf(stderr, "brat build: --target needs zig cc\n");
        return 1;
    } else if (executable_on_path("cc")) compiler = "cc";
    else if (executable_on_path("gcc")) compiler = "gcc";
    else if (executable_on_path("clang")) compiler = "clang";
    else {
        std::fprintf(stderr, "brat build: no C compiler found (set BRAT_CC)\n");
        return 1;
    }
    const auto cc_parts = split_compiler_cmd(compiler);
    if (cc_parts.empty()) {
        std::fprintf(stderr, "brat build: no C compiler found (set BRAT_CC)\n");
        return 1;
    }
    const bool zig_any = std::filesystem::path(cc_parts[0]).filename().string().rfind("zig", 0) == 0;
    const bool zig = zig_any && !(cc_parts.size() >= 2 && cc_parts[1] == "cc");
    if (target.size() && !zig_any) {
        std::fprintf(stderr, "brat build: --target needs zig cc\n");
        return 1;
    }

    const std::filesystem::path output_path(output);
    BuildStage stage(output_path);
    const auto cfile = stage.path / "program.c";
    const auto temporary_output =
        stage.path / (pkind == "windows" ? (shared ? "program.dll" : "program.exe") : "program");
    const auto engine = locate_lang_dir(arg0) / "engine";
    std::string emitted_c;
    try {
        const std::string root_path = set.files.empty() ? file : set.files[0].path;
        std::string c = codegen::emit_program(set.flat, dir_of(file), opt, nullptr, target,
                                              trace_lines, debug_info, root_path,
                                              &set.stmt_files, &set.fn_files);
        if (static_link && !static_explicit &&
            (!link_flags.empty() ||
             c.find("vlib_open(") != std::string::npos ||
             c.find("vhttps_req(") != std::string::npos ||
             c.find("vhttps_request_raw(") != std::string::npos ||
             c.find("vsql_open(") != std::string::npos))
            static_link = false;
        const std::string contract_snippet = build_ffi_contract_c(set.files, set.flat, target, ffi_header, ffi_objects);
        if (!contract_snippet.empty()) c += "\n" + contract_snippet;
        std::ofstream out(cfile, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create " + cfile.string());
        out.write(c.data(), static_cast<std::streamsize>(c.size()));
        if (!out) throw std::runtime_error("cannot write " + cfile.string());
        emitted_c = std::move(c);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat build: emit failed: %s\n", e.what());
        std::error_code ec;
        std::filesystem::remove(cfile, ec);
        return 1;
    }

    std::vector<std::string> vendored_objs;
    std::string vendored_digest, vendored_err;
    if (!collect_vendored_c_objects(set.files, compiler, zig, target, shared,
                                    vendored_objs, vendored_digest, vendored_err)) {
        std::fprintf(stderr, "brat build: %s\n", vendored_err.c_str());
        return 1;
    }

    std::vector<std::string> args = cc_parts;
    if (zig) args.push_back("cc");
    args.push_back(cfile.string());
    for (const std::string &vo : vendored_objs) args.push_back(vo);
    for (const std::string &fo : ffi_objects) {
        const std::string ext = std::filesystem::path(fo).extension().string();
        if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib")
            args.push_back(fo);
        else if (static_link && !static_explicit)
            static_link = false;
    }
    args.push_back("-o");
    args.push_back(temporary_output.string());
    if (target.size()) {
        args.push_back("-target");
        args.push_back(target);
    }
    if (static_link && !shared) args.push_back("-static");
    if (shared) {
        args.push_back("-shared");
        if (pkind != "windows") args.push_back("-fPIC");
    }
    // Pass `-fwrapv` so signed i64 overflow has defined two's-complement semantics.
    args.insert(args.end(), {"-std=gnu11", "-O2", "-fwrapv", "-I", engine.string()});
    if (debug_info) args.push_back("-g");
    if (!debug_info && !zig && target.empty() && link_flags.empty()) {
        maybe_append_crt_pch(compiler, engine, emitted_c,
                             static_link && !shared,
                             shared && pkind != "windows",
                             memory_limit, args);
    }
    if (pkind == "windows") args.push_back("-lws2_32");
    else args.push_back("-lm");
    if (memory_limit)
        args.push_back("-DBRAT_MEM_BYTES=" + std::to_string(memory_limit));
    for (const std::string &lib : libs) {
        args.push_back(format_link_lib(lib, pkind));
    }
    // `--link -lcudart`: the flags a library needs that a name cannot express
    // (`-L`, `-Wl,-rpath,...`, `-lcudart` beside a `.so` with no `lib` prefix).
    for (const std::string &flag : link_flags) args.push_back(flag);

     const int cc_timeout_ms = main_cc_timeout_ms();
     bool cc_timed_out = false;
     const int result = run_process(args, cc_timeout_ms, &cc_timed_out);
     if (result != 0) {
         if (cc_timed_out)
             std::fprintf(stderr, "brat build: C compiler timed out after %d ms\n", cc_timeout_ms);
         else
             std::fprintf(stderr, "brat build: C compiler exited with status %d\n", result);
         return 1;
     }
 #if defined(_WIN32)
     if (!MoveFileExW(temporary_output.wstring().c_str(), output_path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
         const DWORD error = GetLastError();
         std::fprintf(stderr, "brat build: cannot install output (Windows error %lu)\n",
                      static_cast<unsigned long>(error));
         return 1;
     }
 #else
     std::error_code ec;
     std::filesystem::rename(temporary_output, output_path, ec);
     if (ec) {
         std::fprintf(stderr, "brat build: cannot install output: %s\n", ec.message().c_str());
        return 1;
    }
 #endif
    std::printf("build ok %s\n", output.c_str());
    return 0;
}


// Compile once into the cache, then exec. Returns only on failure.
static int run_one(const char *arg0, const std::string &file, int opt,
                   uint64_t memory_limit, char **pass_argv, bool unsafe_ffi = false,
                   bool trace_lines = false,
                   const std::string &ffi_header = {},
                   const std::vector<std::string> &ffi_objects = {},
                   bool no_check = false,
                   bool debug_info = false) {
    ModuleSet set = load_modules(file);
    mangle_modules(set);
    if (no_check) {
        if (const char *why = no_check_forbidden_reason(set.files)) {
            std::fprintf(stderr, "brat run: refusing --no-check for %s: %s, which only the checker applies.\n",
                         file.c_str(), why);
            return 1;
        }
    } else {
        g_report = stderr;
        const int checker_errors = check_programs(set.files, true);
        g_report = stdout;
        if (checker_errors) {
            std::fflush(stdout);
            std::fprintf(stderr, "brat run: refusing to run after checker errors\n");
            return 1;
        }
    }
    if (native_ffi::ffi_gate(file, module_source_pairs(set.files), ffi_header, ffi_objects,
                             "", "brat run", unsafe_ffi) != 0) {
        std::fflush(stdout);
        return 1;
    }
    flatten(set, true);
    const auto engine = locate_lang_dir(arg0) / "engine";
    std::string header;
    if (!read_crt_bundle(engine, header)) {
        std::fprintf(stderr, "brat run: cannot read %s\n", (engine / "crt.h").string().c_str());
        return 1;
    }
    const auto libs = codegen::extern_libs(set.flat);
    const std::string root_path = set.files.empty() ? file : set.files[0].path;
    std::string source = codegen::emit_program(set.flat, dir_of(file), opt, nullptr, "",
                                               trace_lines, debug_info, root_path,
                                               &set.stmt_files, &set.fn_files);
    std::string compiler;
    if (const char *override = std::getenv("BRAT_CC"); override && *override) compiler = override;
    else if (const char *cc_env = std::getenv("CC"); cc_env && *cc_env) compiler = cc_env;
    else if (executable_on_path("cc")) compiler = "cc";
    else if (executable_on_path("gcc")) compiler = "gcc";
    else if (executable_on_path("clang")) compiler = "clang";
    else {
        std::fprintf(stderr, "brat run: needs a C compiler (set BRAT_CC, or precompile with brat build)\n");
        return 1;
    }
    const auto cc_parts = split_compiler_cmd(compiler);
    const auto compiler_resolved = cc_parts.empty() ? std::filesystem::path{} : executable_path(cc_parts[0].c_str());
    if (compiler_resolved.empty() || !std::filesystem::is_regular_file(compiler_resolved)) {
        std::fprintf(stderr, "brat run: needs a C compiler ('%s' not found; set BRAT_CC or use brat build)\n",
                     compiler.c_str());
        return 1;
    }
    const bool run_zig = std::filesystem::path(cc_parts[0]).filename().string().rfind("zig", 0) == 0 &&
                         !(cc_parts.size() >= 2 && cc_parts[1] == "cc");
    std::vector<std::string> vendored_objs;
    std::string vendored_digest, vendored_err;
    if (!collect_vendored_c_objects(set.files, compiler, run_zig, "", false,
                                    vendored_objs, vendored_digest, vendored_err)) {
        std::fprintf(stderr, "brat run: %s\n", vendored_err.c_str());
        return 1;
    }
    const std::string compiler_context = compiler_cache_context(compiler);
    [[maybe_unused]] bool reusable = !compiler_context.empty() && default_compile_environment(compiler);
    std::string key_material = "native-run-v2|" + compiler_context + "|" +
                               sha256_hex(source) + sha256_hex(header) +
                               "|opt=" + std::to_string(opt) +
                               "|dbg=" + std::to_string(debug_info ? 1 : 0) +
                               "|nc=" + std::to_string(no_check ? 1 : 0) +
                               "|mem=" + std::to_string(memory_limit) +
                               "|cobj=" + vendored_digest + "|libs=";
    for (const auto &lib : libs) {
        std::string bytes;
        key_material += lib + ",";
        if (read_file(lib, bytes)) key_material += sha256_hex(bytes);
        else reusable = false;
    }
    for (const auto &fo : ffi_objects) {
        std::string obytes;
        if (read_file(fo, obytes)) key_material += "|fo=" + sha256_hex(obytes);
        else reusable = false;
    }
    const std::set<std::filesystem::path> known_crt = {
        engine / "crt.h",
        engine / "crt" / "core.h",
        engine / "crt" / "ops_str.h",
        engine / "crt" / "net_tls.h",
        engine / "crt" / "sql_proc.h",
        engine / "crt" / "conc.h",
    };
    for (const auto &entry : std::filesystem::recursive_directory_iterator(engine)) {
        if (entry.is_symlink() ||
            (entry.path().extension() == ".h" && !known_crt.count(entry.path())))
            reusable = false;
    }
    const std::string key = sha256_hex(key_material).substr(0, 32);

    std::error_code ec;
    const auto dir = cache_root() / "run";
    std::filesystem::create_directories(dir, ec);
    const std::string pid_tag = std::to_string(static_cast<long long>(
#if defined(_WIN32)
        GetCurrentProcessId()
#else
        getpid()
#endif
    ));
    const auto binary = dir / key;
    const auto cfile = dir / (key + "." + pid_tag + ".c");

    const std::string binary_path = binary.string();
#if !defined(_WIN32)
    if (reusable && !std::filesystem::exists(dir / "crt.h", ec) &&
        std::filesystem::exists(binary, ec)) {
        pass_argv[0] = const_cast<char *>(binary_path.c_str());
        std::fflush(stdout);
        execv(pass_argv[0], pass_argv);
        // A cached binary that cannot exec is a broken entry: fall through and
        // rebuild it rather than failing.
    }
#endif

    {
        std::ofstream out(cfile, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "brat run: cannot write %s\n", cfile.string().c_str());
            return 1;
        }
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        out.flush();
        if (!out) {
            std::fprintf(stderr, "brat run: cannot write %s\n", cfile.string().c_str());
            return 1;
        }
    }
#if defined(_WIN32)
    const auto temporary = dir / (key + "." + pid_tag + ".tmp.exe");
#else
    const auto temporary = dir / (key + "." + pid_tag + ".tmp");
#endif
    const std::string host_pkind = platform_kind("");
    std::vector<std::string> args = cc_parts;
    if (run_zig) args.push_back("cc");
    args.push_back(cfile.string());
    for (const std::string &vo : vendored_objs) args.push_back(vo);
    for (const std::string &fo : ffi_objects) {
        const std::string ext = std::filesystem::path(fo).extension().string();
        if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib")
            args.push_back(fo);
    }
    args.insert(args.end(), {"-o", temporary.string(), "-std=gnu11", "-O2", "-fwrapv", "-I", engine.string()});
    if (debug_info) args.push_back("-g");
    if (!debug_info) {
        maybe_append_crt_pch(compiler, engine, source, false, false, memory_limit, args);
    }
    if (host_pkind == "windows") args.push_back("-lws2_32");
    else args.push_back("-lm");
    if (memory_limit) args.push_back("-DBRAT_MEM_BYTES=" + std::to_string(memory_limit));
    for (const std::string &lib : libs) {
        args.push_back(format_link_lib(lib, host_pkind));
    }
    const int cc_timeout_ms = main_cc_timeout_ms();
    bool cc_timed_out = false;
    const int built = run_process(args, cc_timeout_ms, &cc_timed_out);
    std::filesystem::remove(cfile, ec);
    if (built != 0) {
        std::filesystem::remove(temporary, ec);
        if (cc_timed_out)
            std::fprintf(stderr, "brat run: C compiler timed out after %d ms\n", cc_timeout_ms);
        else
            std::fprintf(stderr, "brat run: C compiler exited with status %d\n", built);
        return 1;
    }
#if defined(_WIN32)
    std::vector<std::string> run_args{temporary.string()};
    for (int i = 1; pass_argv && pass_argv[i]; ++i) run_args.emplace_back(pass_argv[i]);
    const int run_rc = run_process(run_args, 0);
    std::filesystem::remove(temporary, ec);
    return run_rc;
#else
    std::filesystem::rename(temporary, binary, ec);
    if (ec) {
        std::fprintf(stderr, "brat run: cannot install %s: %s\n", binary.string().c_str(),
                     ec.message().c_str());
        return 1;
    }
    pass_argv[0] = const_cast<char *>(binary_path.c_str());
    std::fflush(stdout);
    execv(pass_argv[0], pass_argv);
    std::perror("brat run: exec");
    return 127;
#endif
}

bool parse_memory(const std::string &input, uint64_t &value) {
    std::string text;
    for (unsigned char c : input)
        text.push_back(static_cast<char>(std::toupper(c)));
    if (text.empty()) return false;
    uint64_t multiplier = 1;
    const char suffix = text.back();
    if (suffix == 'K' || suffix == 'M' || suffix == 'G') {
        multiplier = suffix == 'K' ? 1024ULL : suffix == 'M' ? 1024ULL * 1024
                                                              : 1024ULL * 1024 * 1024;
        text.pop_back();
    }
    if (text.empty()) return false;
    uint64_t number = 0;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
        const unsigned digit = c - '0';
        if (number > (UINT64_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    if (!number || number > UINT64_MAX / multiplier) return false;
    value = number * multiplier;
    return true;
}

// Parse, ownership, and type diagnostics share one line format:
//
//     file:line: error: message at line N
//       | <the source line, right-stripped>
void print_parse_error(const std::string &path, const ParseError &e) {
    std::fprintf(stderr, "%s:%u: error: %s at line %u\n", path.c_str(), e.line, e.what(), e.line);
    std::string src;
    if (!read_file(path, src)) return;
    size_t pos = 0;
    for (uint32_t ln = 1; ln < e.line; ln++) {
        const size_t nl = src.find('\n', pos);
        if (nl == std::string::npos) return;
        pos = nl + 1;
    }
    const size_t end = src.find('\n', pos);
    std::string line = src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    std::fprintf(stderr, "  | %s\n", clip(line).c_str());
}

static void dump_one(const std::string &path, int &bad) {
    std::string src;
    if (!read_file(path, src)) {
        std::fprintf(stderr, "brat-cxx: cannot read %s\n", path.c_str());
        bad++;
        return;
    }
    try {
        ParseOut out = parse(src);
        for (const auto &[line, text] : out.stray) {
            std::fprintf(stderr, "%s:%u: warning: `{%s}` is not an expression\n", path.c_str(), line,
                         text.c_str());
        }
        std::string text = dump(out.body);
        text.push_back('\n');
        std::fwrite(text.data(), 1, text.size(), stdout);
    } catch (const ParseError &e) {
        print_parse_error(path, e);
        bad++;
    }
}

static void lex_one(const std::string &path, int &bad) {
    std::string src;
    if (!read_file(path, src)) {
        std::fprintf(stderr, "brat-cxx: cannot read %s\n", path.c_str());
        bad++;
        return;
    }
    try {
        for (const Tok &t : lex(src)) {
            std::printf("%d:%zu@%u %.*s\n", static_cast<int>(t.kind), t.text.size(), t.line,
                        static_cast<int>(t.text.size()), t.text.data());
        }
    } catch (const ParseError &e) {
        print_parse_error(path, e);
        bad++;
    }
}

// `cc` takes flags; the other commands take only files, so a flag there is a
// file that does not exist and is reported as one.
struct CcFlags {
    int opt = 0;
    bool why = false;
    bool no_check = false;
    bool trace_lines = false;
    bool debug_lines = false;
    std::vector<std::string> files;
};

static bool parse_cc(int argc, char **argv, CcFlags &f) {
    bool level_seen = false, level0 = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "-O0") {
            f.opt = 0;
            level_seen = level0 = true;
        } else if (a == "-O1") {
            f.opt = 1;
            level_seen = true;
            level0 = false;
        } else if (a == "-O2") {
            f.opt = 2;
            level_seen = true;
            level0 = false;
        } else if (a == "--why-opt") {
            f.why = true;
            if (!level_seen || level0) f.opt = 1;
        } else if (a == "--no-check") {
            f.no_check = true;
        } else if (a == "--trace-lines") {
            f.trace_lines = true;
        } else if (a == "-g" || a == "--debug") {
            f.debug_lines = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "brat-cxx: unknown flag %s\n", a.c_str());
            return false;
        } else {
            f.files.push_back(a);
        }
    }
    // `--why-opt` is the report of the pass, so asking for it at -O0 asks for
    // the proof of something that was not done.
    if (f.why && level_seen && level0) {
        std::fprintf(stderr, "brat-cxx: --why-opt needs -O1, not -O0\n");
        return false;
    }
    if (f.files.empty()) {
        std::fprintf(stderr, "brat-cxx: cc needs a file\n");
        return false;
    }
    return true;
}

// A flag the subcommand does not know is a usage error, never a file name: the
// answer to `brat-cxx flat --json prog.brat` used to be `cannot read --json`.
static bool reject_flags(int argc, char **argv, const char *cmd, int first = 2) {
    for (int i = first; i < argc; i++) {
        const std::string a = argv[i];
        if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "brat-cxx %s: unknown flag %s\n", cmd, a.c_str());
            return false;
        }
    }
    return true;
}

// The gate `brat emit-c` already had: the ownership pass over one file with the
// signatures of its siblings, printing the errors to `g_report`. Emitted C skips
// every check `brat build` applies, so a program the checker rejects is refused
// before any code is written -- which is what makes "same diagnostics" true for
// `cc` and not only for `check`.
std::size_t ownership_errors(const std::vector<SourceFile> &files, std::size_t i) {
    BorrowOut out = check_sibling_borrows(files, i);
    report(files[i].path, files[i].src, out.errs, "error");
    return out.errs.size();
}

std::size_t type_errors(const std::vector<SourceFile> &files, std::size_t i,
                        bool arena_only) {
    std::vector<TypeErr> errs = check_sibling_types(files, i);
    if (arena_only) {
        std::vector<TypeErr> filtered;
        for (const auto &e : errs) {
            if (e.msg.rfind("arena: ", 0) == 0) filtered.push_back(e);
        }
        errs = std::move(filtered);
    }
    report(files[i].path, files[i].src, errs);
    return errs.size();
}

void print_cli_usage(FILE *out) {
    std::fprintf(out,
                 "usage: brat --version\n"
                 "       brat run|build|dist|check|deps|api|fmt|test|doc|new|emit-c|ffi|bindgen|pkg|explain|lsp|mcp|bundle ...\n"
                 "       brat run FILE [-- args...] [-g|--debug] [-O0|-O1|-O2] [--mem N]\n"
                 "       brat deps FILE [--json|--dot] [-o OUT]\n"
                 "       brat api FILE [--json]\n"
                 "       brat api --diff OLD.bri NEW.bri [--json]\n"
                 "       brat build FILE -o OUT [-g|--debug] [--link FLAG] [--mem N] [--header H]\n"
                 "       brat dist FILE [-o DIR] [--target TRIPLE[,TRIPLE...]] [--shared]\n"
                 "       brat check [--json] FILE...\n"
                 "       brat fmt [--check] [--stdout] FILE...\n"
                 "       brat test [-j N] [--filter SUBSTR] FILE_OR_DIR...\n"
                 "       brat doc FILE_OR_DIR [--json] [-o OUT]\n"
                 "       brat new NAME [--kind service|tool]\n"
                 "       brat emit-c FILE [-o OUT] [-g|--debug] [--bundle] [--no-check] [--trace-lines] [-O0|-O1|-O2]\n"
                 "       brat ffi FILE --header H --abi-matrix [--json]\n"
                 "       brat bindgen <HEADER.h | LIB.rs> [-o OUT] [--lib NAME] [--preprocess] [--header-only] [-I DIR] [-D DEF] [--inline-arrays] [--lang LANG] [--target TRIPLE]\n"
                 "       brat pkg add|publish|update|install|verify|list|remove ...\n"
                 "       brat explain [CODE] [--json]\n"
                 "       brat bundle OUTPUT_DIR\n"
                 "       brat lsp\n"
                 "       brat mcp\n");
}


int brat_cli_main(int argc, char **argv) {
    if (argc >= 1 && argv[0]) {
        const auto std_dir = locate_lang_dir(argv[0]) / "std";
        set_stdlib_root(std_dir.string());
    }
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" ||
                      std::string(argv[1]) == "help")) {
        print_cli_usage(stdout);
        return 0;
    }
    if (argc >= 2 && (std::string(argv[1]) == "-V" || std::string(argv[1]) == "--version" ||
                      std::string(argv[1]) == "version")) {
        if (argc != 2) {
            print_cli_usage();
            return 2;
        }
        std::fprintf(stdout, "brat %s\n", kBratVersion);
        return 0;
    }
    if (argc >= 2) {
        const std::string c1 = argv[1];
        if (c1 == "bundle") {
            if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
                std::fprintf(stdout, "usage: brat bundle OUTPUT_DIR\n");
                return 0;
            }
            if (argc != 3) {
                std::fprintf(stderr, "usage: brat bundle OUTPUT_DIR\n");
                return 2;
            }
            return bundle_driver(argv[0], argv[2]);
        }
        if (c1 == "lsp") {
            if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
                std::fprintf(stdout, "usage: brat lsp\n");
                return 0;
            }
            if (argc != 2) {
                std::fprintf(stderr, "usage: brat lsp\n");
                return 2;
            }
            return run_lsp();
        }
        if (c1 == "mcp") return cmd_mcp(argc, argv);
        if (c1 == "dist") return cmd_dist(argc, argv);
        if (c1 == "ffi") return cmd_ffi_native(argc, argv);
        if (c1 == "bindgen") {
            return native_ffi::cmd_bindgen(argc - 2, argv + 2);
        }
        if (c1 == "fmt") return cmd_fmt(argc, argv);
        if (c1 == "pkg") return cmd_pkg(argc, argv);
        if (c1 == "deps") return cmd_deps(argc, argv);
        if (c1 == "api") return cmd_api(argc, argv);
        if (c1 == "explain") return cmd_explain(argc, argv);
        if (c1 == "new") return cmd_new(argc, argv);
        if (c1 == "doc") return cmd_doc(argc, argv);
        if (c1 == "test") return cmd_test(argc, argv);
        if (c1 == "emit-c") return cmd_emit_c(argc, argv);
    }
    if (argc < 2) {
        print_cli_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const bool native = cmd == "ast" || cmd == "lex" || cmd == "check" || cmd == "flat" ||
                        cmd == "cc" || cmd == "libs" || cmd == "build" || cmd == "run";
    if (!native) {
        std::fprintf(stderr, "brat: unknown command '%s'\n", cmd.c_str());
        print_cli_usage();
        return 2;
    }
    if (argc < 3) {
        print_cli_usage();
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) break;
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_cli_usage(stdout);
            return 0;
        }
    }
    if (cmd == "run") {
        std::string file, ffi_header;
        std::vector<std::string> ffi_objects;
        uint64_t memory_limit = 0;
        int opt = 0;
        bool unsafe_ffi = false;
        bool no_check = false;
        const char *tev = std::getenv("BRAT_TRACE");
        bool trace_lines = (tev && tev[0] && tev[0] != '0');
        bool debug_info = false;
        std::vector<std::string> program_args;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--") {
                for (int j = i + 1; j < argc; ++j) program_args.push_back(argv[j]);
                break;
            }
            if (arg == "-O0") opt = 0;
            else if (arg == "-O1") opt = 1;
            else if (arg == "-O2") opt = 2;
            else if (arg == "-g" || arg == "--debug") debug_info = true;
            else if (arg == "--unsafe-ffi") unsafe_ffi = true;
            else if (arg == "--header" && i + 1 < argc) ffi_header = argv[++i];
            else if (arg.rfind("--header=", 0) == 0) ffi_header = arg.substr(9);
            else if (arg == "--object" && i + 1 < argc) ffi_objects.push_back(argv[++i]);
            else if (arg.rfind("--object=", 0) == 0) ffi_objects.push_back(arg.substr(9));
            else if (arg == "--trace-lines") trace_lines = true;
            else if (arg == "--no-check") no_check = true;
            else if (arg == "--mem" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (!parse_memory(value, memory_limit)) {
                    std::fprintf(stderr, "brat run: bad --mem '%s'\n", value.c_str());
                    return 2;
                }
            } else if (arg.rfind("--mem=", 0) == 0) {
                const std::string value = arg.substr(6);
                if (!parse_memory(value, memory_limit)) {
                    std::fprintf(stderr, "brat run: bad --mem '%s'\n", value.c_str());
                    return 2;
                }
            } else if (!arg.empty() && arg[0] == '-') {
                std::fprintf(stderr, "brat run: unknown flag %s\n", arg.c_str());
                return 2;
            } else if (file.empty()) file = arg;
            else program_args.push_back(arg);
        }
        if (file.empty()) {
            std::fprintf(stderr, "brat run: needs one source file\n");
            return 2;
        }
        std::vector<std::string> keep;
        keep.push_back(file);
        for (const auto &a : program_args) keep.push_back(a);
        std::vector<char *> pass;
        for (auto &k : keep) pass.push_back(const_cast<char *>(k.c_str()));
        pass.push_back(nullptr);
        try {
            return run_one(argv[0], file, opt, memory_limit, pass.data(), unsafe_ffi,
                           trace_lines, ffi_header, ffi_objects, no_check, debug_info);
        } catch (const ParseError &e) {
            print_parse_error(file, e);
            return 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "brat run: %s\n", e.what());
            return 1;
        }
    }
    if (cmd == "build") {
        std::string file, output, target, ffi_header;
        std::vector<std::string> ffi_objects;
#if defined(__linux__)
        bool static_link = true, static_explicit = false, shared = false;
#else
        bool static_link = false, static_explicit = false, shared = false;
#endif
        uint64_t memory_limit = 0;
        int opt = 0;
        bool unsafe_ffi = false;
        bool no_check = false;
        const char *btev = std::getenv("BRAT_TRACE");
        bool trace_lines = (btev && btev[0] && btev[0] != '0');
        bool debug_info = false;
        std::vector<std::string> link_flags;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-o" && i + 1 < argc) output = argv[++i];
            else if (arg == "--target" && i + 1 < argc) target = argv[++i];
            else if (arg.rfind("--target=", 0) == 0) target = arg.substr(9);
            else if (arg == "--mem" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (!parse_memory(value, memory_limit)) {
                    std::fprintf(stderr, "brat build: bad --mem '%s' "
                                         "(want e.g. 16777216, 512K, 16M, 1G)\n",
                                 value.c_str());
                    return 2;
                }
            } else if (arg.rfind("--mem=", 0) == 0) {
                const std::string value = arg.substr(6);
                if (!parse_memory(value, memory_limit)) {
                    std::fprintf(stderr, "brat build: bad --mem '%s' "
                                         "(want e.g. 16777216, 512K, 16M, 1G)\n",
                                 value.c_str());
                    return 2;
                }
            } else if (arg == "--static") {
                static_link = true;
                static_explicit = true;
            } else if (arg == "--dynamic") {
                static_link = false;
                shared = false;
            } else if (arg == "--shared") {
                static_link = false;
                shared = true;
            } else if (arg == "--link" && i + 1 < argc) link_flags.push_back(argv[++i]);
            else if (arg.rfind("--link=", 0) == 0) link_flags.push_back(arg.substr(7));
            else if (arg == "-O0") opt = 0;
            else if (arg == "-O1") opt = 1;
            else if (arg == "-O2") opt = 2;
            else if (arg == "-g" || arg == "--debug") debug_info = true;
            else if (arg == "--unsafe-ffi") unsafe_ffi = true;
            else if (arg == "--header" && i + 1 < argc) ffi_header = argv[++i];
            else if (arg.rfind("--header=", 0) == 0) ffi_header = arg.substr(9);
            else if (arg == "--object" && i + 1 < argc) ffi_objects.push_back(argv[++i]);
            else if (arg.rfind("--object=", 0) == 0) ffi_objects.push_back(arg.substr(9));
            else if (arg == "--trace-lines") trace_lines = true;
            else if (arg == "--no-check") no_check = true;
            else if (!arg.empty() && arg[0] == '-') {
                std::fprintf(stderr, "brat build: unknown flag %s\n", arg.c_str());
                return 2;
            } else if (file.empty()) file = arg;
            else {
                std::fprintf(stderr, "brat build: expected exactly one source file\n");
                return 2;
            }
        }
        if (file.empty()) {
            std::fprintf(stderr, "brat build: needs one source file\n");
            return 2;
        }
        if (output.empty()) {
            std::filesystem::path path(file);
            const std::string pk = platform_kind(target);
            const char *sext = pk == "macos" ? ".dylib" : (pk == "windows" ? ".dll" : ".so");
            path.replace_extension(shared ? sext : ".exe");
            output = path.string();
        }
        try {
            return build_one(argv[0], file, output, target, static_link,
                             static_explicit, shared, opt, memory_limit, link_flags,
                             unsafe_ffi, trace_lines, ffi_header, ffi_objects, no_check,
                             debug_info);
        } catch (const ParseError &e) {
            print_parse_error(file, e);
            return 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "brat build: %s\n", e.what());
            return 1;
        }
    }
    int bad = 0;
    if (cmd == "ast" || cmd == "lex" || cmd == "flat" || cmd == "libs") {
        if (!reject_flags(argc, argv, cmd.c_str())) return 2;
    }
    if (cmd == "ast" || cmd == "lex") {
        for (int i = 2; i < argc; i++) {
            if (cmd == "ast") dump_one(argv[i], bad); else lex_one(argv[i], bad);
        }
        return bad ? 1 : 0;
    }
    if (cmd == "flat") {
        for (int i = 2; i < argc; i++) {
            try {
                ModuleSet set = load_modules(argv[i]);
                flatten(set);
                std::string text = dump(set.flat);
                text.push_back('\n');
                std::fwrite(text.data(), 1, text.size(), stdout);
            } catch (const ParseError &e) {
                print_parse_error(argv[i], e);
                bad++;
            } catch (const std::exception &e) {
                std::printf("%s: load error %s\n", argv[i], e.what());
                return 2;
            }
        }
        return bad ? 1 : 0;
    }

    if (cmd == "cc" || cmd == "libs") {
        CcFlags flags;
        std::vector<std::string> files;
        if (cmd == "cc") {
            if (!parse_cc(argc, argv, flags)) return 2;
            files = flags.files;
        } else {
            for (int i = 2; i < argc; i++) files.push_back(argv[i]);
        }
        for (const std::string &file : files) {
            try {
                ModuleSet set = load_modules(file);
                flatten(set, cmd == "cc");
                if (cmd == "libs") {
                    for (const auto &lib : codegen::extern_libs(set.flat))
                        std::printf("%s\n", lib.c_str());
                } else {
                    // A fresh load: `flatten` splices module bodies into `set.flat`
                    // and leaves a `pub` statement's first child null, so the gate
                    // runs on the tree the parser produced, exactly as `check` does
                    // (it loads the file a second time for the same reason).
                    std::size_t nerr = 0;
                    if (flags.no_check) {
                        ModuleSet fresh = load_modules(file);
                        bool arena_found = false;
                        for (const auto &sf : fresh.files) {
                            if (has_arena_block_tokens(sf.src)) { arena_found = true; break; }
                        }
                        if (arena_found) {
                            std::fprintf(stderr,
                                         "brat-cxx cc: refusing --no-check for %s: arena blocks depend on the "
                                         "escape rules, which only the checker applies.\n",
                                         file.c_str());
                            bad++;
                            continue;
                        }
                    } else {
                        ModuleSet fresh = load_modules(file);
                        mangle_modules(fresh);
                        g_report = stderr;
                        for (std::size_t fi = 0; fi < fresh.files.size(); ++fi) {
                            nerr += ownership_errors(fresh.files, fi);
                            nerr += type_errors(fresh.files, fi, /*arena_only=*/true);
                        }
                        g_report = stdout;
                    }
                    if (nerr != 0) {
                        std::fprintf(stderr,
                                     "brat-cxx cc: refusing to emit %s: the ownership "
                                     "checker reported errors above. Emitted C skips every "
                                     "check `brat build` applies.\nFix it, or pass "
                                     "--no-check to override deliberately.\n",
                                     file.c_str());
                        bad++;
                        continue;
                    }
                    std::vector<ir::Proof> proofs;
                    const std::string root_path = set.files.empty() ? file : set.files[0].path;
                    std::string text =
                        codegen::emit_program(set.flat, dir_of(file), flags.opt, &proofs, "",
                                              flags.trace_lines, flags.debug_lines, root_path,
                                              &set.stmt_files, &set.fn_files);
                    if (flags.no_check)
                        text = "/* BRAT: UNVERIFIED -- ownership not checked */\n" + text;
                    std::fwrite(text.data(), 1, text.size(), stdout);
                    // stderr, so stdout stays the C and a diff of two runs at the
                    // same level still compares the emitted program alone.
                    if (flags.why)
                        for (const auto &p : proofs) std::fprintf(stderr, "%s", p.text.c_str());
                }
            } catch (const ParseError &e) {
                print_parse_error(file, e);
                bad++;
            } catch (const std::exception &e) {
                std::fprintf(stderr, "%s: %s\n", file.c_str(), e.what());
                bad++;
            }
        }
        return bad ? 1 : 0;
    }

    std::vector<std::string> input_files;
    if (cmd == "check") {
        bool want_json = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--json") { want_json = true; continue; }
            if (arg == "--") continue;
            if (!arg.empty() && arg[0] == '-' && arg != "-") {
                std::fprintf(stderr, "brat-cxx check: unknown flag %s\n", arg.c_str());
                return 2;
            }
            input_files.push_back(arg);
        }
        if (want_json) return cmd_check_json(input_files);
        if (input_files.empty()) {
            print_cli_usage();
            return 2;
        }
    } else {
        for (int i = 2; i < argc; ++i) input_files.push_back(argv[i]);
    }

    std::vector<SourceFile> progs;
    for (const std::string &in_file : input_files) {
        try {
            ModuleSet loaded = load_modules(in_file);
            mangle_modules(loaded);
            for (auto &f : loaded.files) progs.push_back(std::move(f));
        } catch (const ParseError &e) {
            print_parse_error(in_file, e);
            bad++;
        } catch (const std::exception &e) {
            std::printf("%s: load error %s\n", in_file.c_str(), e.what());
            return 2;
        }
    }
    if (bad) return 1;

    // `brat check` runs the ownership pass over every file first, then the
    // type pass; both print their own bill per file.
    for (size_t i = 0; i < progs.size(); i++) {
        std::map<std::string, std::vector<ParamMode>> extra;
        std::set<std::string> extra_consts;
        std::set<std::string> extra_scalar;
        std::map<std::string, std::vector<size_t>> extra_borrows;
        std::set<std::string> extra_owned;
        std::map<std::string, std::map<std::string, std::string>> extra_sfields;
        std::map<std::string, std::string> extra_frets;
        std::set<std::string> module_heads = imported_module_heads(progs[i]);
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            FnTable ps = pub_sigs(progs[j].body, exported_fn_names(progs[j]));
            for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
            module_heads.insert(ps.heads.begin(), ps.heads.end());
            extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
            extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
            for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
            extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
            for (auto &kv : ps.struct_fields) extra_sfields[kv.first] = kv.second;
            for (auto &kv : ps.fn_rets) extra_frets[kv.first] = kv.second;
        }
        BorrowOut out = borrow_check(progs[i].body, extra, progs[i].stray, module_heads,
                                     extra_consts, extra_scalar, extra_borrows, extra_owned,
                                     extra_sfields, extra_frets);
        std::printf("%s: %d error(s), %d warning(s)\n", progs[i].path.c_str(),
                    static_cast<int>(out.errs.size()), static_cast<int>(out.warns.size()));
        report(progs[i].path, progs[i].src, out.errs, "error");
        report(progs[i].path, progs[i].src, out.warns, "warning");
        if (!out.errs.empty()) bad++;
    }

    for (size_t i = 0; i < progs.size(); i++) {
        Sigs merged;
        std::map<std::string, Ty> merged_consts;
        std::map<std::string, std::map<std::string, Ty>> merged_structs;
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            collect_exports(progs[j].body, merged_consts, merged_structs);
            collect_sigs(progs[j].body, merged);
        }
        const SourceFile &f = progs[i];
        std::vector<TypeErr> errs = type_check(f.body, merged, merged_consts, merged_structs);
        std::printf("%s: %d type error(s)\n", f.path.c_str(),
                    static_cast<int>(errs.size()));
        report(f.path, f.src, errs);
        if (!errs.empty()) bad++;
    }
    return bad ? 1 : 0;
}


int main(int argc, char **argv) {
    return brat_cli_main(argc, argv);
}
