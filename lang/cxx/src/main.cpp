// `brat-cxx` — the front end and the native editor server.
// the toolchain it needs is the C compiler `brat build` already needs.
//
//   brat-cxx ast FILE      the s-expression of the tree, the diff contract
//   brat-cxx lex FILE      one token per line, for debugging the scanner
//   brat-cxx check FILE    ownership and types on a module set; later files
//                          feed signatures to the earlier ones, like `brat check`
//   brat-cxx flat FILE     the mangled program the emitter sees: imports
//                          spliced in place, names qualified, `pub use` gone
//   brat-cxx cc FILE       the C for FILE from the native emitter
//   brat-cxx cc -O1 FILE   the same C with the checks ir.cpp proves dead removed;
// why-opt prints the proof of each removed check
//   brat-cxx libs FILE     the libraries its extern blocks ask to link, one per line
//   brat-cxx build FILE    check, emit and link without the Python CLI
//   brat-cxx lsp           the native language server over stdio
//   brat-cxx mcp           the native Model Context Protocol server over stdio
//
// `brat-cxx` is also the `brat` the user runs. Commands it does not implement
// are handed to the Python CLI; `run`, `build`, `check`, `deps`, and `api` use
// the native frontend and never start an interpreter.

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

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ast.h"
#include "borrow.h"
#include "check.h"
#include "cli_tools.h"
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

static bool read_file(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

static std::string dir_of(const std::string &path) {
    return std::filesystem::absolute(path).parent_path().lexically_normal().string();
}

static std::vector<std::string> split_lines(const std::string &src) {
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

static std::string rstrip(const std::string &s) {
    size_t n = s.size();
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return s.substr(0, n);
}

// The same shape as diag.to_human: `file:line: severity: message` over the
// source line, then the note the message points at. A diagnostic whose line is
// unknown prints as `file: severity: message`.
// `check` prints its bill on stdout, where the user asked for it. `cc` must not:
// stdout is the C, so a refusal has to go to stderr and leave the pipe empty.
static FILE *g_report = stdout;

// A snippet is an aid, not a dump. Generated programs carry lines with tens of
// thousands of terms, and echoing one verbatim buries the message it belongs to
// (and can be megabytes). Both front ends cut at the same width (`SNIPPET_MAX`
// in `diag.py`) so the rendered diagnostic stays byte-identical.
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
    // `SNIPPET_MAX` counts characters in the Python front end. Counting
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

static void emit(const std::string &path, const std::vector<std::string> &lines,
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
            emit(path, lines, "error", e.line, e.msg);
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
            emit(path, lines, severity, d.line, d.msg);
        } else {
            std::fprintf(g_report, "%s: %s: %s\n", path.c_str(), severity, d.msg.c_str());
        }
        if (d.note) {
            if (d.note->line > 0) {
                emit(path, lines, "note", d.note->line, d.note->msg);
            } else {
                std::fprintf(g_report, "%s: note: %s\n", path.c_str(),
                             d.note->msg.c_str());
            }
        }
    }
}

static std::set<std::string> imported_module_heads(const SourceFile &file) {
    std::set<std::string> heads;
    for (const auto &[name, target] : file.imports) {
        (void)target;
        heads.insert(name.substr(0, name.find('.')));
        size_t tail = name.rfind('.');
        heads.insert(name.substr(tail == std::string::npos ? 0 : tail + 1));
    }
    return heads;
}

static int run_process(const std::vector<std::string> &args, int timeout_ms = 0,
                       bool *timed_out = nullptr);

// The Python CLI, exec'd in place so a fallback command costs exactly what it
// cost before, with no wrapper process left behind. `BRAT_PY` names the
// interpreter, `BRAT_ENGINE` the tree; the lookup walks up from the executable
// so a `bundle/` directory and a checkout both work.
static int python_cli(int argc, char **argv) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) exe = fs::absolute(argv[0], ec);
    fs::path dir = exe.parent_path();
    fs::path engine;
    for (int i = 0; i < 4 && !dir.empty(); i++) {
        if (fs::exists(dir / "engine" / "cli.py", ec)) { engine = dir / "engine" / "cli.py"; break; }
        if (fs::exists(dir / "lang" / "engine" / "cli.py", ec)) {
            engine = dir / "lang" / "engine" / "cli.py";
            break;
        }
        dir = dir.parent_path();
    }
    if (engine.empty()) {
        std::fprintf(stderr,
                     "brat: `%s` is implemented by the Python CLI, which is not next to this "
                     "binary (set BRAT_ENGINE to lang/engine/cli.py)\n",
                     argv[1]);
        return 2;
    }
    const char *env_py = std::getenv("BRAT_PY");
    std::string python = env_py && *env_py ? env_py : "python3";
    std::vector<std::string> args{python, engine.string()};
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    std::vector<char *> pass;
    for (auto &a : args) pass.push_back(const_cast<char *>(a.c_str()));
    pass.push_back(nullptr);
#if defined(_WIN32)
    return run_process(args, 0);
#else
    execvp(pass[0], pass.data());
    std::fprintf(stderr, "brat: cannot run %s: %s\n", python.c_str(), std::strerror(errno));
    return 2;
#endif
}

static std::set<std::string> exported_fn_names(const SourceFile &sf) {
    std::set<std::string> out;
    for (const auto &n : sf.pub) {
        auto it = sf.functions.find(n);
        if (it != sf.functions.end()) out.insert(it->second);
    }
    return out;
}

static int check_programs(const std::vector<SourceFile> &progs, bool quiet = false) {
    int bad = 0;
    for (size_t i = 0; i < progs.size(); i++) {
        std::map<std::string, std::vector<ParamMode>> extra;
        std::set<std::string> extra_consts;
        std::set<std::string> extra_scalar;
        std::map<std::string, std::vector<size_t>> extra_borrows;
        std::set<std::string> extra_owned;
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            FnTable ps = pub_sigs(progs[j].body, exported_fn_names(progs[j]));
            for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
            extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
            extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
            for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
            extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
        }
        std::set<std::string> module_heads = imported_module_heads(progs[i]);
        BorrowOut out = borrow_check(progs[i].body, extra, progs[i].stray, module_heads,
                                     extra_consts, extra_scalar, extra_borrows, extra_owned);
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
        if (!quiet || !errs.empty())
            std::fprintf(g_report, "%s: %d type error(s)\n", f.path.c_str(),
                         static_cast<int>(errs.size()));
        report(f.path, f.src, errs);
        if (!errs.empty()) bad++;
    }
    return bad;
}

static std::filesystem::path executable_path(const char *arg0) {
    std::filesystem::path candidate(arg0);
    if (candidate.has_parent_path()) {
        std::error_code ec;
        auto resolved = std::filesystem::canonical(candidate, ec);
        return ec ? std::filesystem::absolute(candidate) : resolved;
    }
    const char *env_path = std::getenv("PATH");
    if (env_path) {
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
            auto found = std::filesystem::path(paths.substr(start, end - start)) / candidate;
            std::error_code ec;
            if (std::filesystem::is_regular_file(found, ec))
                return std::filesystem::canonical(found, ec);
            if (end == paths.size()) break;
            start = end + 1;
        }
    }
    return std::filesystem::absolute(candidate);
}

static bool executable_on_path(const std::string &name) {
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
        std::error_code ec;
        if (std::filesystem::is_regular_file(
                std::filesystem::path(paths.substr(start, end - start)) / name, ec)) return true;
        if (end == paths.size()) break;
        start = end + 1;
    }
    return false;
}


//
// `brat-cxx run` is the product path without Python: emit, compile once, keep
// the binary keyed by everything that can change its meaning, then exec it.
// The key is a SHA-256 over the emitted C, the runtime header, the compiler and
// the flags, so a cache hit cannot be a stale program -- and a miss costs the
// same compile `brat build` pays, once.

static void sha256_block(const unsigned char *block, uint32_t h[8]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
        const uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static std::string sha256_hex(const std::string &data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string padded = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56) padded.push_back('\0');
    for (int i = 7; i >= 0; i--)
        padded.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    for (size_t at = 0; at < padded.size(); at += 64)
        sha256_block(reinterpret_cast<const unsigned char *>(padded.data() + at), h);
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

struct Sha256Stream {
    std::array<uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block{};
    size_t used = 0;
    uint64_t bytes = 0;

    bool update(const unsigned char *data, size_t size) {
        if (size > std::numeric_limits<uint64_t>::max() / 8 - bytes) return false;
        bytes += size;
        while (size) {
            const size_t take = std::min(size, block.size() - used);
            std::memcpy(block.data() + used, data, take);
            used += take;
            data += take;
            size -= take;
            if (used == block.size()) {
                sha256_block(block.data(), h.data());
                used = 0;
            }
        }
        return true;
    }

    std::string finish() {
        const uint64_t bits = bytes * 8;
        block[used++] = 0x80;
        if (used > 56) {
            std::fill(block.begin() + static_cast<ptrdiff_t>(used), block.end(), 0);
            sha256_block(block.data(), h.data());
            used = 0;
        }
        std::fill(block.begin() + static_cast<ptrdiff_t>(used), block.begin() + 56, 0);
        for (int i = 7; i >= 0; --i)
            block[56 + (7 - i)] = static_cast<unsigned char>((bits >> (i * 8)) & 0xff);
        sha256_block(block.data(), h.data());
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

static bool sha256_file(const std::filesystem::path &path, std::string &digest,
                        uint64_t &size, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    Sha256Stream hash;
    std::array<unsigned char, 64 * 1024> buffer{};
    size = 0;
    while (in) {
        in.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const std::streamsize count = in.gcount();
        if (count > 0) {
            const size_t n = static_cast<size_t>(count);
            if (n > std::numeric_limits<uint64_t>::max() - size ||
                !hash.update(buffer.data(), n)) {
                error = "file is too large to hash";
                return false;
            }
            size += n;
        }
    }
    if (!in.eof()) {
        error = "failed while reading " + path.string();
        return false;
    }
    digest = hash.finish();
    return true;
}

static std::filesystem::path cache_root() {
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

static int main_cc_timeout_ms() {
    if (const char *ev = std::getenv("BRAT_CC_TIMEOUT_MS"); ev && *ev) {
        char *end = nullptr;
        const long long v = std::strtoll(ev, &end, 10);
        if (end != ev && *end == '\0' && v > 0 && v <= 3600000LL) return static_cast<int>(v);
    }
    return 120000;
}

static int run_process(const std::vector<std::string> &args, int timeout_ms, bool *timed_out) {
    if (timed_out) *timed_out = false;
    if (args.empty()) return 127;
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const std::string &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
#if defined(_WIN32)
    if (timeout_ms <= 0) {
        const intptr_t result = _spawnvp(_P_WAIT, argv[0], argv.data());
        return result < 0 ? 127 : static_cast<int>(result);
    }
    auto quote_w = [](std::string_view arg) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, arg.data(), static_cast<int>(arg.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(0, wn)), L'\0');
        if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, arg.data(), static_cast<int>(arg.size()), w.data(), wn);
        if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) return w;
        std::wstring out = L"\"";
        for (size_t i = 0; i < w.size(); ++i) {
            size_t bs = 0;
            while (i < w.size() && w[i] == L'\\') { ++bs; ++i; }
            if (i == w.size()) { out.append(bs * 2, L'\\'); break; }
            if (w[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out.push_back(L'"'); }
            else { out.append(bs, L'\\'); out.push_back(w[i]); }
        }
        out.push_back(L'"');
        return out;
    };
    std::wstring cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline.push_back(L' ');
        cmdline += quote_w(args[i]);
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        if (job) CloseHandle(job);
        return 127;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    const DWORD wrc = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (wrc == WAIT_TIMEOUT) {
        if (timed_out) *timed_out = true;
        if (job) TerminateJobObject(job, 124);
        else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        return 124;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return static_cast<int>(code);
#else
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (timeout_ms > 0) setpgid(0, 0);
        execvp(argv[0], argv.data());
        std::perror("brat-cxx: exec compiler");
        _exit(127);
    }
    if (timeout_ms <= 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) return 127;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    setpgid(pid, pid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        const pid_t wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (wr < 0 && errno != EINTR) return 127;
        if (std::chrono::steady_clock::now() >= deadline) {
            if (timed_out) *timed_out = true;
            if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return 124;
        }
        struct timespec ts {0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
#endif
}

struct BuildStage {
    std::filesystem::path path;

    explicit BuildStage(const std::filesystem::path &output) {
        std::filesystem::path parent = output.parent_path();
        if (parent.empty()) parent = ".";
#if defined(_WIN32)
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path = parent / (".brat-build-" + std::to_string(_getpid()) + "-" +
                             std::to_string(attempt));
            if (CreateDirectoryW(path.wstring().c_str(), nullptr)) return;
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "cannot create build staging directory");
        }
        throw std::runtime_error("cannot reserve build staging directory");
#else
        std::string pattern = (parent / ".brat-build-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        char *created = mkdtemp(name.data());
        if (!created)
            throw std::system_error(errno, std::generic_category(),
                                    "cannot create build staging directory");
        path = created;
#endif
    }

    ~BuildStage() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        if (ec)
            std::fprintf(stderr, "brat-cxx build: cannot clean staging directory %s: %s\n",
                         path.string().c_str(), ec.message().c_str());
    }
};

static int bundle_driver(const char *arg0, const std::filesystem::path &output) {
    try {
        const auto executable = executable_path(arg0);
        const auto lang = executable.parent_path().parent_path().parent_path();
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
        std::filesystem::rename(root, output);
        std::printf("bundle ok %s\n", output.string().c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat-cxx bundle: %s\n", e.what());
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
        if (s.els) {
            if (expr_has_plugin_call(s.els->e1.get()) || stmts_have_plugin_call(s.els->body))
                return true;
        }
        for (const auto &f : s.fns)
            if (stmts_have_plugin_call(f.body)) return true;
    }
    return false;
}

static bool has_extern_decls(const std::vector<StmtP> &body) {
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

static bool is_safe_pkg_ident(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool is_safe_flat_filename(std::string_view s) {
    if (s.empty() || s == "." || s == ".." || s.size() > 255) return false;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '\0' || static_cast<unsigned char>(c) < 32)
            return false;
    }
    return true;
}

static bool is_safe_header_filename(std::string_view s) {
    if (!is_safe_flat_filename(s) || s.size() < 3 || s.substr(s.size() - 2) != ".h")
        return false;
    static const std::set<std::string_view> reserved = {
        "crt.h", "stdio.h", "stdlib.h", "stdint.h", "stddef.h", "stdbool.h",
        "string.h", "errno.h", "math.h", "limits.h", "inttypes.h", "unistd.h",
        "pthread.h", "fcntl.h", "signal.h", "time.h", "ctype.h", "assert.h"
    };
    return reserved.count(s) == 0;
}

static bool confined_subpath(const std::filesystem::path &base_dir,
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

static bool verify_bratbind_bytes(const SourceFile &sf, const std::string &target,
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

static bool module_has_valid_bratbind(const SourceFile &sf, const std::string &target) {
    std::filesystem::path bp(sf.path);
    bp.replace_extension(".bratbind");
    std::string raw_bind;
    if (!read_file(bp.string(), raw_bind)) return false;
    return verify_bratbind_bytes(sf, target, raw_bind);
}

static bool native_lower_hex_sha(std::string_view value) {
    if (value.size() != 64) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

static bool native_archive_name(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() < 'A' ||
        (value.front() > 'Z' && value.front() < 'a') || value.front() > 'z') return false;
    for (char c : value)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    std::string low(value);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (low == "con" || low == "prn" || low == "aux" || low == "nul" || low == "std") return false;
    return !(low.size() == 4 && (low.rfind("com", 0) == 0 || low.rfind("lpt", 0) == 0) &&
             low[3] >= '1' && low[3] <= '9');
}

static bool native_pkg_name(std::string_view value) {
    const size_t slash = value.find('/');
    if (slash == std::string_view::npos || value.find('/', slash + 1) != std::string_view::npos)
        return false;
    auto part_ok = [](std::string_view part) {
        if (part.empty() || part.size() > 64 ||
            !((part.front() >= 'A' && part.front() <= 'Z') ||
              (part.front() >= 'a' && part.front() <= 'z'))) return false;
        for (char c : part)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        std::string low(part);
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (low == "con" || low == "prn" || low == "aux" || low == "nul") return false;
        return !(low.size() == 4 && (low.rfind("com", 0) == 0 || low.rfind("lpt", 0) == 0) &&
                 low[3] >= '1' && low[3] <= '9');
    };
    return part_ok(value.substr(0, slash)) && part_ok(value.substr(slash + 1));
}

static bool native_pkg_version(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    size_t at = 0;
    while (at < value.size()) {
        const size_t dot = value.find('.', at);
        const size_t end = dot == std::string_view::npos ? value.size() : dot;
        if (end == at || end - at > 10) return false;
        uint64_t component = 0;
        for (size_t i = at; i < end; ++i) {
            const char c = value[i];
            if (c < '0' || c > '9') return false;
            component = component * 10 + static_cast<unsigned>(c - '0');
            if (component > 2147483647ULL) return false;
        }
        if (dot == std::string_view::npos) return true;
        at = dot + 1;
    }
    return false;
}

static bool native_pkg_constraint(std::string_view value) {
    if (value == "*") return true;
    if (value.empty()) return false;
    size_t at = 0;
    while (at < value.size()) {
        const size_t comma = value.find(',', at);
        const size_t end = comma == std::string_view::npos ? value.size() : comma;
        std::string_view part = value.substr(at, end - at);
        if (part.empty()) return false;
        if (part.substr(0, 2) == ">=" || part.substr(0, 2) == "<=") part.remove_prefix(2);
        else if (part.front() == '>' || part.front() == '<' || part.front() == '=')
            part.remove_prefix(1);
        if (!native_pkg_version(part)) return false;
        if (comma == std::string_view::npos) return true;
        at = comma + 1;
    }
    return false;
}

static std::vector<uint32_t> native_version_parts(std::string_view value) {
    std::vector<uint32_t> parts;
    size_t at = 0;
    while (at < value.size()) {
        const size_t dot = value.find('.', at);
        const size_t end = dot == std::string_view::npos ? value.size() : dot;
        uint32_t part = 0;
        for (size_t i = at; i < end; ++i)
            part = part * 10 + static_cast<uint32_t>(value[i] - '0');
        parts.push_back(part);
        if (dot == std::string_view::npos) break;
        at = dot + 1;
    }
    while (parts.size() > 1 && parts.back() == 0) parts.pop_back();
    return parts;
}

static int native_version_compare(std::string_view left, std::string_view right) {
    const auto a = native_version_parts(left);
    const auto b = native_version_parts(right);
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const uint32_t av = i < a.size() ? a[i] : 0;
        const uint32_t bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

static bool native_constraint_matches(std::string_view version,
                                      std::string_view constraint) {
    if (constraint == "*") return true;
    size_t at = 0;
    while (at < constraint.size()) {
        const size_t comma = constraint.find(',', at);
        const size_t end = comma == std::string_view::npos ? constraint.size() : comma;
        std::string_view part = constraint.substr(at, end - at);
        std::string_view op = "=";
        if (part.substr(0, 2) == ">=" || part.substr(0, 2) == "<=") {
            op = part.substr(0, 2);
            part.remove_prefix(2);
        } else if (!part.empty() && (part.front() == '>' || part.front() == '<' ||
                                     part.front() == '=')) {
            op = part.substr(0, 1);
            part.remove_prefix(1);
        }
        const int cmp = native_version_compare(version, part);
        if ((op == "=" && cmp != 0) || (op == ">=" && cmp < 0) ||
            (op == "<=" && cmp > 0) || (op == ">" && cmp <= 0) ||
            (op == "<" && cmp >= 0)) return false;
        if (comma == std::string_view::npos) return true;
        at = comma + 1;
    }
    return constraint.empty();
}

static bool native_package_file(std::string_view value) {
    if (value.size() < 6 || value.size() > 128 || value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos || value.substr(value.size() - 5) != ".brat")
        return false;
    if (!((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    for (char c : value.substr(0, value.size() - 5))
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    std::string stem(value.substr(0, value.size() - 5));
    const size_t dot = stem.find('.');
    if (dot != std::string::npos) stem.resize(dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    return !(stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
             stem[3] >= '1' && stem[3] <= '9');
}

static bool native_archive_relpath(std::string_view rel, std::string_view archive) {
    if (rel.empty() || rel.size() > 256 || rel.front() == '/' || rel.back() == '/' ||
        rel.find('\\') != std::string_view::npos || rel.find('\0') != std::string_view::npos)
        return false;
    std::vector<std::string_view> parts;
    size_t at = 0;
    while (at < rel.size()) {
        const size_t slash = rel.find('/', at);
        const size_t end = slash == std::string_view::npos ? rel.size() : slash;
        if (end == at) return false;
        parts.push_back(rel.substr(at, end - at));
        if (slash == std::string_view::npos) break;
        at = slash + 1;
    }
    if (parts.size() < 2 || parts.front() != archive) return false;
    if (parts.size() == 2 && parts[1] == "brat.pkg.json") return true;
    static constexpr std::string_view exts[] = {
        ".bratbind", ".dylib", ".brat", ".bri", ".obj", ".lib", ".dll", ".so", ".a", ".o", ".c", ".h"};
    std::string_view ext;
    for (std::string_view candidate : exts) {
        if (parts.back().size() > candidate.size() &&
            parts.back().substr(parts.back().size() - candidate.size()) == candidate) {
            ext = candidate;
            break;
        }
    }
    if (ext.empty()) return false;
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string_view stem = parts[i];
        if (i + 1 == parts.size()) stem = stem.substr(0, stem.size() - ext.size());
        if (stem.empty() || stem.size() > 64 ||
            !((stem.front() >= 'A' && stem.front() <= 'Z') ||
              (stem.front() >= 'a' && stem.front() <= 'z'))) return false;
        for (char c : stem)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) return false;
        std::string low(stem);
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (low == "con" || low == "prn" || low == "aux" || low == "nul" ||
            (low.size() == 4 && (low.rfind("com", 0) == 0 || low.rfind("lpt", 0) == 0) &&
             low[3] >= '1' && low[3] <= '9')) return false;
    }
    return true;
}

static bool native_safe_url(std::string_view url) {
    if (url.empty()) return false;
    for (unsigned char c : url) if (c < 0x20 || c == 0x7f) return false;
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos) return false;
    std::string scheme(url.substr(0, colon));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string_view rest = url.substr(colon + 1);
    if (scheme == "http" || scheme == "https") {
        if (rest.substr(0, 2) != "//") return false;
        const size_t end = rest.find_first_of("/?#", 2);
        const std::string_view authority = rest.substr(2, end == std::string_view::npos
                                                               ? end : end - 2);
        return !authority.empty() && authority.find('@') == std::string_view::npos;
    }
    if (scheme == "file") {
        if (rest.substr(0, 2) != "//") return false;
        const size_t end = rest.find('/', 2);
        std::string_view authority = rest.substr(2, end == std::string_view::npos
                                                       ? end : end - 2);
        return authority.empty() || authority == "localhost";
    }
    return false;
}

static bool native_validate_lock(const lsp_wire::LspJsonTape &tape, std::string &error) {
    if (!tape.is_array(tape.root())) {
        error = "lockfile wants a JSON list";
        return false;
    }
    std::set<std::string> files, archives, names;
    for (uint32_t item = tape.at(tape.root()).first_child; item != 0;
         item = tape.at(item).next_sibling) {
        if (!tape.is_object(item)) {
            error = "lockfile entry must be an object";
            return false;
        }
        const uint32_t archive_node = tape.get(item, "archive");
        const bool is_archive = archive_node != 0 && !tape.is_null(archive_node);
        std::string label;
        if (is_archive) {
            if (!tape.is_string(archive_node) || tape.get(item, "file") != 0 ||
                !native_archive_name(tape.str_view(archive_node))) {
                error = "lockfile entry has an unsafe archive package name";
                return false;
            }
            const std::string archive(tape.str_view(archive_node));
            std::string arc_key = archive;
            std::transform(arc_key.begin(), arc_key.end(), arc_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (archives.contains(arc_key) || files.contains(arc_key + ".brat")) {
                error = "lockfile has duplicate package " + archive;
                return false;
            }
            archives.insert(arc_key);
            label = archive;
            const uint32_t files_node = tape.get(item, "files");
            if (!tape.is_object(files_node) || tape.at(files_node).count == 0) {
                error = archive + " has no valid file manifest";
                return false;
            }
            for (uint32_t key = tape.at(files_node).first_child; key != 0;) {
                const uint32_t hash = tape.at(key).next_sibling;
                if (hash == 0 || !tape.is_string(key) || !tape.is_string(hash)) {
                    error = archive + " has an invalid file manifest";
                    return false;
                }
                const std::string rel(tape.str_view(key));
                const std::string digest(tape.str_view(hash));
                if (!native_archive_relpath(rel, archive)) {
                    error = archive + " has an unsafe archive file path";
                    return false;
                }
                if (!native_lower_hex_sha(digest)) {
                    error = archive + " has an invalid SHA-256 for " + rel;
                    return false;
                }
                std::string file_key = rel;
                std::transform(file_key.begin(), file_key.end(), file_key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!files.insert(file_key).second) {
                    error = "lockfile has duplicate package filename " + rel;
                    return false;
                }
                key = tape.at(hash).next_sibling;
            }
            const uint32_t c_sources = tape.get(item, "c_sources");
            if (c_sources != 0 && !tape.is_null(c_sources)) {
                if (!tape.is_array(c_sources) || tape.at(c_sources).count == 0) {
                    error = archive + " has an invalid c_sources list";
                    return false;
                }
                for (uint32_t cs = tape.at(c_sources).first_child; cs != 0;
                     cs = tape.at(cs).next_sibling) {
                    if (!tape.is_string(cs)) {
                        error = archive + " lists untracked or non-.c source";
                        return false;
                    }
                    const std::string rel(tape.str_view(cs));
                    if (rel.size() < 3 || rel.substr(rel.size() - 2) != ".c" ||
                        !native_archive_relpath(rel, archive) || tape.get(files_node, rel) == 0) {
                        error = archive + " lists untracked or non-.c source " + rel;
                        return false;
                    }
                }
            }
            const uint32_t dependencies = tape.get(item, "dependencies");
            if (dependencies != 0) {
                if (!tape.is_array(dependencies)) {
                    error = archive + " has invalid archive dependencies";
                    return false;
                }
                for (uint32_t dep = tape.at(dependencies).first_child; dep != 0;
                     dep = tape.at(dep).next_sibling) {
                    if (!tape.is_string(dep) || !native_archive_name(tape.str_view(dep)) ||
                        tape.str_view(dep) == archive) {
                        error = archive + " has invalid archive dependencies";
                        return false;
                    }
                }
            }
            const uint32_t version = tape.get(item, "version");
            if (version != 0 && !tape.is_null(version)) {
                const std::string value(tape.str_view(version));
                if (!tape.is_string(version) || value.empty() || value.size() > 64 ||
                    std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 33; })) {
                    error = archive + " has an invalid archive version";
                    return false;
                }
            }
        } else {
            const uint32_t file_node = tape.get(item, "file");
            if (!tape.is_string(file_node) || !native_package_file(tape.str_view(file_node))) {
                error = "lockfile entry has an unsafe package filename";
                return false;
            }
            const std::string filename(tape.str_view(file_node));
            label = filename;
            std::string file_key = filename;
            std::transform(file_key.begin(), file_key.end(), file_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string arc_key = filename.substr(0, filename.size() - 5);
            std::transform(arc_key.begin(), arc_key.end(), arc_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (files.contains(file_key) || archives.contains(arc_key)) {
                error = "lockfile has duplicate package filename " + filename;
                return false;
            }
            files.insert(file_key);
            const uint32_t name = tape.get(item, "name");
            const uint32_t version = tape.get(item, "version");
            const bool has_name = name != 0 && !tape.is_null(name);
            const bool has_version = version != 0 && !tape.is_null(version);
            if (has_name || has_version) {
                if (!tape.is_string(name) || !native_pkg_name(tape.str_view(name)) ||
                    !tape.is_string(version) || !native_pkg_version(tape.str_view(version))) {
                    error = filename + " has an invalid package identity";
                    return false;
                }
                std::string name_key(tape.str_view(name));
                std::transform(name_key.begin(), name_key.end(), name_key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!names.insert(name_key).second) {
                    error = "lockfile has duplicate package " + std::string(tape.str_view(name));
                    return false;
                }
            }
        }
        const uint32_t url = tape.get(item, "url");
        if (!tape.is_string(url) || !native_safe_url(tape.str_view(url))) {
            error = label + " has no valid URL";
            return false;
        }
        const uint32_t sha = tape.get(item, "sha256");
        if (!tape.is_string(sha) || !native_lower_hex_sha(tape.str_view(sha))) {
            error = label + " has no valid SHA-256";
            return false;
        }
    }
    return true;
}

static bool native_read_lock(std::string &raw, lsp_wire::LspJsonTape &tape,
                             bool &exists, std::string &error) {
    const std::filesystem::path path = std::filesystem::current_path() / "brat.lock";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
        ec.clear();
        exists = false;
        return true;
    }
    if (ec) {
        error = "cannot read " + path.string() + ": " + ec.message();
        return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
        error = "cannot read " + path.string() + ": not a regular file";
        return false;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot read " + path.string() + ": " + ec.message();
        return false;
    }
    if (size > 16u * 1024u * 1024u) {
        error = path.string() + " exceeds 16 MiB";
        return false;
    }
    if (!read_file(path.string(), raw)) {
        error = "cannot read " + path.string();
        return false;
    }
    if (!tape.parse(raw)) {
        error = path.string() + " is not valid JSON: " + std::string(tape.error());
        return false;
    }
    std::string validation_error;
    if (!native_validate_lock(tape, validation_error)) {
        error = path.string() + ": " + validation_error;
        return false;
    }
    exists = true;
    return true;
}

static bool native_read_vendor_file(const std::filesystem::path &root,
                                    std::string_view rel, std::string &out,
                                    std::string &error) {
    std::filesystem::path current = root;
    size_t at = 0;
    while (at < rel.size()) {
        const size_t slash = rel.find('/', at);
        const size_t end = slash == std::string_view::npos ? rel.size() : slash;
        current /= std::string(rel.substr(at, end - at));
        const bool last = slash == std::string_view::npos;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) {
            error = "No such file or directory";
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            error = last ? "not a regular file" : "not a real directory";
            return false;
        }
        if (last) {
            if (!std::filesystem::is_regular_file(status)) {
                error = "not a regular file";
                return false;
            }
            const uintmax_t size = std::filesystem::file_size(current, ec);
            if (ec) { error = ec.message(); return false; }
            if (size > 16u * 1024u * 1024u) {
                error = "file exceeds 16 MiB";
                return false;
            }
            if (!read_file(current.string(), out)) {
                error = "cannot read file";
                return false;
            }
            return true;
        }
        if (!std::filesystem::is_directory(status)) {
            error = "not a real directory";
            return false;
        }
        at = end + 1;
    }
    error = "invalid package path";
    return false;
}

static std::string host_default_triple();
static int main_cc_timeout_ms();

static bool verify_archive_c_sources(const std::string &archive,
                                     const std::map<std::string, std::string> &archive_data,
                                     const std::vector<std::string> &c_sources,
                                     const std::string &target,
                                     std::string &error) {
    if (c_sources.empty()) return true;
    std::filesystem::path stage_path;
    try {
        stage_path = std::filesystem::temp_directory_path() / "brat-pkg-verify";
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    BuildStage stage(stage_path);
    const std::string prefix = archive + "/";
    for (const auto &[rel, bytes] : archive_data) {
        if (rel.rfind(prefix, 0) != 0) {
            error = "unsafe archive path " + rel;
            return false;
        }
        const std::string local = rel.substr(prefix.size());
        if (local.empty() || local.front() == '/' || local.find("..") != std::string::npos) {
            error = "unsafe archive path " + rel;
            return false;
        }
        const auto path = stage.path / std::filesystem::path(local);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot stage " + rel + ": " + ec.message();
            return false;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot stage " + rel;
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "cannot write staged " + rel;
            return false;
        }
    }
    for (const auto &[rel, bytes] : archive_data) {
        if (rel.size() < 9 || rel.substr(rel.size() - 9) != ".bratbind") continue;
        lsp_wire::LspJsonTape bind;
        if (!bind.parse(bytes) || !bind.is_object(bind.root())) {
            error = "invalid JSON in " + rel;
            return false;
        }
        const uint32_t header = bind.get(bind.root(), "header");
        if (!bind.is_object(header)) continue;
        const std::string raw_name(bind.str_view(bind.get(header, "name")));
        const std::string name = std::filesystem::path(raw_name).filename().string();
        const std::string_view text = bind.str_view(bind.get(header, "text"));
        if (raw_name.empty() || text.empty()) continue;
        if (!is_safe_header_filename(name)) {
            error = "unsafe header name in " + rel;
            return false;
        }
        const auto header_path = stage.path / name;
        std::error_code ec;
        if (!std::filesystem::exists(header_path, ec)) {
            std::ofstream out(header_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "cannot stage embedded header " + name;
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out) {
                error = "cannot write embedded header " + name;
                return false;
            }
        }
    }

    std::string compiler;
    bool zig = false;
    if (const char *override = std::getenv("BRAT_CC"); override && *override) {
        compiler = override;
    } else if (!target.empty() && executable_on_path("zig")) {
        compiler = "zig";
    } else if (!target.empty()) {
        error = "--target needs zig cc";
        return false;
    } else if (executable_on_path("cc")) {
        compiler = "cc";
    } else if (executable_on_path("gcc")) {
        compiler = "gcc";
    } else {
        error = "no C compiler found (set BRAT_CC)";
        return false;
    }
    zig = std::filesystem::path(compiler).filename().string().rfind("zig", 0) == 0;
    if (!target.empty() && !zig) {
        error = "--target needs zig cc";
        return false;
    }
    std::vector<std::string> built_objs;
    size_t cs_idx = 0;
    for (const std::string &c_source : c_sources) {
        if (c_source.rfind(prefix, 0) != 0) {
            error = "unsafe c_source path " + c_source;
            return false;
        }
        const std::string local = c_source.substr(prefix.size());
        if (local.size() < 3 || local.substr(local.size() - 2) != ".c" ||
            local.find("..") != std::string::npos || local.front() == '/') {
            error = "unsafe c_source path " + c_source;
            return false;
        }
        const auto source = stage.path / std::filesystem::path(local);
        const auto object = stage.path / ("__verify_cs_" + std::to_string(cs_idx++) + ".o");
        std::vector<std::string> args{compiler};
        if (zig) args.push_back("cc");
        if (!target.empty()) {
            args.push_back("-target");
            args.push_back(target);
        }
        args.insert(args.end(), {"-c", "-g", source.string(), "-o", object.string(),
                                 "-std=gnu11", "-O2", "-fwrapv", "-I", stage.path.string()});
        const int cc_timeout_ms = main_cc_timeout_ms();
        bool cc_timed_out = false;
        const int rc = run_process(args, cc_timeout_ms, &cc_timed_out);
        if (rc != 0) {
            error = "c_sources compile failed for " + c_source +
                    (cc_timed_out ? " (compiler timed out after " + std::to_string(cc_timeout_ms) + " ms)"
                                  : " (compiler status " + std::to_string(rc) + ")");
            return false;
        }
        built_objs.push_back(object.string());
    }
    if (!built_objs.empty()) {
        const std::string bundle_path = (stage.path / "__c_sources_bundle__.a").string();
        if (!pkg_io::write_ar_bundle(bundle_path, built_objs, error)) return false;
        const std::map<std::string, std::string> obj_map{{"__c_sources_bundle__.a", bundle_path}};
        for (const auto &[rel, bytes] : archive_data) {
            if (rel.size() < 9 || rel.substr(rel.size() - 9) != ".bratbind") continue;
            const std::string mrel = rel.substr(0, rel.size() - 9) + ".brat";
            const auto mit = archive_data.find(mrel);
            if (mit == archive_data.end()) {
                error = "missing module " + mrel + " for " + rel;
                return false;
            }
            if (!native_ffi::verify_bratbind_bytes(mit->second, bytes, target, obj_map, rel, error))
                return false;
        }
    }
    return true;
}

static int native_pkg_verify(const std::string &requested_target) {
    std::string target = requested_target;
    if (target.empty()) {
        if (const char *env_target = std::getenv("BRAT_TARGET"); env_target && *env_target)
            target = env_target;
    }
    const std::string proof_target = target.empty() ? host_default_triple() : target;
    const std::filesystem::path lock_path = std::filesystem::current_path() / "brat.lock";
    std::string lock_raw;
    std::error_code ec;
    const auto lock_status = std::filesystem::symlink_status(lock_path, ec);
    if (ec || lock_status.type() == std::filesystem::file_type::not_found) {
        std::fprintf(stderr, "brat pkg: empty lock\n");
        return 1;
    }
    if (!std::filesystem::is_regular_file(lock_status)) {
        std::fprintf(stderr, "brat pkg: cannot read %s: not a regular file\n", lock_path.string().c_str());
        return 1;
    }
    const uintmax_t lock_size = std::filesystem::file_size(lock_path, ec);
    if (ec) {
        std::fprintf(stderr, "brat pkg: cannot read %s: %s\n", lock_path.string().c_str(), ec.message().c_str());
        return 1;
    }
    if (lock_size > 16u * 1024u * 1024u) {
        std::fprintf(stderr, "brat pkg: %s exceeds 16 MiB\n", lock_path.string().c_str());
        return 1;
    }
    if (!read_file(lock_path.string(), lock_raw)) {
        std::fprintf(stderr, "brat pkg: cannot read %s\n", lock_path.string().c_str());
        return 1;
    }
    lsp_wire::LspJsonTape tape;
    if (!tape.parse(lock_raw)) {
        std::fprintf(stderr, "brat pkg: %s is not valid JSON: %.*s\n",
                     lock_path.string().c_str(), static_cast<int>(tape.error().size()), tape.error().data());
        return 1;
    }
    std::string lock_validation_error;
    if (!native_validate_lock(tape, lock_validation_error)) {
        std::fprintf(stderr, "brat pkg: %s: %s\n", lock_path.string().c_str(), lock_validation_error.c_str());
        return 1;
    }
    const uint32_t root_node = tape.root();
    if (!tape.is_array(root_node)) {
        std::fprintf(stderr, "brat pkg: %s: lockfile wants a JSON list\n", lock_path.string().c_str());
        return 1;
    }
    if (tape.at(root_node).count == 0) {
        std::fprintf(stderr, "brat pkg: empty lock\n");
        return 1;
    }
    const std::filesystem::path vendor = std::filesystem::current_path() / "vendor";
    auto vendor_status = std::filesystem::symlink_status(vendor, ec);
    if (ec || vendor_status.type() == std::filesystem::file_type::not_found) {
        ec.clear();
        if (!std::filesystem::create_directory(vendor, ec) && ec) {
            std::fprintf(stderr, "brat pkg: verify failed: %s\n", ec.message().c_str());
            return 1;
        }
        vendor_status = std::filesystem::symlink_status(vendor, ec);
    }
    if (ec || std::filesystem::is_symlink(vendor_status) || !std::filesystem::is_directory(vendor_status)) {
        std::fprintf(stderr, "brat pkg: verify failed: vendor path is not a real directory\n");
        return 1;
    }
    std::set<std::string> file_keys, archive_keys, name_keys;
    size_t verified = 0;
    for (uint32_t item_node = tape.at(root_node).first_child; item_node != 0;
         item_node = tape.at(item_node).next_sibling) {
        if (!tape.is_object(item_node)) {
            std::fprintf(stderr, "brat pkg: %s: lockfile entry must be an object\n", lock_path.string().c_str());
            return 1;
        }
        const uint32_t archive_node = tape.get(item_node, "archive");
        const bool is_archive = archive_node != 0 && !tape.is_null(archive_node);
        std::string archive, filename;
        std::string label;
        if (is_archive) {
            if (!tape.is_string(archive_node)) {
                std::fprintf(stderr, "brat pkg: %s: lockfile entry has an unsafe archive package name\n", lock_path.string().c_str());
                return 1;
            }
            archive.assign(tape.str_view(archive_node));
            std::string low = archive;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tape.get(item_node, "file") != 0 || !native_archive_name(archive) ||
                archive_keys.contains(low) || file_keys.contains(low + ".brat")) {
                std::fprintf(stderr, "brat pkg: %s: lockfile entry has an unsafe archive package name\n", lock_path.string().c_str());
                return 1;
            }
            archive_keys.insert(low);
            label = archive;
            const uint32_t files_node = tape.get(item_node, "files");
            if (!tape.is_object(files_node) || tape.at(files_node).count == 0) {
                std::fprintf(stderr, "brat pkg: %s has no valid file manifest\n", archive.c_str());
                return 1;
            }
            std::map<std::string, std::string> archive_data;
            for (uint32_t key_node = tape.at(files_node).first_child; key_node != 0;) {
                const uint32_t hash_node = tape.at(key_node).next_sibling;
                if (hash_node == 0 || !tape.is_string(key_node) || !tape.is_string(hash_node)) {
                    std::fprintf(stderr, "brat pkg: %s has an invalid file manifest\n", archive.c_str());
                    return 1;
                }
                const std::string rel(tape.str_view(key_node));
                const std::string hash(tape.str_view(hash_node));
                std::string rel_key = rel;
                std::transform(rel_key.begin(), rel_key.end(), rel_key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!native_archive_relpath(rel, archive)) {
                    std::fprintf(stderr, "brat pkg: %s has an unsafe archive file path\n", archive.c_str());
                    return 1;
                }
                if (!native_lower_hex_sha(hash)) {
                    std::fprintf(stderr, "brat pkg: %s has an invalid SHA-256 for %s\n", archive.c_str(), rel.c_str());
                    return 1;
                }
                if (!file_keys.insert(rel_key).second) {
                    std::fprintf(stderr, "brat pkg: lockfile has duplicate package filename %s\n", rel.c_str());
                    return 1;
                }
                std::string body, why;
                if (!native_read_vendor_file(vendor, rel, body, why)) {
                    std::fprintf(stderr, "brat pkg: verify %s: %s\n", rel.c_str(), why.c_str());
                    return 1;
                }
                if (sha256_hex(body) != hash) {
                    std::fprintf(stderr, "brat pkg: HASH MISMATCH %s\n", rel.c_str());
                    return 1;
                }
                archive_data.emplace(rel, std::move(body));
                key_node = tape.at(hash_node).next_sibling;
            }
            std::vector<std::string> archive_c_sources;
            const uint32_t csrc_node = tape.get(item_node, "c_sources");
            if (csrc_node != 0 && !tape.is_null(csrc_node)) {
                if (!tape.is_array(csrc_node) || tape.at(csrc_node).count == 0) {
                    std::fprintf(stderr, "brat pkg: %s has an invalid c_sources list\n", archive.c_str());
                    return 1;
                }
                for (uint32_t cs_node = tape.at(csrc_node).first_child; cs_node != 0;
                     cs_node = tape.at(cs_node).next_sibling) {
                    if (!tape.is_string(cs_node)) {
                        std::fprintf(stderr, "brat pkg: %s lists untracked or non-.c source\n", archive.c_str());
                        return 1;
                    }
                    const std::string cs(tape.str_view(cs_node));
                    const auto found = archive_data.find(cs);
                    if (cs.size() < 3 || cs.substr(cs.size() - 2) != ".c" || found == archive_data.end()) {
                        std::fprintf(stderr, "brat pkg: %s lists untracked or non-.c source %s\n", archive.c_str(), cs.c_str());
                        return 1;
                    }
                    archive_c_sources.push_back(cs);
                }
            }
            const std::string manifest_path = archive + "/brat.pkg.json";
            const auto manifest_it = archive_data.find(manifest_path);
            if (!archive_c_sources.empty() || manifest_it != archive_data.end()) {
                lsp_wire::LspJsonTape manifest_tape;
                if (manifest_it != archive_data.end() &&
                    (!manifest_tape.parse(manifest_it->second) || !manifest_tape.is_object(manifest_tape.root()))) {
                    std::fprintf(stderr, "brat pkg: verify %s: invalid brat.pkg.json\n", archive.c_str());
                    return 1;
                }
                std::set<std::string> locked_sources(archive_c_sources.begin(), archive_c_sources.end());
                std::set<std::string> manifest_sources;
                uint32_t manifest_cs = 0;
                if (manifest_it != archive_data.end())
                    manifest_cs = manifest_tape.get(manifest_tape.root(), "c_sources");
                if (manifest_cs != 0 && !manifest_tape.is_null(manifest_cs) &&
                    !manifest_tape.is_array(manifest_cs)) {
                    std::fprintf(stderr, "brat pkg: verify %s: invalid c_sources manifest\n", archive.c_str());
                    return 1;
                }
                const std::string prefix = archive + "/";
                if (manifest_tape.is_array(manifest_cs)) {
                    for (uint32_t cs_node = manifest_tape.at(manifest_cs).first_child; cs_node != 0;
                         cs_node = manifest_tape.at(cs_node).next_sibling) {
                        if (!manifest_tape.is_string(cs_node)) {
                            std::fprintf(stderr, "brat pkg: verify %s: invalid c_sources manifest\n", archive.c_str());
                            return 1;
                        }
                        std::string rel(manifest_tape.str_view(cs_node));
                        if (rel.rfind(prefix, 0) == 0) rel.erase(0, prefix.size());
                        const std::string full = prefix + rel;
                        if (!native_archive_relpath(full, archive) || rel.size() < 3 ||
                            rel.substr(rel.size() - 2) != ".c" || !archive_data.contains(full)) {
                            std::fprintf(stderr, "brat pkg: verify %s: invalid c_source %s\n",
                                         archive.c_str(), rel.c_str());
                            return 1;
                        }
                        manifest_sources.insert(full);
                    }
                }
                if (manifest_sources != locked_sources) {
                    std::fprintf(stderr, "brat pkg: verify %s: c_sources lock does not match brat.pkg.json\n",
                                 archive.c_str());
                    return 1;
                }
                if (!archive_c_sources.empty()) {
                    bool has_bind = false;
                    for (const auto &entry : archive_data)
                        if (entry.first.size() >= 9 && entry.first.substr(entry.first.size() - 9) == ".bratbind")
                            has_bind = true;
                    if (!has_bind) {
                        std::fprintf(stderr, "brat pkg: verify %s: c_sources without a .bratbind proof\n",
                                     archive.c_str());
                        return 1;
                    }
                }
            }
            const uint32_t deps_node = tape.get(item_node, "dependencies");
            if (deps_node != 0 && !tape.is_null(deps_node)) {
                if (!tape.is_array(deps_node)) {
                    std::fprintf(stderr, "brat pkg: %s has invalid archive dependencies\n", archive.c_str());
                    return 1;
                }
                for (uint32_t dep = tape.at(deps_node).first_child; dep != 0; dep = tape.at(dep).next_sibling) {
                    if (!tape.is_string(dep) || !native_archive_name(tape.str_view(dep)) || tape.str_view(dep) == archive) {
                        std::fprintf(stderr, "brat pkg: %s has invalid archive dependencies\n", archive.c_str());
                        return 1;
                    }
                }
            }
            const uint32_t ver_node = tape.get(item_node, "version");
            if (ver_node != 0 && !tape.is_null(ver_node)) {
                const std::string ver(tape.str_view(ver_node));
                if (!tape.is_string(ver_node) || ver.empty() || ver.size() > 64 ||
                    std::any_of(ver.begin(), ver.end(), [](unsigned char c) { return c < 33; })) {
                    std::fprintf(stderr, "brat pkg: %s has an invalid archive version\n", archive.c_str());
                    return 1;
                }
            }
            for (const auto &entry : archive_data) {
                if (entry.first.size() < 9 || entry.first.substr(entry.first.size() - 9) != ".bratbind") continue;
                const std::string module = entry.first.substr(0, entry.first.size() - 9) + ".brat";
                if (archive_data.find(module) == archive_data.end()) {
                    std::fprintf(stderr, "brat pkg: verify %s: has no matching %s in archive\n",
                                 entry.first.c_str(), module.c_str());
                    return 1;
                }
            }
            for (const auto &entry : archive_data) {
                if (entry.first.size() < 5 || entry.first.substr(entry.first.size() - 5) != ".brat") continue;
                ParseOut parsed;
                try { parsed = parse(entry.second); }
                catch (const std::exception &e) {
                    std::fprintf(stderr, "brat pkg: verify %s: %s\n", entry.first.c_str(), e.what());
                    return 1;
                }
                const std::string bind = entry.first.substr(0, entry.first.size() - 5) + ".bratbind";
                const bool has_bind = archive_data.find(bind) != archive_data.end();
                if (has_extern_decls(parsed.body) && !has_bind) {
                    std::fprintf(stderr, "brat pkg: verify %s: declares C externs without a %s proof\n",
                                 entry.first.c_str(), bind.c_str());
                    return 1;
                }
                if (has_bind) {
                    SourceFile sf;
                    sf.path = (vendor / entry.first).string();
                    sf.src = entry.second;
                    sf.body = std::move(parsed.body);
                    sf.stray = std::move(parsed.stray);
                    const auto bind_data = archive_data.find(bind);
                    if (bind_data == archive_data.end() ||
                        !verify_bratbind_bytes(sf, proof_target, bind_data->second)) {
                        std::fprintf(stderr, "brat pkg: verify %s: invalid or stale .bratbind proof\n", bind.c_str());
                        return 1;
                    }
                }
            }
            if (!archive_c_sources.empty() && target.empty()) {
                std::string why;
                if (!verify_archive_c_sources(archive, archive_data, archive_c_sources, target, why)) {
                    std::fprintf(stderr, "brat pkg: verify %s: %s\n", archive.c_str(), why.c_str());
                    return 1;
                }
            }
        } else {
            const uint32_t file_node = tape.get(item_node, "file");
            const uint32_t sha_node = tape.get(item_node, "sha256");
            if (!tape.is_string(file_node) || !native_package_file(tape.str_view(file_node))) {
                std::fprintf(stderr, "brat pkg: %s: lockfile entry has an unsafe package filename\n", lock_path.string().c_str());
                return 1;
            }
            filename.assign(tape.str_view(file_node));
            label = filename;
            std::string file_key = filename;
            std::transform(file_key.begin(), file_key.end(), file_key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string arc_key = filename.substr(0, filename.size() - 5);
            std::transform(arc_key.begin(), arc_key.end(), arc_key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (file_keys.contains(file_key) || archive_keys.contains(arc_key)) {
                std::fprintf(stderr, "brat pkg: lockfile has duplicate package filename %s\n", filename.c_str());
                return 1;
            }
            file_keys.insert(file_key);
            if (!tape.is_string(sha_node) || !native_lower_hex_sha(tape.str_view(sha_node))) {
                std::fprintf(stderr, "brat pkg: %s has no valid SHA-256\n", label.c_str());
                return 1;
            }
        }
        const uint32_t url_node = tape.get(item_node, "url");
        if (!tape.is_string(url_node) || !native_safe_url(tape.str_view(url_node))) {
            std::fprintf(stderr, "brat pkg: %s has no valid URL\n", label.c_str());
            return 1;
        }
        const uint32_t sha_node = is_archive ? tape.get(item_node, "sha256") : tape.get(item_node, "sha256");
        if (!tape.is_string(sha_node) || !native_lower_hex_sha(tape.str_view(sha_node))) {
            std::fprintf(stderr, "brat pkg: %s has no valid SHA-256\n", label.c_str());
            return 1;
        }
        if (!is_archive) {
            std::string body, why;
            if (!native_read_vendor_file(vendor, filename, body, why)) {
                std::fprintf(stderr, "brat pkg: verify %s: %s\n", filename.c_str(), why.c_str());
                return 1;
            }
            if (sha256_hex(body) != tape.str_view(sha_node)) {
                std::fprintf(stderr, "brat pkg: HASH MISMATCH %s\n", filename.c_str());
                return 1;
            }
            const uint32_t name_node = tape.get(item_node, "name");
            const uint32_t ver_node = tape.get(item_node, "version");
            if ((name_node != 0 && !tape.is_null(name_node)) || (ver_node != 0 && !tape.is_null(ver_node))) {
                if (!tape.is_string(name_node) || !native_pkg_name(tape.str_view(name_node)) ||
                    !tape.is_string(ver_node) || !native_pkg_version(tape.str_view(ver_node))) {
                    std::fprintf(stderr, "brat pkg: %s has an invalid package identity\n", filename.c_str());
                    return 1;
                }
                std::string name(tape.str_view(name_node));
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!name_keys.insert(name).second) {
                    std::fprintf(stderr, "brat pkg: lockfile has duplicate package %s\n", name.c_str());
                    return 1;
                }
            }
        }
        verified++;
    }
    std::printf("verified %zu package(s)\n", verified);
    return 0;
}

static bool has_unproven_externs(const std::vector<SourceFile> &files, const std::string &target) {
    for (const auto &sf : files) {
        if (!has_extern_decls(sf.body) && !native_ffi::has_any_ffi_surface(sf.body)) continue;
        if (!module_has_valid_bratbind(sf, target)) return true;
    }
    return false;
}

static std::vector<std::pair<std::string, std::string>> module_source_pairs(const std::vector<SourceFile> &files) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(files.size());
    for (const auto &sf : files) out.emplace_back(sf.path, sf.src);
    return out;
}

static std::string host_default_triple() {
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

static std::string build_ffi_contract_c(const std::vector<SourceFile> &files,
                                        const std::vector<StmtP> &flat,
                                        const std::string &target,
                                        const std::string &cli_header = {},
                                        const std::vector<std::string> &cli_objects = {}) {
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

static std::string platform_kind(const std::string &target);
static std::filesystem::path cache_root();

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
            const std::string key = sha256_hex(
                cbytes + "|" + hdr_material + "|" + compiler + "|" +
                (zig ? "zig|" : "") + target + "|" + (shared ? "pic" : "nopic")).substr(0, 32);
            out_digest += key + ",";
            const auto obj_path = obj_cache / (key + ".o");
            if (!std::filesystem::is_regular_file(obj_path, ec)) {
                const auto obj_tmp = obj_cache / (key + "." + std::to_string(static_cast<long long>(
#if defined(_WIN32)
                    GetCurrentProcessId()
#else
                    getpid()
#endif
                )) + ".tmp.o");
                std::vector<std::string> cc_args{compiler};
                if (zig) cc_args.push_back("cc");
                cc_args.insert(cc_args.end(), {"-c", cpath.string(), "-o", obj_tmp.string(),
                                               "-std=gnu11", "-O2", "-fwrapv", "-I", pkg_dir.string(),
                                               "-I", inc_dir.string()});
                if (!target.empty()) {
                    cc_args.push_back("-target");
                    cc_args.push_back(target);
                }
                if (shared && pkind != "windows") cc_args.push_back("-fPIC");
                const int cc_timeout_ms = main_cc_timeout_ms();
                bool cc_timed_out = false;
                if (run_process(cc_args, cc_timeout_ms, &cc_timed_out) != 0) {
                    std::filesystem::remove(obj_tmp, ec);
                    out_err = "failed compiling " + pkg_name + "/" + cs +
                              (cc_timed_out ? " (compiler timed out after " +
                                                  std::to_string(cc_timeout_ms) + " ms)"
                                            : "");
                    return false;
                }
                std::filesystem::rename(obj_tmp, obj_path, ec);
                if (ec && !std::filesystem::is_regular_file(obj_path, ec)) {
                    std::filesystem::remove(obj_tmp, ec);
                    out_err = "failed staging object for " + pkg_name + "/" + cs;
                    return false;
                }
                std::filesystem::remove(obj_tmp, ec);
            }
            out_objs.push_back(obj_path.string());
        }
    }
    return true;
}

static std::string platform_kind(const std::string &target) {
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
    return "-l:" + lib;
}

static int build_one(const char *arg0, const std::string &file,
                     const std::string &output, const std::string &target,
                     bool static_link, bool static_explicit, bool shared, int opt,
                     uint64_t memory_limit,
                     const std::vector<std::string> &link_flags = {},
                     bool unsafe_ffi = false,
                     bool trace_lines = false,
                     const std::string &ffi_header = {},
                     const std::vector<std::string> &ffi_objects = {}) {
    ModuleSet set = load_modules(file);
    mangle_modules(set);
    if (check_programs(set.files)) {
        std::fprintf(stderr, "brat-cxx build: refusing to build after checker errors\n");
        return 1;
    }
    if (native_ffi::ffi_gate(file, module_source_pairs(set.files), ffi_header, ffi_objects,
                             target, "brat build", unsafe_ffi) != 0) {
        return 1;
    }
    flatten(set);
    const std::string pkind = platform_kind(target);
    if (pkind != "linux" && static_link && !static_explicit) static_link = false;
    const auto libs = codegen::extern_libs(set.flat);
    if (!libs.empty() && static_link && !static_explicit) static_link = false;

    std::string compiler;
    if (const char *override = std::getenv("BRAT_CC"); override && *override)
        compiler = override;
    else if ((target.size() || static_explicit) && executable_on_path("zig"))
        compiler = "zig";
    else if (target.size()) {
        std::fprintf(stderr, "brat build: --target needs zig cc\n");
        return 1;
    } else if (executable_on_path("cc")) compiler = "cc";
    else if (executable_on_path("gcc")) compiler = "gcc";
    else {
        std::fprintf(stderr, "brat-cxx build: no C compiler found (set BRAT_CC)\n");
        return 1;
    }
    const bool zig = std::filesystem::path(compiler).filename().string().rfind("zig", 0) == 0;
    if (target.size() && !zig) {
        std::fprintf(stderr, "brat build: --target needs zig cc\n");
        return 1;
    }

    const std::filesystem::path output_path(output);
    BuildStage stage(output_path);
    const auto cfile = stage.path / "program.c";
    const auto temporary_output = stage.path / "program";
    const auto engine = executable_path(arg0).parent_path().parent_path().parent_path() / "engine";
    try {
        std::string c = codegen::emit_program(set.flat, dir_of(file), opt, nullptr, target, trace_lines);
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
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat-cxx build: emit failed: %s\n", e.what());
        std::error_code ec;
        std::filesystem::remove(cfile, ec);
        return 1;
    }

    std::vector<std::string> vendored_objs;
    std::string vendored_digest, vendored_err;
    if (!collect_vendored_c_objects(set.files, compiler, zig, target, shared,
                                    vendored_objs, vendored_digest, vendored_err)) {
        std::fprintf(stderr, "brat-cxx build: %s\n", vendored_err.c_str());
        return 1;
    }

    std::vector<std::string> args{compiler};
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
    // `-fwrapv` is what makes the native build agree with the interpreter on
    // integer overflow. Signed overflow is undefined in C: without the flag the
    // answer is the optimizer's, and an i64 sum that wraps is only "correct"
    // until the next compiler version notices the loop bound cannot overflow.
    args.insert(args.end(), {"-std=gnu11", "-O2", "-fwrapv", "-I", engine.string()});
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
             std::fprintf(stderr, "brat-cxx build: C compiler timed out after %d ms\n", cc_timeout_ms);
         else
             std::fprintf(stderr, "brat-cxx build: C compiler exited with status %d\n", result);
         return 1;
     }
 #if defined(_WIN32)
     if (!MoveFileExW(temporary_output.wstring().c_str(), output_path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
         const DWORD error = GetLastError();
         std::fprintf(stderr, "brat-cxx build: cannot install output (Windows error %lu)\n",
                      static_cast<unsigned long>(error));
         return 1;
     }
 #else
     std::error_code ec;
     std::filesystem::rename(temporary_output, output_path, ec);
     if (ec) {
         std::fprintf(stderr, "brat-cxx build: cannot install output: %s\n", ec.message().c_str());
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
                   const std::vector<std::string> &ffi_objects = {}) {
    ModuleSet set = load_modules(file);
    mangle_modules(set);
    // Quiet on success: `run` is not `check`, and a program that runs must not
    // print a report first. What it does report goes to stderr: stdout is the program's.
    g_report = stderr;
    const int checker_errors = check_programs(set.files, true);
    g_report = stdout;
    if (checker_errors) {
        std::fflush(stdout);
        std::fprintf(stderr, "brat-cxx run: refusing to run after checker errors\n");
        return 1;
    }
    if (native_ffi::ffi_gate(file, module_source_pairs(set.files), ffi_header, ffi_objects,
                             "", "brat run", unsafe_ffi) != 0) {
        std::fflush(stdout);
        return 1;
    }
    flatten(set);
    const auto engine = executable_path(arg0).parent_path().parent_path().parent_path() / "engine";
    std::string header;
    if (!read_file((engine / "crt.h").string(), header)) {
        std::fprintf(stderr, "brat-cxx run: cannot read %s\n", (engine / "crt.h").string().c_str());
        return 1;
    }
    const auto libs = codegen::extern_libs(set.flat);
    std::string source = codegen::emit_program(set.flat, dir_of(file), opt, nullptr, "", trace_lines);
    std::string compiler;
    if (const char *override = std::getenv("BRAT_CC"); override && *override) compiler = override;
    else if (executable_on_path("cc")) compiler = "cc";
    else if (executable_on_path("gcc")) compiler = "gcc";
    else {
        std::fprintf(stderr, "brat-cxx run: no C compiler found (set BRAT_CC)\n");
        return 1;
    }
    std::vector<std::string> vendored_objs;
    std::string vendored_digest, vendored_err;
    if (!collect_vendored_c_objects(set.files, compiler, false, "", false,
                                    vendored_objs, vendored_digest, vendored_err)) {
        std::fprintf(stderr, "brat-cxx run: %s\n", vendored_err.c_str());
        return 1;
    }
    // The key covers the program, the runtime it is compiled against, and the
    // flags that change the binary. Cwd and file names are not in it on
    // purpose: `vbase_set` is baked into the emitted C.
    std::string key_material = sha256_hex(source) + sha256_hex(header) +
                               "|opt=" + std::to_string(opt) +
                               "|mem=" + std::to_string(memory_limit) +
                               "|cobj=" + vendored_digest + "|libs=";
    for (const auto &lib : libs) key_material += lib + ",";
    for (const auto &fo : ffi_objects) {
        std::string obytes;
        if (read_file(fo, obytes)) key_material += "|fo=" + sha256_hex(obytes);
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
    if (std::filesystem::exists(binary, ec)) {
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
            std::fprintf(stderr, "brat-cxx run: cannot write %s\n", cfile.string().c_str());
            return 1;
        }
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
    }
#if defined(_WIN32)
    const auto temporary = dir / (key + "." + pid_tag + ".tmp.exe");
#else
    const auto temporary = dir / (key + "." + pid_tag + ".tmp");
#endif
    const std::string host_pkind = platform_kind("");
    std::vector<std::string> args{compiler, cfile.string()};
    for (const std::string &vo : vendored_objs) args.push_back(vo);
    for (const std::string &fo : ffi_objects) {
        const std::string ext = std::filesystem::path(fo).extension().string();
        if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib")
            args.push_back(fo);
    }
    args.insert(args.end(), {"-o", temporary.string(), "-std=gnu11", "-O2", "-fwrapv", "-I", engine.string()});
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
            std::fprintf(stderr, "brat-cxx run: C compiler timed out after %d ms\n", cc_timeout_ms);
        else
            std::fprintf(stderr, "brat-cxx run: C compiler exited with status %d\n", built);
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
        std::fprintf(stderr, "brat-cxx run: cannot install %s: %s\n", binary.string().c_str(),
                     ec.message().c_str());
        return 1;
    }
    pass_argv[0] = const_cast<char *>(binary_path.c_str());
    std::fflush(stdout);
    execv(pass_argv[0], pass_argv);
    std::perror("brat-cxx run: exec");
    return 127;
#endif
}

static bool parse_memory(const std::string &input, uint64_t &value) {
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

// Parse diagnostics have one shape across the two front ends, the same one
// `lang/engine/diag.py` prints for ownership and type errors:
//
//     file:line: error: message at line N
//       | <the source line, right-stripped>
//
// Everything a tool greps for -- `: error: ` and the line number -- is here, so
// a script that reads the Python checker's output reads this one too.
static void print_parse_error(const std::string &path, const ParseError &e) {
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
static std::size_t ownership_errors(const std::vector<SourceFile> &files, std::size_t i) {
    std::map<std::string, std::vector<ParamMode>> extra;
    std::set<std::string> extra_consts;
    std::set<std::string> extra_scalar;
    std::map<std::string, std::vector<size_t>> extra_borrows;
    std::set<std::string> extra_owned;
    for (std::size_t j = 0; j < files.size(); j++) {
        if (j == i) continue;
        FnTable ps = pub_sigs(files[j].body, exported_fn_names(files[j]));
        for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
        extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
        extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
        for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
        extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
    }
    std::set<std::string> module_heads = imported_module_heads(files[i]);
    BorrowOut out = borrow_check(files[i].body, extra, files[i].stray, module_heads,
                                 extra_consts, extra_scalar, extra_borrows, extra_owned);
    report(files[i].path, files[i].src, out.errs, "error");
    return out.errs.size();
}

static std::size_t type_errors(const std::vector<SourceFile> &files, std::size_t i,
                               bool arena_only = false) {
    Sigs merged;
    std::map<std::string, Ty> merged_consts;
    std::map<std::string, std::map<std::string, Ty>> merged_structs;
    for (std::size_t j = 0; j < files.size(); j++) {
        if (j == i) continue;
        collect_exports(files[j].body, merged_consts, merged_structs);
        collect_sigs(files[j].body, merged);
    }
    std::vector<TypeErr> errs = type_check(files[i].body, merged, merged_consts, merged_structs);
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

static void print_cli_usage() {
    std::fprintf(stderr,
                 "usage: brat --version\n"
                 "       brat run|build|dist|check|deps|api|fmt|test|doc|new|emit-c|ffi|explain|lsp|mcp|bundle ...\n"
                 "       brat run FILE [-- args...] [-O0|-O1|-O2] [--mem N]\n"
                 "       brat deps FILE [--json|--dot] [-o OUT]\n"
                 "       brat api FILE [--json]\n"
                 "       brat api --diff OLD.bri NEW.bri [--json]\n"
                 "       brat build FILE -o OUT [--link FLAG] [--mem N] [--header H]\n"
                 "       brat dist FILE [-o DIR] [--target TRIPLE[,TRIPLE...]] [--shared]\n"
                 "       brat check [--json] FILE...\n"
                 "       brat fmt [--check] [--stdout] FILE...\n"
                 "       brat test FILE_OR_DIR...\n"
                 "       brat doc FILE_OR_DIR [--json] [-o OUT]\n"
                 "       brat new NAME [--kind service|tool]\n"
                 "       brat emit-c FILE [--bundle] [--no-check] [--trace-lines] [-O0|-O1|-O2]\n"
                 "       brat ffi FILE --header H --abi-matrix [--json]\n"
                 "       brat explain [CODE] [--json]\n"
                 "       brat bundle OUTPUT_DIR\n"
                 "       brat mcp\n");
}

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
     "A borrow captured by `spawn`; the frame is copied, the borrow hangs on dead storage — data race.",
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
     "Return a copy/value, not a borrow; borrowing a parameter is fine — its storage belongs to the caller."},
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

static int cmd_check_json(const std::vector<std::string> &files) {
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
            for (size_t j = 0; j < loaded.files.size(); ++j) {
                if (j == i) continue;
                FnTable ps = pub_sigs(loaded.files[j].body, exported_fn_names(loaded.files[j]));
                for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
                extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
                extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
                for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
                extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
                collect_exports(loaded.files[j].body, merged_ty_consts, merged_ty_structs);
                collect_sigs(loaded.files[j].body, merged_ty);
            }
            std::set<std::string> module_heads = imported_module_heads(sf);
            BorrowOut bout = borrow_check(sf.body, extra, sf.stray, module_heads,
                                          extra_consts, extra_scalar, extra_borrows, extra_owned);
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
                prev == '%') {
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

static int cmd_fmt(int argc, char **argv) {
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

static int cmd_explain(int argc, char **argv) {
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

static int cmd_new(int argc, char **argv) {
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

static int cmd_doc(int argc, char **argv) {
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
            } else if (node->k == SK::Enum && is_pub) {
                std::string vs;
                for (size_t vi = 0; vi < node->variants.size(); ++vi) {
                    if (vi) vs += ", ";
                    vs += node->variants[vi].name;
                    if (!node->variants[vi].fields.empty()) {
                        vs.push_back('(');
                        for (size_t vfi = 0; vfi < node->variants[vi].fields.size(); ++vfi) {
                            if (vfi) vs += ", ";
                            vs += node->variants[vi].fields[vfi];
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

static int cmd_test(int argc, char **argv) {
    std::vector<std::string> inputs;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") continue;
        if (a == "--help" || a == "-h") { print_cli_usage(); return 0; }
        if (a == "--interp") return python_cli(argc, argv);
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
    const std::string self_exe = executable_path(argv[0]).string();
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

static bool has_arena_block_tokens(std::string_view src) {
    try {
        const auto toks = lex(src);
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (toks[i].text == "arena" && toks[i + 1].text == "{") return true;
        }
    } catch (...) {
    }
    return false;
}

static int cmd_emit_c(int argc, char **argv) {
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
        flatten(set);
        std::string out = codegen::emit_program(set.flat, dir_of(path), opt, nullptr, target, trace_lines);
        if (bundle) {
            const auto crt_path = executable_path(argv[0]).parent_path().parent_path().parent_path() /
                                  "engine" / "crt.h";
            std::string crt_text;
            if (!read_file(crt_path.string(), crt_text)) {
                std::fprintf(stderr, "brat emit-c: cannot read runtime header %s\n",
                             crt_path.string().c_str());
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

static unsigned long native_process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

static bool native_atomic_write_lock(const std::filesystem::path &path,
                                     std::string_view data,
                                     std::string &error) {
    static unsigned long sequence = 0;
    std::filesystem::path temp_dir;
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temp_dir = path.parent_path() /
            (".brat-lock-tmp-" + std::to_string(native_process_id()) + "-" +
             std::to_string(++sequence));
        if (std::filesystem::create_directory(temp_dir, ec)) break;
        if (ec && ec != std::errc::file_exists) {
            error = "cannot create temporary lock directory: " + ec.message();
            return false;
        }
        ec.clear();
        temp_dir.clear();
    }
    if (temp_dir.empty()) {
        error = "cannot allocate a temporary lock directory";
        return false;
    }
    std::filesystem::permissions(temp_dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        error = "cannot secure temporary lock directory: " + ec.message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
    const std::filesystem::path temporary = temp_dir / "brat.lock";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write temporary lockfile";
            std::filesystem::remove_all(temp_dir, ec);
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            error = "cannot write temporary lockfile";
            out.close();
            std::filesystem::remove_all(temp_dir, ec);
            return false;
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace brat.lock: " +
                std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = "cannot replace brat.lock: " + ec.message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
#endif
    std::filesystem::remove(temp_dir, ec);
    return true;
}

static bool native_atomic_write_data(const std::filesystem::path &path,
                                     std::string_view data,
                                     std::string &error) {
    static unsigned long sequence = 0;
    const std::filesystem::path parent = path.parent_path();
    const std::string filename = path.filename().string();
    if (parent.empty() || filename.empty()) {
        error = "invalid output path";
        return false;
    }
    std::filesystem::path temp_dir;
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temp_dir = parent /
            (".brat-publish-tmp-" + std::to_string(native_process_id()) + "-" +
             std::to_string(++sequence));
        if (std::filesystem::create_directory(temp_dir, ec)) break;
        if (ec && ec != std::errc::file_exists) {
            error = "cannot create temporary output directory: " + ec.message();
            return false;
        }
        ec.clear();
        temp_dir.clear();
    }
    if (temp_dir.empty()) {
        error = "cannot allocate a temporary output directory";
        return false;
    }
    std::filesystem::permissions(temp_dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
        error = "cannot secure temporary output directory: " + ec.message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
    const std::filesystem::path temporary = temp_dir / path.filename();
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write temporary output file";
            std::filesystem::remove_all(temp_dir, ec);
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            error = "cannot write temporary output file";
            out.close();
            std::filesystem::remove_all(temp_dir, ec);
            return false;
        }
    }
    std::filesystem::permissions(temporary,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        error = "cannot secure temporary output file: " + ec.message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + filename + ": " +
                std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = "cannot replace " + filename + ": " + ec.message();
        std::filesystem::remove_all(temp_dir, ec);
        return false;
    }
#endif
    std::filesystem::remove(temp_dir, ec);
    return true;
}

static bool native_read_limited(const std::filesystem::path &path, size_t limit,
                                std::string &out, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    out.resize(limit + 1);
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    const std::streamsize got = in.gcount();
    if (in.bad()) {
        out.clear();
        error = "cannot read " + path.string();
        return false;
    }
    out.resize(static_cast<size_t>(got));
    if (out.size() > limit) {
        error = path.string() + " exceeds 16 MiB";
        out.clear();
        return false;
    }
    return true;
}

static bool native_write_lock_items(const lsp_wire::LspJsonTape &tape,
                                    const std::vector<uint32_t> &items,
                                    std::string &error) {
    lsp_wire::JsonWriter writer;
    writer.begin_arr();
    for (uint32_t item : items) writer.raw_value(tape.raw_json_slice(item));
    writer.end_arr();
    std::string data(writer.view());
    data.push_back('\n');
    return native_atomic_write_lock(std::filesystem::current_path() / "brat.lock", data, error);
}

static void native_json_indent(std::string &out, size_t depth) {
    out.append(depth * 2, ' ');
}

static bool native_json_pretty(const lsp_wire::LspJsonTape &tape, uint32_t node,
                               std::string &out, size_t depth, std::string &error) {
    if (node == 0 || depth > 128) {
        error = "index JSON is too deeply nested";
        return false;
    }
    const auto kind = tape.at(node).kind;
    if (kind == lsp_wire::JKind::Null) { out += "null"; return true; }
    if (kind == lsp_wire::JKind::Bool) {
        out += tape.bool_val(node) ? "true" : "false";
        return true;
    }
    if (kind == lsp_wire::JKind::Int) {
        out += std::to_string(tape.at(node).num.i64);
        return true;
    }
    if (kind == lsp_wire::JKind::Float) {
        const std::string_view raw = tape.raw_json_slice(node);
        if (raw.empty()) { error = "index JSON has an invalid number"; return false; }
        out.append(raw.data(), raw.size());
        return true;
    }
    if (tape.is_string(node)) {
        lsp_wire::JsonWriter::write_escaped_string(out, tape.str_view(node));
        return true;
    }
    if (tape.is_array(node)) {
        const auto &array = tape.at(node);
        if (array.count == 0) { out += "[]"; return true; }
        out += "[\n";
        uint32_t child = array.first_child;
        for (uint32_t i = 0; i < array.count; ++i) {
            if (child == 0) { error = "index JSON has a malformed array"; return false; }
            native_json_indent(out, depth + 1);
            if (!native_json_pretty(tape, child, out, depth + 1, error)) return false;
            child = tape.at(child).next_sibling;
            out += i + 1 == array.count ? "\n" : ",\n";
        }
        native_json_indent(out, depth);
        out.push_back(']');
        return true;
    }
    if (tape.is_object(node)) {
        const auto &object = tape.at(node);
        if (object.count == 0) { out += "{}"; return true; }
        out += "{\n";
        uint32_t key = object.first_child;
        for (uint32_t i = 0; i < object.count; ++i) {
            if (key == 0 || !tape.is_string(key)) {
                error = "index JSON has a malformed object";
                return false;
            }
            const uint32_t value = tape.at(key).next_sibling;
            if (value == 0) { error = "index JSON has a malformed object"; return false; }
            native_json_indent(out, depth + 1);
            lsp_wire::JsonWriter::write_escaped_string(out, tape.str_view(key));
            out += ": ";
            if (!native_json_pretty(tape, value, out, depth + 1, error)) return false;
            key = tape.at(value).next_sibling;
            out += i + 1 == object.count ? "\n" : ",\n";
        }
        native_json_indent(out, depth);
        out.push_back('}');
        return true;
    }
    error = "index JSON has an unsupported value";
    return false;
}

static void native_json_new_version(std::string &out, std::string_view url,
                                    std::string_view digest,
                                    const std::vector<std::pair<std::string, std::string>> &deps,
                                    size_t depth) {
    out += "{\n";
    native_json_indent(out, depth + 1);
    lsp_wire::JsonWriter::write_escaped_string(out, "url");
    out += ": ";
    lsp_wire::JsonWriter::write_escaped_string(out, url);
    out += ",\n";
    native_json_indent(out, depth + 1);
    lsp_wire::JsonWriter::write_escaped_string(out, "sha256");
    out += ": ";
    lsp_wire::JsonWriter::write_escaped_string(out, digest);
    out += ",\n";
    native_json_indent(out, depth + 1);
    lsp_wire::JsonWriter::write_escaped_string(out, "dependencies");
    out += ": ";
    if (deps.empty()) {
        out += "{}\n";
    } else {
        out += "{\n";
        for (size_t i = 0; i < deps.size(); ++i) {
            native_json_indent(out, depth + 2);
            lsp_wire::JsonWriter::write_escaped_string(out, deps[i].first);
            out += ": ";
            lsp_wire::JsonWriter::write_escaped_string(out, deps[i].second);
            out += i + 1 == deps.size() ? "\n" : ",\n";
        }
        native_json_indent(out, depth + 1);
        out += "}\n";
    }
    native_json_indent(out, depth);
    out.push_back('}');
}

static bool native_json_write_versions(
        const lsp_wire::LspJsonTape &tape, uint32_t versions,
        std::string_view new_version, std::string_view url, std::string_view digest,
        const std::vector<std::pair<std::string, std::string>> &deps,
        std::string &out, size_t depth, std::string &error) {
    if (versions != 0 && !tape.is_object(versions)) {
        error = "package versions entry is not an object";
        return false;
    }
    out += "{\n";
    size_t emitted = 0;
    bool wrote_new_version = false;
    if (versions != 0) {
        uint32_t key = tape.at(versions).first_child;
        for (uint32_t i = 0; i < tape.at(versions).count; ++i) {
            if (key == 0 || !tape.is_string(key)) {
                error = "package versions entry is malformed";
                return false;
            }
            const uint32_t value = tape.at(key).next_sibling;
            if (value == 0) { error = "package versions entry is malformed"; return false; }
            const std::string_view name = tape.str_view(key);
            const uint32_t next = tape.at(value).next_sibling;
            if (emitted++) out += ",\n";
            native_json_indent(out, depth + 1);
            lsp_wire::JsonWriter::write_escaped_string(out, name);
            out += ": ";
            if (name == new_version) {
                native_json_new_version(out, url, digest, deps, depth + 1);
                wrote_new_version = true;
            } else if (!native_json_pretty(tape, value, out, depth + 1, error)) {
                return false;
            }
            key = next;
        }
    }
    if (!wrote_new_version) {
        if (emitted++) out += ",\n";
        native_json_indent(out, depth + 1);
        lsp_wire::JsonWriter::write_escaped_string(out, new_version);
        out += ": ";
        native_json_new_version(out, url, digest, deps, depth + 1);
    }
    out += "\n";
    native_json_indent(out, depth);
    out.push_back('}');
    return true;
}

static bool native_index_path(std::filesystem::path &path, std::string &error) {
    const char *env = std::getenv("BRAT_INDEX");
    const std::string value = env && *env ? env : "brat-index.json";
    if (value.find("://") != std::string::npos) {
        error = "publish requires a local BRAT_INDEX file";
        return false;
    }
    std::error_code ec;
    path = std::filesystem::absolute(std::filesystem::path(value), ec).lexically_normal();
    if (ec) { error = "cannot resolve index path: " + ec.message(); return false; }
    return true;
}

static bool native_object_member(const lsp_wire::LspJsonTape &tape, uint32_t object,
                                 std::string_view wanted, uint32_t &value,
                                 std::string &error) {
    value = 0;
    uint32_t key = tape.at(object).first_child;
    for (uint32_t i = 0; i < tape.at(object).count; ++i) {
        if (key == 0 || !tape.is_string(key)) {
            error = "index JSON has a malformed object";
            return false;
        }
        const uint32_t child = tape.at(key).next_sibling;
        if (child == 0) { error = "index JSON has a malformed object"; return false; }
        if (tape.str_view(key) == wanted) {
            if (value != 0) {
                error = "index JSON has a duplicate key " + std::string(wanted);
                return false;
            }
            value = child;
        }
        key = tape.at(child).next_sibling;
    }
    return true;
}

static bool native_json_unique_keys(const lsp_wire::LspJsonTape &tape, uint32_t node,
                                    size_t depth, std::string &error) {
    if (depth > 128) { error = "index JSON is too deeply nested"; return false; }
    if (tape.is_array(node)) {
        uint32_t child = tape.at(node).first_child;
        for (uint32_t i = 0; i < tape.at(node).count; ++i) {
            if (child == 0 || !native_json_unique_keys(tape, child, depth + 1, error))
                return false;
            child = tape.at(child).next_sibling;
        }
    } else if (tape.is_object(node)) {
        std::set<std::string> keys;
        uint32_t key = tape.at(node).first_child;
        for (uint32_t i = 0; i < tape.at(node).count; ++i) {
            if (key == 0 || !tape.is_string(key)) {
                error = "index JSON has a malformed object";
                return false;
            }
            const uint32_t value = tape.at(key).next_sibling;
            if (value == 0) { error = "index JSON has a malformed object"; return false; }
            if (!keys.emplace(tape.str_view(key)).second) {
                error = "index JSON has a duplicate key " + std::string(tape.str_view(key));
                return false;
            }
            if (!native_json_unique_keys(tape, value, depth + 1, error)) return false;
            key = tape.at(value).next_sibling;
        }
    }
    return true;
}

static bool native_read_pkg_index(const std::filesystem::path &path, std::string &raw,
                                  lsp_wire::LspJsonTape &tape, bool &exists,
                                  std::string &error) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
        ec.clear();
        exists = false;
        return true;
    }
    if (ec) { error = "index " + path.string() + ": " + ec.message(); return false; }
    if (!std::filesystem::is_regular_file(status)) {
        error = "index " + path.string() + ": not a regular file";
        return false;
    }
    if (!native_read_limited(path, 16u * 1024u * 1024u, raw, error)) return false;
    if (!tape.parse(raw)) {
        error = "index " + path.string() + " is not valid JSON: " + std::string(tape.error());
        return false;
    }
    if (!native_json_unique_keys(tape, tape.root(), 0, error)) return false;
    if (!tape.is_object(tape.root())) {
        error = "index " + path.string() + " wants {\"packages\": {...}}";
        return false;
    }
    uint32_t packages = 0;
    if (!native_object_member(tape, tape.root(), "packages", packages, error)) return false;
    if (!tape.is_object(packages)) {
        error = "index " + path.string() + " wants {\"packages\": {...}}";
        return false;
    }
    exists = true;
    return true;
}

static std::string native_file_uri(const std::filesystem::path &path) {
    std::string raw = std::filesystem::absolute(path).lexically_normal().generic_string();
#if defined(_WIN32)
    if (raw.size() >= 2 && raw[1] == ':') raw.insert(raw.begin(), '/');
#endif
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    if (raw.empty() || raw.front() != '/') out.push_back('/');
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        const bool drive_colon =
#if defined(_WIN32)
            i == 2 && raw.size() > 2 && raw[0] == '/' && raw[2] == ':';
#else
            false;
#endif
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
            c == '.' || c == '~' || drive_colon) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static bool native_fetch_raw_url(std::string_view url, std::string &body,
                                 std::string &error, bool allow_file = true,
                                 bool block_internal_root = false,
                                 bool *connected_internal_out = nullptr) {
    if (!native_safe_url(url)) { error = "invalid URL"; return false; }
    return pkg_io::fetch_url(std::string(url), body, error, allow_file,
                             16u * 1024u * 1024u, block_internal_root, connected_internal_out);
}

static bool native_fetch_package_url(std::string_view url, std::string &body,
                                     std::string &error, bool allow_file = true,
                                     bool block_internal_root = false,
                                     bool *connected_internal_out = nullptr) {
    if (!native_fetch_raw_url(url, body, error, allow_file, block_internal_root, connected_internal_out))
        return false;
    uint32_t bad_line = 1;
    if (!utf8_valid(body, bad_line)) {
        error = "non-utf8 dep " + std::string(url);
        return false;
    }
    return true;
}

static bool native_read_pkg_index_any(std::string &raw,
                                      lsp_wire::LspJsonTape &tape,
                                      bool &exists,
                                      bool &is_remote,
                                      std::string &index_label,
                                      std::string &error,
                                      bool *index_block_internal_out = nullptr) {
    if (index_block_internal_out) *index_block_internal_out = false;
    const char *env = std::getenv("BRAT_INDEX");
    const std::string value = env && *env ? env : "brat-index.json";
    if (value.find("://") != std::string::npos) {
        index_label = value;
        std::string scheme = value.substr(0, value.find("://"));
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        is_remote = scheme != "file";
        bool conn_internal = false;
        if (!native_fetch_package_url(value, raw, error, true, false, &conn_internal)) {
            error = "cannot fetch index " + value + ": " + error;
            return false;
        }
        if (index_block_internal_out) *index_block_internal_out = is_remote && !conn_internal;
        if (!tape.parse(raw)) {
            error = "index " + value + " is not valid JSON: " + std::string(tape.error());
            return false;
        }
        if (!native_json_unique_keys(tape, tape.root(), 0, error)) return false;
        if (!tape.is_object(tape.root())) {
            error = "index " + value + " wants {\"packages\": {...}}";
            return false;
        }
        uint32_t packages = 0;
        if (!native_object_member(tape, tape.root(), "packages", packages, error)) return false;
        if (!tape.is_object(packages)) {
            error = "index " + value + " wants {\"packages\": {...}}";
            return false;
        }
        exists = true;
        return true;
    }
    is_remote = false;
    std::error_code ec;
    const auto path = std::filesystem::absolute(std::filesystem::path(value), ec).lexically_normal();
    if (ec) {
        error = "cannot resolve index path: " + ec.message();
        return false;
    }
    index_label = path.string();
    return native_read_pkg_index(path, raw, tape, exists, error);
}

static int native_pkg_list() {
    std::string raw, error;
    bool exists = false;
    lsp_wire::LspJsonTape tape;
    if (!native_read_lock(raw, tape, exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!exists || tape.at(tape.root()).count == 0) {
        std::puts("empty lock");
        return 0;
    }
    for (uint32_t item = tape.at(tape.root()).first_child; item != 0;
         item = tape.at(item).next_sibling) {
        const uint32_t name = tape.get(item, "name");
        const uint32_t archive = tape.get(item, "archive");
        const uint32_t file = tape.get(item, "file");
        std::string who;
        if (tape.is_string(name) && !tape.str_view(name).empty()) who.assign(tape.str_view(name));
        else if (tape.is_string(archive) && !tape.str_view(archive).empty()) who.assign(tape.str_view(archive));
        else who.assign(tape.str_view(file));
        const uint32_t version = tape.get(item, "version");
        if (tape.is_string(version) && !tape.str_view(version).empty()) {
            who.push_back('@');
            who.append(tape.str_view(version));
        }
        std::string digest(tape.str_view(tape.get(item, "sha256")));
        if (digest.size() > 12) digest.resize(12);
        const std::string url(tape.str_view(tape.get(item, "url")));
        std::printf("%s  %s  %s\n", who.c_str(), digest.c_str(), url.c_str());
    }
    return 0;
}

static bool native_create_stage_dir(const std::filesystem::path &parent,
                                    std::string_view prefix,
                                    std::filesystem::path &out,
                                    std::string &error) {
    static unsigned long sequence = 0;
    std::error_code ec;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        out = parent / (std::string(prefix) + std::to_string(native_process_id()) + "-" +
                        std::to_string(++sequence));
        if (std::filesystem::create_directory(out, ec)) {
            std::filesystem::permissions(out, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            if (ec) {
                const std::string permission_message = ec.message();
                std::error_code cleanup_error;
                std::filesystem::remove_all(out, cleanup_error);
                error = "cannot secure temporary directory: " + permission_message;
                return false;
            }
            return true;
        }
        if (ec && ec != std::errc::file_exists) {
            error = ec.message();
            return false;
        }
        ec.clear();
    }
    error = "cannot allocate a temporary directory";
    return false;
}

struct NativePkgRequirement {
    std::string name;
    std::string constraint;
    std::vector<std::string> ancestry;
};

struct NativePkgSelection {
    std::string name;
    std::string version;
    uint32_t entry = 0;
};

struct NativePkgPlanItem {
    std::string name;
    std::string version;
    std::string url;
    std::string digest;
    std::string file;
    std::string text;
};

static bool native_pkg_search(
        const lsp_wire::LspJsonTape &tape, uint32_t packages,
        const std::vector<NativePkgRequirement> &pending,
        const std::map<std::string, NativePkgSelection> &chosen,
        const std::map<std::string, std::vector<std::string>> &constraints,
        std::map<std::string, NativePkgSelection> &result,
        std::string &latest_error, size_t depth = 0) {
    if (pending.empty()) { result = chosen; return true; }
    if (depth > 256 || chosen.size() > 64) {
        latest_error = "package dependency graph exceeds native resolver limits";
        return false;
    }
    const NativePkgRequirement requirement = pending.front();
    std::vector<NativePkgRequirement> rest(pending.begin() + 1, pending.end());
    if (std::find(requirement.ancestry.begin(), requirement.ancestry.end(),
                  requirement.name) != requirement.ancestry.end()) {
        std::string cycle = "dependency cycle: ";
        bool started = false;
        for (const std::string &part : requirement.ancestry) {
            if (part == requirement.name) started = true;
            if (started) cycle += (cycle.back() == ' ' ? "" : " -> ") + part;
        }
        cycle += " -> " + requirement.name;
        latest_error = std::move(cycle);
        return false;
    }
    if (!native_pkg_constraint(requirement.constraint)) {
        latest_error = "invalid package version constraint " + requirement.constraint;
        return false;
    }
    const uint32_t versions_node = tape.get(packages, requirement.name);
    if (!tape.is_object(versions_node) || tape.at(versions_node).count == 0) {
        latest_error = requirement.name + " is not in the package index";
        return false;
    }
    std::vector<std::string> versions;
    for (uint32_t key = tape.at(versions_node).first_child; key != 0;) {
        const uint32_t value = tape.at(key).next_sibling;
        if (value == 0 || !tape.is_string(key)) {
            latest_error = requirement.name + " has a malformed version map";
            return false;
        }
        const std::string version(tape.str_view(key));
        if (!native_pkg_version(version)) {
            latest_error = requirement.name + " has an invalid version in the package index";
            return false;
        }
        versions.push_back(version);
        key = tape.at(value).next_sibling;
    }
    std::stable_sort(versions.begin(), versions.end(), [](const std::string &a,
                                                           const std::string &b) {
        return native_version_compare(a, b) > 0;
    });
    auto combined = constraints;
    combined[requirement.name].push_back(requirement.constraint.empty()
                                          ? "*" : requirement.constraint);
    auto satisfies = [&](std::string_view version) {
        for (const std::string &constraint : combined[requirement.name])
            if (!native_constraint_matches(version, constraint)) return false;
        return true;
    };
    auto assigned = chosen.find(requirement.name);
    if (assigned != chosen.end()) {
        if (satisfies(assigned->second.version))
            return native_pkg_search(tape, packages, rest, chosen, combined,
                                     result, latest_error, depth + 1);
        std::string joined;
        for (const std::string &constraint : combined[requirement.name]) {
            if (!joined.empty()) joined += " and ";
            joined += constraint;
        }
        latest_error = "no version of " + requirement.name + " satisfies " + joined;
        return false;
    }
    std::vector<std::string> candidates;
    for (const std::string &version : versions)
        if (satisfies(version)) candidates.push_back(version);
    if (candidates.empty()) {
        const auto &requirements = combined[requirement.name];
        std::vector<std::string> ascending = versions;
        std::stable_sort(ascending.begin(), ascending.end(), [](const std::string &a,
                                                               const std::string &b) {
            return native_version_compare(a, b) < 0;
        });
        std::string listed;
        for (const std::string &version : ascending) {
            if (!listed.empty()) listed += ", ";
            listed += version;
        }
        if (requirements.size() == 1 && native_pkg_version(requirements.front())) {
            latest_error = requirement.name + " has no version " + requirements.front() +
                           " (index has " + listed + ")";
        } else {
            std::string joined;
            for (const std::string &constraint : requirements) {
                if (!joined.empty()) joined += " and ";
                joined += constraint;
            }
            latest_error = "no version of " + requirement.name + " satisfies " + joined +
                           " (index has " + listed + ")";
        }
        return false;
    }
    for (const std::string &version : candidates) {
        uint32_t entry = 0;
        for (uint32_t key = tape.at(versions_node).first_child; key != 0;) {
            const uint32_t value = tape.at(key).next_sibling;
            if (tape.str_view(key) == version) { entry = value; break; }
            key = tape.at(value).next_sibling;
        }
        if (!tape.is_object(entry) || tape.get(entry, "url") == 0) {
            latest_error = requirement.name + " has no valid entry for version " + version;
            continue;
        }
        std::vector<NativePkgRequirement> dependencies;
        const uint32_t deps_node = tape.get(entry, "dependencies");
        if (deps_node != 0 && !tape.is_object(deps_node)) {
            latest_error = requirement.name + "@" + version + " dependencies must be an object";
            continue;
        }
        if (deps_node != 0) {
            bool malformed = false;
            std::vector<std::pair<std::string, std::string>> sorted;
            uint32_t key = tape.at(deps_node).first_child;
            for (uint32_t i = 0; i < tape.at(deps_node).count; ++i) {
                if (key == 0 || !tape.is_string(key)) {
                    latest_error = requirement.name + "@" + version + " has malformed dependencies";
                    malformed = true;
                    break;
                }
                const uint32_t value = tape.at(key).next_sibling;
                if (value == 0 || !tape.is_string(value)) {
                    latest_error = requirement.name + "@" + version +
                                   " has an invalid dependency " + std::string(tape.str_view(key));
                    malformed = true;
                    break;
                }
                sorted.emplace_back(std::string(tape.str_view(key)),
                                    std::string(tape.str_view(value)));
                key = tape.at(value).next_sibling;
            }
            std::sort(sorted.begin(), sorted.end());
            bool invalid = malformed;
            for (const auto &[dep, constraint] : sorted) {
                if (!native_pkg_name(dep)) {
                    latest_error = requirement.name + "@" + version +
                                   " has an invalid dependency " + dep;
                    invalid = true;
                    break;
                }
                if (!native_pkg_constraint(constraint)) {
                    latest_error = requirement.name + "@" + version +
                                   " has an invalid dependency " + dep + "@" + constraint;
                    invalid = true;
                    break;
                }
                NativePkgRequirement next;
                next.name = dep;
                next.constraint = constraint;
                next.ancestry = requirement.ancestry;
                next.ancestry.push_back(requirement.name);
                dependencies.push_back(std::move(next));
            }
            if (invalid) continue;
        }
        auto next_chosen = chosen;
        next_chosen[requirement.name] = {requirement.name, version, entry};
        dependencies.insert(dependencies.end(), rest.begin(), rest.end());
        if (native_pkg_search(tape, packages, dependencies, next_chosen, combined,
                              result, latest_error, depth + 1)) return true;
    }
    return false;
}

static bool native_pkg_collect_plan(
        const lsp_wire::LspJsonTape &tape,
        const std::map<std::string, NativePkgSelection> &selected,
        const std::string &name, std::set<std::string> &completed,
        std::set<std::string> &active, std::map<std::string, std::string> &filenames,
        std::vector<NativePkgPlanItem> &plan, std::string &error,
        bool allow_file = true, bool block_internal_root = false) {
    if (completed.contains(name)) return true;
    if (active.contains(name)) { error = "dependency cycle: " + name; return false; }
    auto found = selected.find(name);
    if (found == selected.end()) { error = "unresolved package dependency " + name; return false; }
    active.insert(name);
    const uint32_t entry = found->second.entry;
    const uint32_t deps_node = tape.get(entry, "dependencies");
    std::vector<std::string> deps;
    if (deps_node != 0 && !tape.is_object(deps_node)) {
        error = name + "@" + found->second.version + " dependencies must be an object";
        return false;
    }
    if (deps_node != 0) {
        for (uint32_t key = tape.at(deps_node).first_child; key != 0;) {
            if (!tape.is_string(key)) { error = "malformed dependency name in " + name; return false; }
            deps.emplace_back(tape.str_view(key));
            const uint32_t value = tape.at(key).next_sibling;
            if (value == 0) { error = "malformed dependency in " + name; return false; }
            key = tape.at(value).next_sibling;
        }
        std::sort(deps.begin(), deps.end());
    }
    for (const std::string &dep : deps) {
        if (!native_pkg_collect_plan(tape, selected, dep, completed, active,
                                     filenames, plan, error, allow_file, block_internal_root)) return false;
    }
    const uint32_t url_node = tape.get(entry, "url");
    if (!tape.is_string(url_node) || tape.str_view(url_node).empty()) {
        error = name + "@" + found->second.version + " has no valid URL";
        return false;
    }
    const std::string url(tape.str_view(url_node));
    const uint32_t sha_node = tape.get(entry, "sha256");
    if (!tape.is_string(sha_node) || !native_lower_hex_sha(tape.str_view(sha_node))) {
        error = name + "@" + found->second.version + " has no valid SHA-256";
        return false;
    }
    const std::string expected(tape.str_view(sha_node));
    if (plan.size() >= pkg_io::kMaxIndexGraphPackages) {
        error = "package dependency graph exceeds 256 packages";
        return false;
    }
    std::string body;
    if (!native_fetch_package_url(url, body, error, allow_file, block_internal_root)) {
        error = "fetch " + url + ": " + error;
        return false;
    }
    size_t graph_bytes = body.size();
    for (const auto &existing : plan) graph_bytes += existing.text.size();
    if (graph_bytes > pkg_io::kMaxGraphArchiveBytes) {
        error = "package dependency graph exceeds 64 MiB total download budget";
        return false;
    }
    const std::string digest = sha256_hex(body);
    if (digest != expected) {
        error = "HASH MISMATCH " + name + "@" + found->second.version +
                " (index " + expected.substr(0, 12) + ", got " + digest.substr(0, 12) + ")";
        return false;
    }
    const size_t slash = name.rfind('/');
    const std::string filename = name.substr(slash + 1) + ".brat";
    std::string file_key = filename;
    std::transform(file_key.begin(), file_key.end(), file_key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    auto [other, inserted] = filenames.emplace(file_key, name);
    if (!inserted && other->second != name) {
        error = other->second + " and " + name + " both install as " + filename;
        return false;
    }
    plan.push_back({name, found->second.version, url, digest, filename, std::move(body)});
    active.erase(name);
    completed.insert(name);
    return true;
}

static bool native_ensure_directory(const std::filesystem::path &path,
                                    std::string &error) {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
        ec.clear();
        std::filesystem::create_directory(path, ec);
        if (!ec) status = std::filesystem::symlink_status(path, ec);
    }
    if (ec || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        error = "not a real directory: " + path.string();
        if (ec) error += ": " + ec.message();
        return false;
    }
    return true;
}

static bool native_pkg_lock_data(const lsp_wire::LspJsonTape &old_tape,
                                 bool old_exists,
                                 const std::vector<NativePkgPlanItem> &plan,
                                 std::string &data, std::string &error) {
    std::set<std::string> names, files;
    for (const auto &item : plan) {
        names.insert(item.name);
        files.insert(item.file);
    }
    std::vector<uint32_t> kept;
    if (old_exists) {
        for (uint32_t item = old_tape.at(old_tape.root()).first_child; item != 0;
             item = old_tape.at(item).next_sibling) {
            const uint32_t name = old_tape.get(item, "name");
            const uint32_t file = old_tape.get(item, "file");
            const bool replace_name = old_tape.is_string(name) &&
                                      names.contains(std::string(old_tape.str_view(name)));
            const bool replace_file = old_tape.is_string(file) &&
                                      files.contains(std::string(old_tape.str_view(file)));
            if (!replace_name && !replace_file) kept.push_back(item);
        }
    }
    lsp_wire::JsonWriter writer;
    writer.begin_arr();
    for (uint32_t item : kept) writer.raw_value(old_tape.raw_json_slice(item));
    for (const auto &item : plan) {
        writer.begin_obj();
        writer.key_lit("file"); writer.str(item.file);
        writer.key_lit("url"); writer.str(item.url);
        writer.key_lit("sha256"); writer.str(item.digest);
        if (!item.name.empty()) {
            writer.key_lit("name"); writer.str(item.name);
        }
        if (!item.version.empty()) {
            writer.key_lit("version"); writer.str(item.version);
        }
        writer.end_obj();
    }
    writer.end_arr();
    data.assign(writer.view());
    data.push_back('\n');
    if (data.size() > 16u * 1024u * 1024u) {
        error = "lockfile exceeds 16 MiB";
        return false;
    }
    lsp_wire::LspJsonTape check;
    if (!check.parse(data) || !native_validate_lock(check, error)) {
        if (error.empty()) error = "cannot validate new lockfile";
        return false;
    }
    return true;
}

static bool native_archive_lock_data(const lsp_wire::LspJsonTape &old_tape,
                                     bool old_exists,
                                     const std::vector<pkg_archive::ArchivePlanItem> &plan,
                                     std::string &data, std::string &error) {
    std::set<std::string> replacing;
    for (const auto &it : plan) {
        std::string low = it.archive;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        replacing.insert(std::move(low));
    }
    std::vector<uint32_t> kept;
    if (old_exists) {
        for (uint32_t item = old_tape.at(old_tape.root()).first_child; item != 0;
             item = old_tape.at(item).next_sibling) {
            const uint32_t an = old_tape.get(item, "archive");
            if (old_tape.is_string(an) && !old_tape.str_view(an).empty()) {
                std::string low(old_tape.str_view(an));
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (replacing.contains(low)) continue;
            }
            kept.push_back(item);
        }
    }
    lsp_wire::JsonWriter writer;
    writer.begin_arr();
    for (uint32_t item : kept) writer.raw_value(old_tape.raw_json_slice(item));
    for (const auto &it : plan) {
        writer.begin_obj();
        writer.key_lit("archive"); writer.str(it.archive);
        writer.key_lit("url"); writer.str(it.url);
        writer.key_lit("sha256"); writer.str(it.sha256);
        writer.key_lit("files");
        writer.begin_obj();
        for (const auto &[rel, hash] : it.files) {
            writer.key(rel);
            writer.str(hash);
        }
        writer.end_obj();
        if (!it.version.empty()) {
            writer.key_lit("version");
            writer.str(it.version);
        }
        if (!it.dependencies.empty()) {
            writer.key_lit("dependencies");
            writer.begin_arr();
            for (const auto &d : it.dependencies) writer.str(d);
            writer.end_arr();
        }
        if (!it.c_sources.empty()) {
            writer.key_lit("c_sources");
            writer.begin_arr();
            for (const auto &cs : it.c_sources) writer.str(cs);
            writer.end_arr();
        }
        writer.end_obj();
    }
    writer.end_arr();
    data.assign(writer.view());
    data.push_back('\n');
    if (data.size() > 16u * 1024u * 1024u) {
        error = "lockfile exceeds 16 MiB";
        return false;
    }
    lsp_wire::LspJsonTape check;
    if (!check.parse(data) || !native_validate_lock(check, error)) {
        if (error.empty()) error = "cannot validate new lockfile";
        return false;
    }
    return true;
}

static bool native_stage_vendor_tree(
        const std::vector<NativePkgPlanItem> &single_plan,
        const std::vector<pkg_archive::ArchivePlanItem> &archive_plan,
        const std::string *lock_data,
        std::string &error) {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path vendor = cwd / "vendor";
    if (!native_ensure_directory(vendor, error)) return false;
    std::filesystem::path stage;
    if (!native_create_stage_dir(vendor, ".brat-pkg-stage-", stage, error)) return false;
    const std::filesystem::path staged = stage / "staged";
    const std::filesystem::path backup = stage / "backup";
    std::error_code ec;
    if (!std::filesystem::create_directory(staged, ec) || ec ||
        !std::filesystem::create_directory(backup, ec) || ec) {
        error = "cannot create package staging tree";
        std::filesystem::remove_all(stage, ec);
        return false;
    }
    for (const auto &item : single_plan) {
        std::ofstream out(staged / item.file, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot stage " + item.file;
            std::filesystem::remove_all(stage, ec);
            return false;
        }
        out.write(item.text.data(), static_cast<std::streamsize>(item.text.size()));
        out.flush();
        if (!out) {
            error = "cannot stage " + item.file;
            out.close();
            std::filesystem::remove_all(stage, ec);
            return false;
        }
    }
    for (const auto &arc : archive_plan) {
        std::filesystem::create_directories(staged / arc.archive, ec);
        if (ec) {
            error = "cannot stage archive " + arc.archive + ": " + ec.message();
            std::filesystem::remove_all(stage, ec);
            return false;
        }
        for (const auto &[rel, content] : arc.extracted) {
            const auto dest = staged / std::filesystem::path(rel);
            std::filesystem::create_directories(dest.parent_path(), ec);
            if (ec) {
                error = "cannot stage " + rel + ": " + ec.message();
                std::filesystem::remove_all(stage, ec);
                return false;
            }
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "cannot stage " + rel;
                std::filesystem::remove_all(stage, ec);
                return false;
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out) {
                error = "cannot stage " + rel;
                out.close();
                std::filesystem::remove_all(stage, ec);
                return false;
            }
        }
    }

    std::vector<std::pair<std::string, bool>> targets;
    targets.reserve(single_plan.size() + archive_plan.size());
    for (const auto &item : single_plan) targets.emplace_back(item.file, false);
    for (const auto &arc : archive_plan) targets.emplace_back(arc.archive, true);

    std::vector<std::string> backed_up, installed;
    auto rollback = [&]() {
        for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
            std::error_code remove_error;
            std::filesystem::remove_all(vendor / *it, remove_error);
        }
        for (auto it = backed_up.rbegin(); it != backed_up.rend(); ++it) {
            std::error_code restore_error;
            std::filesystem::rename(backup / *it, vendor / *it, restore_error);
            if (restore_error) error += "; rollback failed: " + restore_error.message();
        }
    };
    for (const auto &[entry_name, want_dir] : targets) {
        const std::filesystem::path target = vendor / entry_name;
        const std::filesystem::path old = backup / entry_name;
        const auto status = std::filesystem::symlink_status(target, ec);
        const bool missing = ec == std::errc::no_such_file_or_directory ||
                             (!ec && status.type() == std::filesystem::file_type::not_found);
        if (ec && !missing) {
            error = "cannot inspect vendor package " + entry_name + ": " + ec.message();
            rollback();
            std::filesystem::remove_all(stage, ec);
            return false;
        }
        ec.clear();
        if (!missing) {
            if (std::filesystem::is_symlink(status) ||
                (!want_dir && !std::filesystem::is_regular_file(status)) ||
                (want_dir && !std::filesystem::is_directory(status))) {
                error = "refusing non-regular vendor package: " + entry_name;
                rollback();
                std::filesystem::remove_all(stage, ec);
                return false;
            }
            std::filesystem::rename(target, old, ec);
            if (ec) {
                error = "cannot stage existing vendor package " + entry_name + ": " + ec.message();
                rollback();
                std::filesystem::remove_all(stage, ec);
                return false;
            }
            backed_up.push_back(entry_name);
        }
        std::filesystem::rename(staged / entry_name, target, ec);
        if (ec) {
            error = "cannot install vendor package " + entry_name + ": " + ec.message();
            rollback();
            std::filesystem::remove_all(stage, ec);
            return false;
        }
        installed.push_back(entry_name);
    }
    if (lock_data && !native_atomic_write_lock(cwd / "brat.lock", *lock_data, error)) {
        rollback();
        std::filesystem::remove_all(stage, ec);
        return false;
    }
    std::filesystem::remove_all(stage, ec);
    return true;
}

static bool native_stage_pkg_files(const std::vector<NativePkgPlanItem> &plan,
                                   const std::string *lock_data,
                                   std::string &error) {
    return native_stage_vendor_tree(plan, {}, lock_data, error);
}

static int native_pkg_remove(const std::string &name) {
    std::string raw, error;
    bool exists = false;
    lsp_wire::LspJsonTape tape;
    if (!native_read_lock(raw, tape, exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint32_t> kept;
    uint32_t selected = 0;
    bool is_archive = false;
    if (exists) {
        for (uint32_t item = tape.at(tape.root()).first_child; item != 0;
             item = tape.at(item).next_sibling) {
            const uint32_t archive = tape.get(item, "archive");
            const uint32_t file = tape.get(item, "file");
            const bool item_is_archive = tape.is_string(archive) && !tape.str_view(archive).empty();
            const std::string_view item_name = item_is_archive ? tape.str_view(archive) : tape.str_view(file);
            if (item_name == name) {
                selected = item;
                is_archive = item_is_archive;
            } else kept.push_back(item);
        }
    }
    if (!selected) {
        std::fprintf(stderr, "brat pkg: not in lock: %s\n", name.c_str());
        return 1;
    }

    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path vendor = cwd / "vendor";
    std::error_code ec;
    const auto vendor_status = std::filesystem::symlink_status(vendor, ec);
    const bool vendor_missing = (ec == std::errc::no_such_file_or_directory) ||
        (!ec && vendor_status.type() == std::filesystem::file_type::not_found);
    if (ec && !vendor_missing) {
        std::fprintf(stderr, "brat pkg: remove failed: %s\n", ec.message().c_str());
        return 1;
    }
    ec.clear();
    if (!vendor_missing && (std::filesystem::is_symlink(vendor_status) ||
                            !std::filesystem::is_directory(vendor_status))) {
        std::fprintf(stderr, "brat pkg: remove failed: vendor path is not a real directory\n");
        return 1;
    }

    const std::filesystem::path target = vendor / name;
    bool target_exists = false;
    if (!vendor_missing) {
        const auto target_status = std::filesystem::symlink_status(target, ec);
        target_exists = !ec && target_status.type() != std::filesystem::file_type::not_found;
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        if (ec) {
            std::fprintf(stderr, "brat pkg: remove failed: %s\n", ec.message().c_str());
            return 1;
        }
        if (target_exists && (std::filesystem::is_symlink(target_status) ||
            (is_archive ? !std::filesystem::is_directory(target_status)
                        : !std::filesystem::is_regular_file(target_status)))) {
            std::fprintf(stderr, "brat pkg: remove failed: %s\n",
                         is_archive ? "vendor archive path is not a real directory"
                                    : "vendor package is not a regular file");
            return 1;
        }
    }
    if (!target_exists) {
        if (!native_write_lock_items(tape, kept, error)) {
            std::fprintf(stderr, "brat pkg: remove failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("removed %s\n", name.c_str());
        return 0;
    }

    std::filesystem::path stage;
    if (!native_create_stage_dir(vendor, ".brat-remove-", stage, error)) {
        std::fprintf(stderr, "brat pkg: remove failed: %s\n", error.c_str());
        return 1;
    }
    const std::filesystem::path backup = stage / name;
    std::filesystem::rename(target, backup, ec);
    if (ec) {
        const std::string move_error = ec.message();
        std::error_code cleanup_error;
        std::filesystem::remove_all(stage, cleanup_error);
        std::fprintf(stderr, "brat pkg: remove failed: cannot stage package: %s\n", move_error.c_str());
        return 1;
    }
    if (!native_write_lock_items(tape, kept, error)) {
        std::error_code rollback_error;
        std::filesystem::rename(backup, target, rollback_error);
        std::filesystem::remove_all(stage, ec);
        if (rollback_error) error += "; rollback failed: " + rollback_error.message();
        std::fprintf(stderr, "brat pkg: remove failed: %s\n", error.c_str());
        return 1;
    }
    std::filesystem::remove_all(backup, ec);
    if (ec) {
        std::fprintf(stderr, "brat pkg: remove failed: cannot remove staged package: %s\n", ec.message().c_str());
        return 1;
    }
    std::filesystem::remove(stage, ec);
    std::printf("removed %s\n", name.c_str());
    return 0;
}

static int native_pkg_add_archive(const std::string &spec, bool allow_file_root,
                                  const std::string &target) {
    std::string lock_raw, new_lock, error;
    bool lock_exists = false;
    lsp_wire::LspJsonTape lock_tape;
    if (!native_read_lock(lock_raw, lock_tape, lock_exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::vector<pkg_archive::ArchivePlanItem> plan;
    if (!pkg_archive::resolve_archive_graph(spec, allow_file_root, target, lock_tape,
                                            lock_exists, plan, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!native_archive_lock_data(lock_tape, lock_exists, plan, new_lock, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!native_stage_vendor_tree({}, plan, &new_lock, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    for (const auto &it : plan) {
        const std::string ver = it.version.empty() ? std::string() : ("@" + it.version);
        std::printf("add %s%s %.12s (%zu file(s))\n",
                    it.archive.c_str(), ver.c_str(), it.sha256.c_str(), it.files.size());
    }
    return 0;
}

static int native_pkg_add_url(const std::string &url, const std::string &target) {
    if (!native_safe_url(url)) {
        std::fprintf(stderr, "brat pkg: invalid URL\n");
        return 1;
    }
    if (pkg_archive::is_archive_url(url))
        return native_pkg_add_archive(url, true, target);
    const std::string filename = pkg_archive::url_unquoted_basename(url);
    if (!native_package_file(filename)) {
        std::fprintf(stderr, "brat pkg: URL must name a safe .brat file or .tar.gz archive\n");
        return 1;
    }
    std::string error, text;
    if (!native_fetch_package_url(url, text, error, true)) {
        std::fprintf(stderr, "brat pkg: fetch %s: %s\n", url.c_str(), error.c_str());
        return 1;
    }
    NativePkgPlanItem item;
    item.url = url;
    item.digest = sha256_hex(text);
    item.file = filename;
    item.text = std::move(text);
    std::vector<NativePkgPlanItem> plan{std::move(item)};
    std::string lock_raw, new_lock;
    bool lock_exists = false;
    lsp_wire::LspJsonTape lock_tape;
    if (!native_read_lock(lock_raw, lock_tape, lock_exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!native_pkg_lock_data(lock_tape, lock_exists, plan, new_lock, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!native_stage_pkg_files(plan, &new_lock, error)) {
        std::fprintf(stderr, "brat pkg: write %s\n", error.c_str());
        return 1;
    }
    std::printf("add %s %.12s\n", filename.c_str(), plan.front().digest.c_str());
    return 0;
}

static int native_pkg_add(const std::string &spec, const std::string &target) {
    if (spec.rfind("github.com/", 0) == 0)
        return native_pkg_add_archive(spec, false, target);
    if (spec.find("://") != std::string::npos)
        return native_pkg_add_url(spec, target);
    const size_t at = spec.rfind('@');
    const std::string name = at == std::string::npos ? spec : spec.substr(0, at);
    const std::string constraint = at == std::string::npos ? "*" : spec.substr(at + 1);
    if (spec.find('/') == std::string::npos) {
        std::fprintf(stderr, "brat pkg: %s is neither a URL nor owner/name\n", spec.c_str());
        return 1;
    }
    if (!native_pkg_name(name)) {
        std::fprintf(stderr, "brat pkg: invalid package name %s\n", name.c_str());
        return 1;
    }
    if (!native_pkg_constraint(constraint)) {
        std::fprintf(stderr, "brat pkg: invalid package version constraint %s\n",
                     constraint.c_str());
        return 1;
    }
    std::string error, index_raw, index_label;
    bool index_exists = false, index_remote = false, index_block_internal = false;
    lsp_wire::LspJsonTape index_tape;
    if (!native_read_pkg_index_any(index_raw, index_tape, index_exists, index_remote,
                                   index_label, error, &index_block_internal)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    const uint32_t packages = index_exists ? index_tape.get(index_tape.root(), "packages") : 0;
    if (packages == 0) {
        std::fprintf(stderr, "brat pkg: %s is not in %s\n", name.c_str(), index_label.c_str());
        return 1;
    }
    std::string lock_raw;
    bool lock_exists = false;
    lsp_wire::LspJsonTape lock_tape;
    if (!native_read_lock(lock_raw, lock_tape, lock_exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::map<std::string, std::vector<std::string>> constraints;
    std::string unpinned = name;
    std::transform(unpinned.begin(), unpinned.end(), unpinned.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lock_exists) {
        for (uint32_t item = lock_tape.at(lock_tape.root()).first_child; item != 0;
             item = lock_tape.at(item).next_sibling) {
            const uint32_t locked_name = lock_tape.get(item, "name");
            const uint32_t locked_version = lock_tape.get(item, "version");
            if (!lock_tape.is_string(locked_name) || !lock_tape.is_string(locked_version)) continue;
            std::string key(lock_tape.str_view(locked_name));
            std::string folded = key;
            std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (folded != unpinned)
                constraints[key].push_back("=" + std::string(lock_tape.str_view(locked_version)));
        }
    }
    NativePkgRequirement root{name, constraint, {}};
    std::map<std::string, NativePkgSelection> selected, resolved;
    std::string latest_error;
    if (!native_pkg_search(index_tape, packages, {root}, selected, constraints,
                           resolved, latest_error)) {
        std::fprintf(stderr, "brat pkg: %s\n",
                     latest_error.empty() ? "could not resolve package" : latest_error.c_str());
        return 1;
    }
    std::vector<NativePkgPlanItem> plan;
    std::set<std::string> completed, active;
    std::map<std::string, std::string> filenames;
    if (!native_pkg_collect_plan(index_tape, resolved, name, completed, active,
                                 filenames, plan, error, !index_remote, index_block_internal)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::string new_lock;
    if (!native_pkg_lock_data(lock_tape, lock_exists, plan, new_lock, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!native_stage_pkg_files(plan, &new_lock, error)) {
        std::fprintf(stderr, "brat pkg: install failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto &item : plan)
        std::printf("add %s %s %.12s\n", item.name.c_str(), item.version.c_str(),
                    item.digest.c_str());
    return 0;
}

static int native_pkg_install(const std::string &target) {
    std::string raw, error;
    bool exists = false;
    lsp_wire::LspJsonTape tape;
    if (!native_read_lock(raw, tape, exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    if (!exists || tape.at(tape.root()).count == 0) {
        std::fprintf(stderr, "brat pkg: empty lock\n");
        return 1;
    }
    std::vector<NativePkgPlanItem> single_plan;
    std::vector<pkg_archive::ArchivePlanItem> archive_plan;
    std::vector<std::string> ok_labels;
    for (uint32_t item = tape.at(tape.root()).first_child; item != 0;
         item = tape.at(item).next_sibling) {
        const uint32_t archive = tape.get(item, "archive");
        const uint32_t url_node = tape.get(item, "url");
        const uint32_t sha_node = tape.get(item, "sha256");
        const std::string url(tape.str_view(url_node));
        if (tape.is_string(archive) && !tape.str_view(archive).empty()) {
            const std::string pkg_name(tape.str_view(archive));
            std::string raw_bytes;
            if (!native_fetch_raw_url(url, raw_bytes, error, true)) {
                std::fprintf(stderr, "brat pkg: fetch %s: %s\n", url.c_str(), error.c_str());
                return 1;
            }
            const std::string digest = sha256_hex(raw_bytes);
            if (digest != tape.str_view(sha_node)) {
                std::fprintf(stderr, "brat pkg: HASH MISMATCH %s\n", pkg_name.c_str());
                return 1;
            }
            std::map<std::string, std::string> extracted, file_hashes;
            pkg_archive::ArchiveManifest manifest;
            if (!pkg_archive::extract_tar_package(raw_bytes, pkg_name, target,
                                                  extracted, file_hashes, manifest, error)) {
                std::fprintf(stderr, "brat pkg: extract %s: %s\n", pkg_name.c_str(), error.c_str());
                return 1;
            }
            std::map<std::string, std::string> locked_hashes;
            const uint32_t files_node = tape.get(item, "files");
            for (uint32_t k = tape.at(files_node).first_child; k != 0;) {
                const uint32_t v = tape.at(k).next_sibling;
                if (v == 0) break;
                locked_hashes.emplace(std::string(tape.str_view(k)), std::string(tape.str_view(v)));
                k = tape.at(v).next_sibling;
            }
            if (file_hashes != locked_hashes) {
                std::fprintf(stderr, "brat pkg: HASH MISMATCH %s\n", pkg_name.c_str());
                return 1;
            }
            pkg_archive::ArchivePlanItem entry;
            entry.archive = pkg_name;
            entry.extracted = std::move(extracted);
            archive_plan.push_back(std::move(entry));
            ok_labels.push_back(pkg_name);
            continue;
        }
        const uint32_t file_node = tape.get(item, "file");
        const std::string filename(tape.str_view(file_node));
        std::string body;
        if (!native_fetch_package_url(url, body, error, true)) {
            std::fprintf(stderr, "brat pkg: fetch %s: %s\n", url.c_str(), error.c_str());
            return 1;
        }
        const std::string digest = sha256_hex(body);
        if (digest != tape.str_view(sha_node)) {
            std::fprintf(stderr, "brat pkg: HASH MISMATCH %s\n", filename.c_str());
            return 1;
        }
        NativePkgPlanItem entry;
        entry.name = tape.is_string(tape.get(item, "name"))
            ? std::string(tape.str_view(tape.get(item, "name"))) : std::string();
        entry.version = tape.is_string(tape.get(item, "version"))
            ? std::string(tape.str_view(tape.get(item, "version"))) : std::string();
        entry.url = url;
        entry.digest = digest;
        entry.file = filename;
        entry.text = std::move(body);
        single_plan.push_back(std::move(entry));
        ok_labels.push_back(filename);
    }
    if (!native_stage_vendor_tree(single_plan, archive_plan, nullptr, error)) {
        std::fprintf(stderr, "brat pkg: install failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto &label : ok_labels) std::printf("ok %s\n", label.c_str());
    return 0;
}

static int native_pkg_update(int argc, char **argv) {
    bool dry_run = false;
    std::vector<std::string> targets;
    std::vector<std::string> options;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!arg.empty() && arg[0] == '-') options.push_back(arg);
        else targets.push_back(arg);
    }
    if (options.size() > 1 || (!options.empty() && options.front() != "--dry-run") ||
        targets.size() > 1) {
        std::fprintf(stderr, "usage: brat-cxx pkg update [NAME] [--dry-run]\n");
        return 2;
    }
    dry_run = !options.empty();
    std::string target;
    if (!targets.empty()) {
        target = targets.front();
        if (!native_pkg_name(target)) {
            std::fprintf(stderr, "brat pkg: invalid package name %s\n", target.c_str());
            return 1;
        }
    }
    std::string lock_raw, error;
    bool lock_exists = false;
    lsp_wire::LspJsonTape lock_tape;
    if (!native_read_lock(lock_raw, lock_tape, lock_exists, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> locked_names;
    std::map<std::string, uint32_t> locked_by_name;
    if (lock_exists) {
        for (uint32_t item = lock_tape.at(lock_tape.root()).first_child; item != 0;
             item = lock_tape.at(item).next_sibling) {
            const uint32_t name_node = lock_tape.get(item, "name");
            if (!lock_tape.is_string(name_node)) continue;
            const std::string name(lock_tape.str_view(name_node));
            locked_names.push_back(name);
            locked_by_name[name] = item;
        }
    }
    std::sort(locked_names.begin(), locked_names.end());
    locked_names.erase(std::unique(locked_names.begin(), locked_names.end()), locked_names.end());
    if (locked_names.empty()) {
        std::fprintf(stderr, "brat pkg: no indexed packages in lock\n");
        return 1;
    }
    const std::vector<std::string> roots = locked_names;
    std::set<std::string> unpinned;
    if (!target.empty() && !locked_by_name.contains(target)) {
        std::fprintf(stderr, "brat pkg: not in lock: %s\n", target.c_str());
        return 1;
    }

    std::string index_raw, index_label;
    bool index_exists = false, index_remote = false, index_block_internal = false;
    lsp_wire::LspJsonTape index_tape;
    if (!native_read_pkg_index_any(index_raw, index_tape, index_exists, index_remote,
                                   index_label, error, &index_block_internal)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    const uint32_t packages = index_exists ? index_tape.get(index_tape.root(), "packages") : 0;
    if (!index_exists || !index_tape.is_object(packages)) {
        std::fprintf(stderr, "brat pkg: package index has no packages object\n");
        return 1;
    }
    if (target.empty()) unpinned.insert(locked_names.begin(), locked_names.end());
    else {
        auto visit = [&](auto &&self, const std::string &current) -> void {
            if (!unpinned.insert(current).second) return;
            auto locked = locked_by_name.find(current);
            if (locked == locked_by_name.end() || packages == 0) return;
            const uint32_t version_node = lock_tape.get(locked->second, "version");
            const uint32_t versions = index_tape.get(packages, current);
            const uint32_t entry = lock_tape.is_string(version_node) && index_tape.is_object(versions)
                ? index_tape.get(versions, lock_tape.str_view(version_node)) : 0;
            const uint32_t deps = entry == 0 ? 0 : index_tape.get(entry, "dependencies");
            if (!index_tape.is_object(deps)) return;
            for (uint32_t key = index_tape.at(deps).first_child; key != 0;) {
                if (!index_tape.is_string(key)) break;
                const std::string dependency(index_tape.str_view(key));
                const uint32_t value = index_tape.at(key).next_sibling;
                if (value == 0) break;
                self(self, dependency);
                key = index_tape.at(value).next_sibling;
            }
        };
        visit(visit, target);
    }
    std::map<std::string, std::vector<std::string>> constraints;
    for (const std::string &name : locked_names) {
        if (unpinned.contains(name)) continue;
        const uint32_t item = locked_by_name[name];
        const uint32_t version = lock_tape.get(item, "version");
        if (lock_tape.is_string(version))
            constraints[name].push_back("=" + std::string(lock_tape.str_view(version)));
    }
    std::vector<NativePkgRequirement> pending;
    for (const std::string &root : roots) pending.push_back({root, "*", {}});
    std::map<std::string, NativePkgSelection> resolved, chosen;
    std::string latest_error;
    if (!native_pkg_search(index_tape, packages, pending, chosen, constraints,
                           resolved, latest_error)) {
        std::fprintf(stderr, "brat pkg: %s\n",
                     latest_error.empty() ? "could not resolve packages" : latest_error.c_str());
        return 1;
    }
    std::vector<NativePkgPlanItem> plan;
    std::set<std::string> completed, active;
    std::map<std::string, std::string> filenames;
    for (const std::string &root : roots) {
        if (!native_pkg_collect_plan(index_tape, resolved, root, completed, active,
                                     filenames, plan, error, !index_remote, index_block_internal)) {
            std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
            return 1;
        }
    }
    std::vector<std::pair<const NativePkgPlanItem *, std::string>> changes;
    for (const auto &item : plan) {
        auto old = locked_by_name.find(item.name);
        if (old == locked_by_name.end()) {
            changes.emplace_back(&item, "not locked");
            continue;
        }
        const uint32_t version_node = lock_tape.get(old->second, "version");
        const uint32_t sha_node = lock_tape.get(old->second, "sha256");
        const std::string old_version = std::string(lock_tape.str_view(version_node));
        const std::string old_sha = std::string(lock_tape.str_view(sha_node));
        if (old_version != item.version || old_sha != item.digest)
            changes.emplace_back(&item, old_version + " " + old_sha.substr(0, 12));
    }
    if (dry_run) {
        if (changes.empty()) std::puts("all packages are up to date");
        for (const auto &[item, old] : changes)
            std::printf("would update %s %s -> %s %.12s\n", item->name.c_str(), old.c_str(),
                        item->version.c_str(), item->digest.c_str());
        return 0;
    }
    std::string new_lock;
    if (!native_pkg_lock_data(lock_tape, lock_exists, plan, new_lock, error)) {
        std::fprintf(stderr, "brat pkg: update failed: %s\n", error.c_str());
        return 1;
    }
    if (!native_stage_pkg_files(plan, &new_lock, error)) {
        std::fprintf(stderr, "brat pkg: update failed: %s\n", error.c_str());
        return 1;
    }
    if (changes.empty()) std::puts("all packages are up to date");
    for (const auto &[item, old] : changes)
        std::printf("updated %s %s -> %s %.12s\n", item->name.c_str(), old.c_str(),
                    item->version.c_str(), item->digest.c_str());
    return 0;
}

static int native_pkg_publish(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: brat-cxx pkg publish OWNER/NAME@VERSION SOURCE [--depends OWNER/NAME@CONSTRAINT]...\n");
        return 2;
    }
    const std::string spec = argv[3];
    const std::string source = argv[4];
    const size_t at = spec.rfind('@');
    if (at == std::string::npos) {
        std::fprintf(stderr, "brat pkg: publish requires owner/name@version\n");
        return 1;
    }
    const std::string package = spec.substr(0, at);
    const std::string version = spec.substr(at + 1);
    if (!native_pkg_name(package) || !native_pkg_version(version)) {
        std::fprintf(stderr, "brat pkg: invalid package name or version %s\n", spec.c_str());
        return 1;
    }
    const size_t slash = package.find('/');
    const std::string owner = package.substr(0, slash);
    const std::string name = package.substr(slash + 1);
    std::vector<std::pair<std::string, std::string>> dependencies;
    std::map<std::string, std::string> seen_dependencies;
    for (int i = 5; i < argc;) {
        if (std::string_view(argv[i]) != "--depends" || i + 1 >= argc) {
            std::fprintf(stderr, "brat pkg: expected --depends owner/name@constraint\n");
            return 1;
        }
        const std::string dep = argv[i + 1];
        const size_t dep_at = dep.rfind('@');
        if (dep_at == std::string::npos) {
            std::fprintf(stderr, "brat pkg: dependency needs owner/name@constraint: %s\n",
                         dep.c_str());
            return 1;
        }
        const std::string dep_name = dep.substr(0, dep_at);
        const std::string constraint = dep.substr(dep_at + 1);
        if (!native_pkg_name(dep_name) || !native_pkg_constraint(constraint) ||
            dep_name == package) {
            std::fprintf(stderr, "brat pkg: invalid dependency %s\n", dep.c_str());
            return 1;
        }
        auto previous = seen_dependencies.find(dep_name);
        if (previous != seen_dependencies.end()) {
            if (previous->second != constraint) {
                std::fprintf(stderr, "brat pkg: conflicting versions for %s\n", dep_name.c_str());
                return 1;
            }
        } else {
            seen_dependencies.emplace(dep_name, constraint);
            dependencies.emplace_back(dep_name, constraint);
        }
        i += 2;
    }

    std::string error, body;
    if (!native_read_limited(std::filesystem::path(source), 16u * 1024u * 1024u,
                             body, error)) {
        std::fprintf(stderr, "brat pkg: cannot publish %s: %s\n", source.c_str(), error.c_str());
        return 1;
    }
    try {
        (void)parse(body);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "brat pkg: cannot publish %s: %s\n", source.c_str(), e.what());
        return 1;
    }
    const std::string digest = sha256_hex(body);

    std::filesystem::path index_path;
    if (!native_index_path(index_path, error)) {
        std::fprintf(stderr, "brat pkg: %s\n", error.c_str());
        return 1;
    }
    std::string raw;
    bool index_exists = false;
    lsp_wire::LspJsonTape tape;
    if (!native_read_pkg_index(index_path, raw, tape, index_exists, error)) {
        std::fprintf(stderr, "brat pkg: publish failed: brat pkg: %s\n", error.c_str());
        return 1;
    }
    uint32_t packages_node = 0;
    if (index_exists && !native_object_member(tape, tape.root(), "packages",
                                               packages_node, error)) {
        std::fprintf(stderr, "brat pkg: publish failed: brat pkg: %s\n", error.c_str());
        return 1;
    }
    uint32_t package_node = 0;
    if (index_exists && !native_object_member(tape, packages_node, package,
                                               package_node, error)) {
        std::fprintf(stderr, "brat pkg: publish failed: brat pkg: %s\n", error.c_str());
        return 1;
    }
    uint32_t existing_version = 0;
    if (package_node != 0) {
        if (!tape.is_object(package_node)) {
            std::fprintf(stderr, "brat pkg: publish failed: package versions entry is not an object\n");
            return 1;
        }
        if (!native_object_member(tape, package_node, version, existing_version, error)) {
            std::fprintf(stderr, "brat pkg: publish failed: brat pkg: %s\n", error.c_str());
            return 1;
        }
    }
    (void)existing_version;

    const std::filesystem::path artifact_root = index_path.parent_path() / ".brat-packages";
    const std::filesystem::path owner_dir = artifact_root / owner;
    const std::filesystem::path artifact = owner_dir /
        (name + "-" + version + "-" + digest.substr(0, 16) + ".brat");
    const std::string url = native_file_uri(artifact);

    std::string index_data = "{\n  \"packages\": {\n";
    bool first_package = true;
    bool package_found = false;
    if (index_exists) {
        uint32_t key = tape.at(packages_node).first_child;
        for (uint32_t i = 0; i < tape.at(packages_node).count; ++i) {
            if (key == 0 || !tape.is_string(key)) {
                std::fprintf(stderr, "brat pkg: publish failed: index packages object is malformed\n");
                return 1;
            }
            const uint32_t value = tape.at(key).next_sibling;
            if (value == 0) {
                std::fprintf(stderr, "brat pkg: publish failed: index packages object is malformed\n");
                return 1;
            }
            const std::string_view package_key = tape.str_view(key);
            if (!first_package) index_data += ",\n";
            first_package = false;
            native_json_indent(index_data, 2);
            lsp_wire::JsonWriter::write_escaped_string(index_data, package_key);
            index_data += ": ";
            if (package_key == package) {
                package_found = true;
                if (!native_json_write_versions(tape, value, version, url, digest,
                                                dependencies, index_data, 2, error)) {
                    std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
                    return 1;
                }
            } else if (!native_json_pretty(tape, value, index_data, 2, error)) {
                std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
                return 1;
            }
            key = tape.at(value).next_sibling;
        }
    }
    if (!package_found) {
        if (!first_package) index_data += ",\n";
        native_json_indent(index_data, 2);
        lsp_wire::JsonWriter::write_escaped_string(index_data, package);
        index_data += ": ";
        if (!native_json_write_versions(tape, 0, version, url, digest,
                                        dependencies, index_data, 2, error)) {
            std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
            return 1;
        }
    }
    index_data += "\n  }\n}\n";
    if (index_data.size() > 16u * 1024u * 1024u) {
        std::fprintf(stderr, "brat pkg: publish failed: index exceeds 16 MiB\n");
        return 1;
    }

    auto ensure_directory = [&](const std::filesystem::path &path) {
        std::error_code ec;
        auto status = std::filesystem::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && status.type() == std::filesystem::file_type::not_found)) {
            ec.clear();
            if (!std::filesystem::create_directory(path, ec) && !ec) {
                status = std::filesystem::symlink_status(path, ec);
            } else if (!ec) {
                status = std::filesystem::symlink_status(path, ec);
            }
        }
        if (ec || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            error = "not a real directory: " + path.string();
            if (ec) error += ": " + ec.message();
            return false;
        }
        return true;
    };
    if (!ensure_directory(artifact_root) || !ensure_directory(owner_dir)) {
        std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
        return 1;
    }
    std::error_code ec;
    const auto artifact_status = std::filesystem::symlink_status(artifact, ec);
    const bool artifact_existed = !ec && artifact_status.type() != std::filesystem::file_type::not_found;
    if (ec == std::errc::no_such_file_or_directory) ec.clear();
    if (ec) {
        std::fprintf(stderr, "brat pkg: publish failed: %s\n", ec.message().c_str());
        return 1;
    }
    if (!native_atomic_write_data(artifact, body, error)) {
        std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
        return 1;
    }
    if (!native_atomic_write_data(index_path, index_data, error)) {
        if (!artifact_existed) {
            std::error_code cleanup_error;
            std::filesystem::remove(artifact, cleanup_error);
        }
        std::fprintf(stderr, "brat pkg: publish failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("publish %s %s %.12s\n", package.c_str(), version.c_str(), digest.c_str());
    return 0;
}

static bool parse_pkg_target_args(int argc, char **argv, int start,
                                  std::string &target_out,
                                  std::vector<std::string> &pos_out) {
    target_out.clear();
    pos_out.clear();
    auto safe_triple = [](std::string_view t) {
        if (t.empty() || t.size() > 128 || t.front() == '-') return false;
        for (char c : t) {
            const auto u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '-' || c == '_' || c == '.')) return false;
        }
        return true;
    };
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--target") {
            if (i + 1 >= argc || !safe_triple(argv[i + 1])) return false;
            target_out = argv[++i];
        } else if (arg.rfind("--target=", 0) == 0) {
            const std::string val = arg.substr(9);
            if (!safe_triple(val)) return false;
            target_out = val;
        } else if (!arg.empty() && arg[0] == '-') {
            return false;
        } else {
            pos_out.push_back(arg);
        }
    }
    return true;
}

static int cmd_pkg(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: brat-cxx pkg {add|install|update|verify|list|remove|publish} [ARGS]\n");
        return 2;
    }
    const std::string sub = argv[2];
    if (sub == "publish") return native_pkg_publish(argc, argv);
    if (sub == "update") return native_pkg_update(argc, argv);
    if (sub == "add") {
        std::string target;
        std::vector<std::string> pos;
        if (!parse_pkg_target_args(argc, argv, 3, target, pos) || pos.size() != 1) {
            std::fprintf(stderr, "usage: brat-cxx pkg add <SPEC|URL> [--target TRIPLE]\n");
            return 2;
        }
        return native_pkg_add(pos[0], target);
    }
    if (sub == "install") {
        std::string target;
        std::vector<std::string> pos;
        if (!parse_pkg_target_args(argc, argv, 3, target, pos) || !pos.empty()) {
            std::fprintf(stderr, "usage: brat-cxx pkg install [--target TRIPLE]\n");
            return 2;
        }
        return native_pkg_install(target);
    }
    if (sub == "list") {
        if (argc != 3) {
            std::fprintf(stderr, "usage: brat-cxx pkg list\n");
            return 2;
        }
        return native_pkg_list();
    }
    if (sub == "remove") {
        if (argc != 4) {
            std::fprintf(stderr, "usage: brat-cxx pkg remove NAME\n");
            return 2;
        }
        return native_pkg_remove(argv[3]);
    }
    if (sub != "verify") {
        std::fprintf(stderr, "brat-cxx pkg: native subcommand not implemented: %s\n", sub.c_str());
        return 2;
    }
    std::string target;
    std::vector<std::string> pos;
    if (!parse_pkg_target_args(argc, argv, 3, target, pos) || !pos.empty()) {
        std::fprintf(stderr, "usage: brat-cxx pkg verify [--target TRIPLE]\n");
        return 2;
    }
    return native_pkg_verify(target);
}

static int cmd_dist(int argc, char **argv);
static int cmd_ffi_native(int argc, char **argv);

int brat_cli_main(int argc, char **argv) {
    if (argc >= 1 && argv[0]) {
        const auto std_dir = executable_path(argv[0]).parent_path().parent_path().parent_path() / "std";
        set_stdlib_root(std_dir.string());
    }
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_cli_usage();
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
                print_cli_usage();
                return 0;
            }
            if (argc != 3) {
                std::fprintf(stderr, "usage: brat-cxx bundle OUTPUT_DIR\n");
                return 2;
            }
            return bundle_driver(argv[0], argv[2]);
        }
        if (c1 == "lsp") {
            if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
                print_cli_usage();
                return 0;
            }
            if (argc != 2) {
                std::fprintf(stderr, "usage: brat-cxx lsp\n");
                return 2;
            }
            return run_lsp();
        }
        if (c1 == "mcp") return cmd_mcp(argc, argv);
        if (c1 == "dist") return cmd_dist(argc, argv);
        if (c1 == "ffi") return cmd_ffi_native(argc, argv);
        if (c1 == "bindgen") {
            if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
                print_cli_usage();
                return 0;
            }
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
    if (argc < 3) {
        print_cli_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const bool native = cmd == "ast" || cmd == "lex" || cmd == "check" || cmd == "flat" ||
                        cmd == "cc" || cmd == "libs" || cmd == "build" || cmd == "run";
    if (!native) return python_cli(argc, argv);
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) break;
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_cli_usage();
            return 0;
        }
    }
    if (cmd == "run") {
        // `--interp` belongs to the Python reference interpreter; native `--header` / `--object` run here!
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--") == 0) break;
            if (std::strcmp(argv[i], "--interp") == 0)
                return python_cli(argc, argv);
        }
        std::string file, ffi_header;
        std::vector<std::string> ffi_objects;
        uint64_t memory_limit = 0;
        int opt = 0;
        bool unsafe_ffi = false;
        const char *tev = std::getenv("BRAT_TRACE");
        bool trace_lines = (tev && tev[0] && tev[0] != '0');
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
            else if (arg == "--unsafe-ffi") unsafe_ffi = true;
            else if (arg == "--header" && i + 1 < argc) ffi_header = argv[++i];
            else if (arg.rfind("--header=", 0) == 0) ffi_header = arg.substr(9);
            else if (arg == "--object" && i + 1 < argc) ffi_objects.push_back(argv[++i]);
            else if (arg.rfind("--object=", 0) == 0) ffi_objects.push_back(arg.substr(9));
            else if (arg == "--trace-lines") trace_lines = true;
            else if (arg == "--no-check") continue;
            else if (arg == "--mem" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (!parse_memory(value, memory_limit)) {
                    std::fprintf(stderr, "brat run: bad --mem '%s'\n", value.c_str());
                    return 2;
                }
            } else if (!arg.empty() && arg[0] == '-') {
                std::fprintf(stderr, "brat-cxx run: unknown flag %s\n", arg.c_str());
                return 2;
            } else if (file.empty()) file = arg;
            else program_args.push_back(arg);
        }
        if (file.empty()) {
            std::fprintf(stderr, "brat-cxx run: needs one source file\n");
            return 2;
        }
        std::vector<std::string> keep;
        keep.push_back(file);
        for (const auto &a : program_args) keep.push_back(a);
        std::vector<char *> pass;
        for (auto &k : keep) pass.push_back(const_cast<char *>(k.c_str()));
        pass.push_back(nullptr);
        try {
            return run_one(argv[0], file, opt, memory_limit, pass.data(), unsafe_ffi, trace_lines, ffi_header, ffi_objects);
        } catch (const ParseError &e) {
            print_parse_error(file, e);
            return 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "brat-cxx run: %s\n", e.what());
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
        const char *btev = std::getenv("BRAT_TRACE");
        bool trace_lines = (btev && btev[0] && btev[0] != '0');
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
            else if (arg == "--unsafe-ffi") unsafe_ffi = true;
            else if (arg == "--header" && i + 1 < argc) ffi_header = argv[++i];
            else if (arg.rfind("--header=", 0) == 0) ffi_header = arg.substr(9);
            else if (arg == "--object" && i + 1 < argc) ffi_objects.push_back(argv[++i]);
            else if (arg.rfind("--object=", 0) == 0) ffi_objects.push_back(arg.substr(9));
            else if (arg == "--trace-lines") trace_lines = true;
            else if (arg == "--no-check") continue;
            else if (!arg.empty() && arg[0] == '-') {
                std::fprintf(stderr, "brat-cxx build: unknown flag %s\n", arg.c_str());
                return 2;
            } else if (file.empty()) file = arg;
            else {
                std::fprintf(stderr, "brat-cxx build: expected exactly one source file\n");
                return 2;
            }
        }
        if (file.empty()) {
            std::fprintf(stderr, "brat-cxx build: needs one source file\n");
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
                             unsafe_ffi, trace_lines, ffi_header, ffi_objects);
        } catch (const ParseError &e) {
            print_parse_error(file, e);
            return 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "brat-cxx build: %s\n", e.what());
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
                flatten(set);
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
                    std::string text =
                        codegen::emit_program(set.flat, dir_of(file), flags.opt, &proofs, "", flags.trace_lines);
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

    if (cmd == "check") {
        bool want_json = false;
        std::vector<std::string> check_files;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--py") return python_cli(argc, argv);
            if (arg == "--json") { want_json = true; continue; }
            if (arg == "--") continue;
            if (!arg.empty() && arg[0] == '-' && arg != "-") {
                std::fprintf(stderr, "brat-cxx check: unknown flag %s\n", arg.c_str());
                return 2;
            }
            check_files.push_back(arg);
        }
        if (want_json) return cmd_check_json(check_files);
    }

    std::vector<SourceFile> progs;
    for (int i = 2; i < argc; i++) {
        try {
            ModuleSet loaded = load_modules(argv[i]);
            mangle_modules(loaded);
            for (auto &f : loaded.files) progs.push_back(std::move(f));
        } catch (const ParseError &e) {
            print_parse_error(argv[i], e);
            bad++;
        } catch (const std::exception &e) {
            std::printf("%s: load error %s\n", argv[i], e.what());
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
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            FnTable ps = pub_sigs(progs[j].body, exported_fn_names(progs[j]));
            for (auto &kv : ps.sigs) extra[kv.first] = kv.second;
            extra_consts.insert(ps.pub_consts.begin(), ps.pub_consts.end());
            extra_scalar.insert(ps.scalar_ret.begin(), ps.scalar_ret.end());
            for (auto &kv : ps.borrows_ret) extra_borrows[kv.first] = kv.second;
            extra_owned.insert(ps.owned_ret.begin(), ps.owned_ret.end());
        }
        std::set<std::string> module_heads = imported_module_heads(progs[i]);
        BorrowOut out = borrow_check(progs[i].body, extra, progs[i].stray, module_heads,
                                     extra_consts, extra_scalar, extra_borrows, extra_owned);
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

static int cmd_dist(int argc, char **argv) {
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
            flatten(set);
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

static int cmd_ffi_native(int argc, char **argv) {
    if (argc == 3 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
        print_cli_usage();
        return 0;
    }
    return native_ffi::cmd_ffi(argc - 2, argv + 2);
}

int main(int argc, char **argv) {
    return brat_cli_main(argc, argv);
}
