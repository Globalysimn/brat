#include "ffi_cdecl.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <thread>
#include <windows.h>
#ifdef cdecl
#undef cdecl
#endif
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace native_ffi {
namespace {

namespace fs = std::filesystem;
constexpr size_t kMaxHeaderBytes = 16 * 1024 * 1024;

static std::string trim_sv(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

static bool is_ident0(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
static bool is_identc(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
static bool is_valid_ident(std::string_view s) {
    if (s.empty() || !is_ident0(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (!is_identc(s[i])) return false;
    return true;
}

static std::vector<std::string> ident_words(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        if (is_ident0(s[i])) {
            size_t j = i + 1;
            while (j < s.size() && is_identc(s[j])) ++j;
            out.emplace_back(s.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

static std::string strip_comments(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i];
            out.push_back(src[i++]);
            while (i < src.size()) {
                char c = src[i++];
                out.push_back(c);
                if (c == '\\' && i < src.size()) out.push_back(src[i++]);
                else if (c == q) break;
            }
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') out.push_back('\n');
                ++i;
            }
            if (i + 1 < src.size()) i += 2;
            out.push_back(' ');
        } else if (i + 1 < src.size() && src[i] == '\\' && src[i + 1] == '\n') {
            i += 2;
            out.push_back(' ');
        } else if (i + 2 < src.size() && src[i] == '\\' && src[i + 1] == '\r' && src[i + 2] == '\n') {
            i += 3;
            out.push_back(' ');
        } else if (src[i] == '\r') {
            ++i;
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

static std::string strip_include_guard(const std::string &src) {
    std::vector<std::string> lines;
    std::istringstream iss(src);
    std::string ln;
    while (std::getline(iss, ln)) lines.push_back(ln);

    size_t first = 0;
    while (first < lines.size() && trim_sv(lines[first]).empty()) ++first;
    if (first + 1 >= lines.size()) return src;

    std::string l0 = trim_sv(lines[first]);
    std::string l1 = trim_sv(lines[first + 1]);
    std::string guard;
    if (l0.rfind("#ifndef", 0) == 0 && std::isspace(static_cast<unsigned char>(l0[7]))) {
        guard = trim_sv(std::string_view(l0).substr(7));
    }
    if (!guard.empty() && is_valid_ident(guard) && l1.rfind("#define", 0) == 0) {
        auto rest = trim_sv(std::string_view(l1).substr(7));
        if (rest == guard || (rest.rfind(guard, 0) == 0 && std::isspace(static_cast<unsigned char>(rest[guard.size()])))) {
            size_t last = lines.size();
            while (last > first + 2 && trim_sv(lines[last - 1]).empty()) --last;
            if (last > first + 2 && trim_sv(lines[last - 1]).rfind("#endif", 0) == 0) {
                lines[first].clear();
                lines[first + 1].clear();
                lines[last - 1].clear();
            }
        }
    }
    std::string out;
    for (const auto &l : lines) {
        out += l;
        out.push_back('\n');
    }
    return out;
}

static bool read_file_bounded(const fs::path &p, std::string &out, std::string &err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "cannot read header: " + p.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0 || static_cast<size_t>(sz) > kMaxHeaderBytes) {
        err = "header too large: " + p.string();
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return true;
}

static bool read_header_rec(const fs::path &p, std::set<std::string> &seen, std::string &out, std::string &err) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    std::string key = ec ? p.lexically_normal().string() : canon.string();
    if (!seen.insert(key).second) return true;
    std::string raw;
    if (!read_file_bounded(p, raw, err)) return false;
    std::string clean = strip_include_guard(strip_comments(raw));
    fs::path dir = p.parent_path();

    std::istringstream iss(clean);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim_sv(line);
        if (!t.empty() && t[0] == '#') {
            std::string body = trim_sv(std::string_view(t).substr(1));
            if (body.rfind("include", 0) == 0) {
                std::string arg = trim_sv(std::string_view(body).substr(7));
                if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
                    std::string rel = arg.substr(1, arg.size() - 2);
                    fs::path sub = dir / rel;
                    if (fs::is_regular_file(sub, ec)) {
                        std::string sub_txt;
                        if (!read_header_rec(sub, seen, sub_txt, err)) return false;
                        if (out.size() + sub_txt.size() + 1u > 4u * kMaxHeaderBytes) {
                            err = "inlined header exceeds 64 MiB limit: " + p.string();
                            return false;
                        }
                        out += sub_txt;
                        out.push_back('\n');
                        continue;
                    }
                }
            }
        }
        out += line;
        out.push_back('\n');
    }
    return true;
}

}  // namespace

TargetModel make_target_model(std::string_view triple) {
    TargetModel m;
    m.triple = std::string(triple);
    if (triple.empty()) {
        m.ptr = static_cast<int>(sizeof(void *));
        m.lng = static_cast<int>(sizeof(long));
        struct WideProbe { char c; int64_t x; };
        m.wide_align = static_cast<int>(offsetof(WideProbe, x));
        m.model = (m.ptr == 4) ? "ilp32" : (m.lng == 4 ? "llp64" : "lp64");
        return m;
    }
    std::string low(triple);
    for (char &c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](std::string_view sub) { return low.find(sub) != std::string::npos; };
    bool is32 = has("i386") || has("i486") || has("i586") || has("i686") || has("wasm32") ||
                has("riscv32") || has("armv7") || has("arm-") || low.rfind("arm", 0) == 0;
    if (has("aarch64") || has("arm64")) is32 = false;
    if (is32) {
        m.ptr = 4;
        m.lng = 4;
        m.model = "ilp32";
        m.wide_align = (has("i386") || has("i486") || has("i586") || has("i686")) && !has("darwin") ? 4 : 8;
        return m;
    }
    m.ptr = 8;
    m.wide_align = 8;
    if (has("windows") || has("msvc") || has("mingw") || has("win32")) {
        m.lng = 4;
        m.model = "llp64";
    } else {
        m.lng = 8;
        m.model = "lp64";
    }
    return m;
}

bool read_header_inlined(const std::string &path, std::string &out, std::string &err) {
    out.clear();
    std::set<std::string> seen;
    return read_header_rec(fs::path(path), seen, out, err);
}

[[maybe_unused]] static std::string find_in_path(std::string_view bin) {
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

#ifdef _WIN32
static std::wstring pp_win_quote_arg(std::string_view arg) {
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
}
#endif

static std::string unescape_linemarker_path(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == '\\' || raw[i + 1] == '"')) {
            out.push_back(raw[++i]);
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

bool preprocess_header_cc(const std::string &path,
                          const std::vector<std::string> &include_dirs,
                          const std::vector<std::string> &defines,
                          std::string_view target,
                          std::string &out,
                          std::string &err,
                          std::string *root_only_out) {
    std::vector<std::string> argv;
    if (!target.empty()) {
        std::string zig = find_in_path("zig");
        if (!zig.empty()) {
            argv = {zig, "cc", "-target", std::string(target)};
        } else {
            std::string clang = find_in_path("clang");
            if (!clang.empty()) argv = {clang, "--target=" + std::string(target)};
        }
    }
    if (argv.empty()) {
        for (const char *env_var : {"BRAT_CC", "CC"}) {
            if (const char *ov = std::getenv(env_var); ov && *ov) {
                std::string p = fs::path(ov).has_parent_path() ? std::string(ov) : find_in_path(ov);
                if (!p.empty()) { argv = {p}; break; }
            }
        }
    }
    if (argv.empty()) {
        for (const char *cand : {"cc", "clang", "gcc"}) {
            std::string p = find_in_path(cand);
            if (!p.empty()) { argv = {p}; break; }
        }
    }
    if (argv.empty()) {
        err = "no C compiler found on PATH for --preprocess";
        return false;
    }
    argv.insert(argv.end(), {"-E", "-dD", "-std=gnu11", "-x", "c"});
    std::string here_dir = fs::absolute(fs::path(path)).parent_path().string();
    if (!here_dir.empty()) { argv.push_back("-I"); argv.push_back(here_dir); }
    for (const auto &inc : include_dirs) { argv.push_back("-I"); argv.push_back(inc); }
    for (const auto &def : defines) argv.push_back("-D" + def);
    argv.push_back((!path.empty() && path[0] == '-') ? ("./" + path) : path);

    int timeout_ms = 120000;
    if (const char *ev = std::getenv("BRAT_CC_TIMEOUT_MS"); ev && *ev) {
        char *end = nullptr;
        const long long v = std::strtoll(ev, &end, 10);
        if (end != ev && *end == '\0' && v > 0 && v <= 3600000LL) timeout_ms = static_cast<int>(v);
    }
    std::string raw_out, raw_err;
    constexpr size_t kMaxPpBytes = 64u * 1024u * 1024u;
    bool over = false, timed_out = false;

#ifdef _WIN32
    std::wstring cmdline;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline.push_back(L' ');
        cmdline += pp_win_quote_arg(argv[i]);
    }
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_rd = nullptr, out_wr = nullptr, err_rd = nullptr, err_wr = nullptr;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) { err = "pipe failed"; return false; }
    if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) {
        CloseHandle(out_rd); CloseHandle(out_wr);
        err = "pipe failed";
        return false;
    }
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_wr;
    si.hStdError = err_wr;
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        CloseHandle(out_rd); CloseHandle(out_wr);
        CloseHandle(err_rd); CloseHandle(err_wr);
        if (job) CloseHandle(job);
        err = "CreateProcess failed for C preprocessor";
        return false;
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(out_wr);
    CloseHandle(err_wr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::thread out_thr([&]() {
        char buf[8192];
        DWORD got = 0;
        while (ReadFile(out_rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (raw_out.size() + got > kMaxPpBytes) {
                over = true;
                if (job) TerminateJobObject(job, 124);
                else TerminateProcess(pi.hProcess, 124);
                break;
            }
            for (DWORD k = 0; k < got; ++k) {
                if (buf[k] != '\r') raw_out.push_back(buf[k]);
            }
        }
    });
    std::thread err_thr([&]() {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(err_rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (raw_err.size() < 262144u) raw_err.append(buf, got);
        }
    });
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (wait_rc == WAIT_TIMEOUT) {
        timed_out = true;
        if (job) TerminateJobObject(job, 124);
        else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    out_thr.join();
    err_thr.join();
    DWORD win_ec = 1;
    GetExitCodeProcess(pi.hProcess, &win_ec);
    CloseHandle(pi.hProcess);
    CloseHandle(out_rd);
    CloseHandle(err_rd);
    if (job) CloseHandle(job);
    if (timed_out) { err = "C preprocessor timed out"; return false; }
    if (over) { err = "preprocessed header exceeds 64 MiB limit"; return false; }
    if (win_ec != 0) {
        err = trim_sv(raw_err);
        if (err.empty()) err = "C preprocessor failed on " + path;
        return false;
    }
#else
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        err = "pipe failed";
        return false;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        err = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        err = "fork failed";
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        std::vector<char *> cargv;
        for (auto &s : argv) cargv.push_back(s.data());
        cargv.push_back(nullptr);
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(out_pipe[1]);
    close(err_pipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool o_open = true, e_open = true;
    char buf[8192];
    while (o_open || e_open) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { timed_out = true; break; }
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd fds[2];
        nfds_t nf = 0;
        int oi = -1, ei = -1;
        if (o_open) { oi = static_cast<int>(nf); fds[nf++] = {out_pipe[0], POLLIN, 0}; }
        if (ei < 0 && e_open) { ei = static_cast<int>(nf); fds[nf++] = {err_pipe[0], POLLIN, 0}; }
        int pr = poll(fds, nf, static_cast<int>(std::min<long long>(rem, 100)));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (raw_out.size() + static_cast<size_t>(n) > kMaxPpBytes) { over = true; break; }
                raw_out.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                o_open = false;
            }
        }
        if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (raw_err.size() < 262144u) raw_err.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                e_open = false;
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    if (over || timed_out) {
        if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out) {
        err = "C preprocessor timed out";
        return false;
    }
    if (over) {
        err = "preprocessed header exceeds 64 MiB limit";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err = trim_sv(raw_err);
        if (err.empty()) err = "C preprocessor failed on " + path;
        return false;
    }
#endif

    // Filter out system header chunks marked with linemarker flag `3` when binding a user
    // header, while also supporting direct `--preprocess` on a system header (`/usr/include/*.h`)
    // without flooding output with `<stdc-predef.h>` / `__GLIBC_*` internal macros.
    out.clear();
    if (root_only_out) root_only_out->clear();
    std::error_code canon_ec;
    fs::path abs_target = fs::weakly_canonical(fs::absolute(path, canon_ec), canon_ec);
    std::string target_base = fs::path(path).filename().string();

    auto is_same_root = [&](const std::string &fname) -> bool {
        if (fname.empty() || fname[0] == '<') return false;
        if (fname == path) return true;
        std::error_code ec2;
        if (!abs_target.empty() && fs::weakly_canonical(fs::absolute(fname, ec2), ec2) == abs_target)
            return true;
        return false;
    };
    auto parse_has_flag3 = [](const std::string &flags) -> bool {
        std::istringstream fss(flags);
        std::string tok;
        while (fss >> tok) {
            if (tok == "3") return true;
        }
        return false;
    };
    auto is_plumbing_hdr = [](const std::string &fname) -> bool {
        std::string b = fs::path(fname).filename().string();
        return b == "stdc-predef.h" || b == "features.h" || b == "features-time64.h" ||
               b == "cdefs.h" || b.rfind("stubs", 0) == 0;
    };
    auto is_reserved_sys_macro = [](std::string_view def_rest) -> bool {
        // C99 7.1.3 reserves `__*` and `_[A-Z]*` for the implementation / include guards.
        if (def_rest.size() >= 2 && def_rest[0] == '_') {
            unsigned char c1 = static_cast<unsigned char>(def_rest[1]);
            if (c1 == '_' || std::isupper(c1)) return true;
        }
        return false;
    };

    auto path_in_sys_include = [](const fs::path &p) -> bool {
        std::string s = p.generic_string();
        return s.rfind("/usr/include/", 0) == 0 ||
               s.find("/sysroot/usr/include/") != std::string::npos ||
               s.find(".sdk/usr/include/") != std::string::npos ||
               s.find("/mingw64/include/") != std::string::npos ||
               s.find("/ucrt64/include/") != std::string::npos ||
               s.find("/clang64/include/") != std::string::npos;
    };
    bool root_is_sys = path_in_sys_include(abs_target);
    if (!root_is_sys) {
        std::istringstream scan(raw_out);
        std::string sln;
        while (std::getline(scan, sln)) {
            std::string st = trim_sv(sln);
            if (st.rfind("# ", 0) == 0 && st.find('"') != std::string::npos) {
                size_t q1 = st.find('"');
                size_t q2 = st.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    std::string fn = unescape_linemarker_path(std::string_view(st).substr(q1 + 1, q2 - q1 - 1));
                    std::string fl = trim_sv(std::string_view(st).substr(q2 + 1));
                    if (is_same_root(fn) && parse_has_flag3(fl)) {
                        root_is_sys = true;
                        break;
                    }
                }
            }
        }
    }

    enum class PpMode { Keep, SysKeep, CmdLine, Skip };
    PpMode mode = PpMode::Keep;
    bool in_root_file = false;
    std::istringstream iss(raw_out);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim_sv(line);
        if (t.rfind("# ", 0) == 0 && t.find('"') != std::string::npos) {
            size_t q1 = t.find('"');
            size_t q2 = t.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                std::string fname = unescape_linemarker_path(std::string_view(t).substr(q1 + 1, q2 - q1 - 1));
                std::string flags = trim_sv(std::string_view(t).substr(q2 + 1));
                bool has3 = parse_has_flag3(flags);
                in_root_file = is_same_root(fname);
                if (fname == "<command-line>") {
                    mode = PpMode::CmdLine;
                } else if (!fname.empty() && fname[0] != '<' && !has3) {
                    mode = PpMode::Keep;
                } else if (root_is_sys && !fname.empty() && fname[0] != '<' && !is_plumbing_hdr(fname)) {
                    mode = PpMode::SysKeep;
                } else {
                    mode = PpMode::Skip;
                }
            }
            continue;
        }
        if (mode == PpMode::Skip) continue;
        if (mode == PpMode::CmdLine) {
            if (!t.empty() && t[0] == '#') {
                std::string body = trim_sv(std::string_view(t).substr(1));
                if (body.rfind("define", 0) == 0) {
                    std::string rest = trim_sv(std::string_view(body).substr(6));
                    if (!rest.empty() && std::isalpha(static_cast<unsigned char>(rest[0]))) {
                        out += line;
                        out.push_back('\n');
                    }
                }
            }
            continue;
        }
        if (!t.empty() && t[0] == '#') {
            std::string body = trim_sv(std::string_view(t).substr(1));
            if (body.rfind("define", 0) == 0) {
                std::string rest = trim_sv(std::string_view(body).substr(6));
                if (mode == PpMode::SysKeep && is_reserved_sys_macro(rest)) continue;
                out += line;
                out.push_back('\n');
                if (root_only_out && in_root_file) {
                    *root_only_out += line;
                    root_only_out->push_back('\n');
                }
            } else if (body.rfind("pragma", 0) == 0) {
                out += line;
                out.push_back('\n');
                if (root_only_out && in_root_file) {
                    *root_only_out += line;
                    root_only_out->push_back('\n');
                }
            }
            continue;
        }
        out += line;
        out.push_back('\n');
        if (root_only_out && in_root_file) {
            *root_only_out += line;
            root_only_out->push_back('\n');
        }
    }
    return true;
}

namespace {

using CInt = __int128;

static std::optional<int64_t> parse_pos_dim(std::string_view cnt_s) {
    if (cnt_s.empty() || cnt_s.size() > 12) return std::nullopt;
    for (char c : cnt_s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    char buf[16];
    std::memcpy(buf, cnt_s.data(), cnt_s.size());
    buf[cnt_s.size()] = '\0';
    char *ep = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(buf, &ep, 10);
    if (errno == ERANGE || !ep || *ep != '\0' || v < 1ULL || v > (1ULL << 30))
        return std::nullopt;
    return static_cast<int64_t>(v);
}

static bool fits_signed_i64(CInt v) {
    constexpr CInt kMin = -static_cast<CInt>(1ULL << 63);
    constexpr CInt kMax = static_cast<CInt>((1ULL << 63) - 1ULL);
    return v >= kMin && v <= kMax;
}

// Safe recursive-descent C integer constant evaluator (`|`, `^`, `&`, `<<`, `>>`, `+`, `-`, `*`, `/`, `%`, `~`).
// Evaluates in 128-bit precision so unsigned 64-bit constants like `1ULL << 63` or `0xFFFFFFFFFFFFFFFFULL`
// preserve their exact positive magnitude (`> INT64_MAX`) instead of wrapping to negative `int64_t`.
class ConstExpr {
public:
    ConstExpr(std::string_view src, const std::map<std::string, CInt> &env)
        : s_(src), env_(env) {}

    std::optional<CInt> eval() {
        auto v = parse_or();
        skip_ws();
        if (!v || pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    std::string_view s_;
    const std::map<std::string, CInt> &env_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    std::optional<CInt> parse_or() {
        auto lhs = parse_xor();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '|' && (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '|')) {
                ++pos_;
                auto rhs = parse_xor();
                if (!rhs) return std::nullopt;
                lhs = *lhs | *rhs;
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_xor() {
        auto lhs = parse_and();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '^') {
                ++pos_;
                auto rhs = parse_and();
                if (!rhs) return std::nullopt;
                lhs = *lhs ^ *rhs;
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_and() {
        auto lhs = parse_shift();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '&' && (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '&')) {
                ++pos_;
                auto rhs = parse_shift();
                if (!rhs) return std::nullopt;
                lhs = *lhs & *rhs;
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_shift() {
        auto lhs = parse_add();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ + 1 < s_.size() && s_[pos_] == '<' && s_[pos_ + 1] == '<') {
                pos_ += 2;
                auto rhs = parse_add();
                if (!rhs || *rhs < 0 || *rhs >= 64) return std::nullopt;
                lhs = static_cast<CInt>(static_cast<unsigned __int128>(*lhs) << static_cast<int>(*rhs));
            } else if (pos_ + 1 < s_.size() && s_[pos_] == '>' && s_[pos_ + 1] == '>') {
                pos_ += 2;
                auto rhs = parse_add();
                if (!rhs || *rhs < 0 || *rhs >= 64) return std::nullopt;
                lhs = *lhs >> static_cast<int>(*rhs);
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_add() {
        auto lhs = parse_mul();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                char op = s_[pos_++];
                auto rhs = parse_mul();
                if (!rhs) return std::nullopt;
                CInt res = 0;
                bool ov = (op == '+') ? __builtin_add_overflow(*lhs, *rhs, &res)
                                      : __builtin_sub_overflow(*lhs, *rhs, &res);
                if (ov) return std::nullopt;
                lhs = res;
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_mul() {
        auto lhs = parse_unary();
        if (!lhs) return std::nullopt;
        while (true) {
            skip_ws();
            if (pos_ < s_.size() && (s_[pos_] == '*' || s_[pos_] == '/' || s_[pos_] == '%')) {
                char op = s_[pos_++];
                auto rhs = parse_unary();
                if (!rhs) return std::nullopt;
                if (op == '*') {
                    CInt res = 0;
                    if (__builtin_mul_overflow(*lhs, *rhs, &res)) return std::nullopt;
                    lhs = res;
                } else {
                    constexpr CInt kMin128 = static_cast<CInt>(static_cast<unsigned __int128>(1) << 127);
                    if (*rhs == 0 || (*lhs == kMin128 && *rhs == -1)) return std::nullopt;
                    lhs = (op == '/') ? (*lhs / *rhs) : (*lhs % *rhs);
                }
            } else break;
        }
        return lhs;
    }

    std::optional<CInt> parse_unary() {
        skip_ws();
        if (pos_ >= s_.size()) return std::nullopt;
        if (s_[pos_] == '+') { ++pos_; return parse_unary(); }
        if (s_[pos_] == '-') {
            ++pos_;
            auto v = parse_unary();
            if (!v) return std::nullopt;
            CInt res = 0;
            if (__builtin_sub_overflow(CInt(0), *v, &res)) return std::nullopt;
            return res;
        }
        if (s_[pos_] == '~') {
            ++pos_;
            auto v = parse_unary();
            if (!v) return std::nullopt;
            return ~(*v);
        }
        if (s_[pos_] == '(') {
            size_t save = pos_ + 1;
            int depth = 1;
            size_t close = save;
            while (close < s_.size() && depth > 0) {
                if (s_[close] == '(') ++depth;
                else if (s_[close] == ')') --depth;
                if (depth > 0) ++close;
            }
            if (depth != 0) return std::nullopt;
            std::string inner = trim_sv(s_.substr(save, close - save));
            static const std::set<std::string> kCastWords = {
                "int", "unsigned", "signed", "long", "short", "char",
                "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
                "int64_t", "uint64_t", "size_t", "ssize_t", "intptr_t", "uintptr_t"
            };
            auto ws = ident_words(inner);
            bool all_cast = !ws.empty() && inner.find_first_of("+-*/%|&^~<>()") == std::string::npos;
            for (const auto &w : ws) if (!kCastWords.count(w)) all_cast = false;
            if (all_cast) {
                pos_ = close + 1;
                return parse_unary();
            }
            ++pos_;
            auto v = parse_or();
            skip_ws();
            if (!v || pos_ >= s_.size() || s_[pos_] != ')') return std::nullopt;
            ++pos_;
            return v;
        }
        if (s_[pos_] == '\'' && pos_ + 2 < s_.size()) {
            ++pos_;
            unsigned char ch = 0;
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                ++pos_;
                char e = s_[pos_++];
                ch = (e == 'n') ? '\n' : (e == 'r' ? '\r' : (e == 't' ? '\t' : (e == '0' ? '\0' : static_cast<unsigned char>(e))));
            } else {
                ch = static_cast<unsigned char>(s_[pos_++]);
            }
            if (pos_ >= s_.size() || s_[pos_] != '\'') return std::nullopt;
            ++pos_;
            return static_cast<CInt>(ch);
        }
        if (std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            size_t start = pos_;
            int base = 10;
            if (s_[pos_] == '0' && pos_ + 1 < s_.size()) {
                if (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X') { base = 16; pos_ += 2; start = pos_; }
                else if (s_[pos_ + 1] == 'b' || s_[pos_ + 1] == 'B') { base = 2; pos_ += 2; start = pos_; }
                else if (std::isdigit(static_cast<unsigned char>(s_[pos_ + 1]))) { base = 8; }
            }
            while (pos_ < s_.size() && (std::isxdigit(static_cast<unsigned char>(s_[pos_])))) ++pos_;
            std::string digits(s_.substr(start, pos_ - start));
            if (digits.empty()) return std::nullopt;
            while (pos_ < s_.size() && (s_[pos_] == 'u' || s_[pos_] == 'U' || s_[pos_] == 'l' || s_[pos_] == 'L'))
                ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'f' || s_[pos_] == 'F')) return std::nullopt;
            char *endp = nullptr;
            errno = 0;
            unsigned long long uv = std::strtoull(digits.c_str(), &endp, base);
            if (errno == ERANGE || !endp || *endp != '\0') return std::nullopt;
            return static_cast<CInt>(uv);
        }
        if (is_ident0(s_[pos_])) {
            size_t start = pos_;
            while (pos_ < s_.size() && is_identc(s_[pos_])) ++pos_;
            std::string id(s_.substr(start, pos_ - start));
            auto it = env_.find(id);
            if (it == env_.end()) return std::nullopt;
            return it->second;
        }
        return std::nullopt;
    }
};

static std::optional<CInt> eval_c_int(std::string_view expr, const std::map<std::string, CInt> &env) {
    std::string t = trim_sv(expr);
    if (t.empty()) return std::nullopt;
    return ConstExpr(t, env).eval();
}

static std::optional<int64_t> eval_pos_dim(std::string_view cnt_s, const CDecl *cdecl) {
    if (auto fast = parse_pos_dim(cnt_s)) return fast;
    std::map<std::string, CInt> env;
    if (cdecl) {
        for (const auto &[k, v] : cdecl->consts) env[k] = static_cast<CInt>(v);
        for (const auto &[_, items] : cdecl->enums) {
            for (const auto &[ek, ev] : items) env[ek] = static_cast<CInt>(ev);
        }
    }
    auto val = eval_c_int(cnt_s, env);
    if (!val || *val < 1 || *val > static_cast<CInt>(1ULL << 30)) return std::nullopt;
    return static_cast<int64_t>(*val);
}

static bool has_packed_attr(std::string_view s) {
    return s.find("__packed__") != std::string_view::npos ||
           s.find("(packed)") != std::string_view::npos ||
           s.find("(packed,") != std::string_view::npos ||
           s.find(", packed)") != std::string_view::npos ||
           s.find(",packed)") != std::string_view::npos;
}

static bool has_aligned_attr(std::string_view s) {
    if (s.find("_Alignas") != std::string_view::npos) return true;
    size_t pos = 0;
    while ((pos = s.find("aligned", pos)) != std::string_view::npos) {
        bool left_ok = (pos == 0 || !is_identc(s[pos - 1]) || (pos >= 2 && s.substr(pos - 2, 2) == "__"));
        size_t end = pos + 7;
        if (end + 2 <= s.size() && s.substr(end, 2) == "__") end += 2;
        bool right_ok = (end == s.size() || !is_identc(s[end]));
        if (left_ok && right_ok && s.find("__attribute__", 0) != std::string_view::npos) return true;
        pos += 7;
    }
    return false;
}

static std::string detect_abi_attr(std::string_view s) {
    static const std::pair<const char *, const char *> kBad[] = {
        {"fastcall", "fastcall"}, {"thiscall", "thiscall"}, {"vectorcall", "vectorcall"},
        {"stdcall", "stdcall"}, {"regparm", "regparm"}, {"ms_abi", "ms_abi"},
        {"sysv_abi", "sysv_abi"}, {"naked", "naked"}, {"interrupt", "interrupt"},
    };
    for (auto [needle, label] : kBad) {
        if (s.find(needle) != std::string_view::npos) return std::string("calling convention ") + label;
    }
    if (s.find("scalar_storage_order") != std::string_view::npos) return "attribute scalar_storage_order";
    if (s.find("transparent_union") != std::string_view::npos) return "attribute transparent_union";
    return {};
}

static std::string strip_c_attrs(std::string_view src) {
    std::string s(src);
    for (;;) {
        size_t p = s.find("__attribute__");
        if (p == std::string::npos) break;
        size_t q = p + 13;
        while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) ++q;
        if (q < s.size() && s[q] == '(') {
            int d = 0;
            while (q < s.size()) {
                if (s[q] == '(') ++d;
                else if (s[q] == ')') { --d; if (d == 0) { ++q; break; } }
                ++q;
            }
            s.replace(p, q - p, " ");
        } else {
            s.replace(p, 13, " ");
        }
    }
    for (;;) {
        size_t p = s.find("__declspec");
        if (p == std::string::npos) break;
        size_t q = p + 10;
        while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) ++q;
        if (q < s.size() && s[q] == '(') {
            int d = 0;
            while (q < s.size()) {
                if (s[q] == '(') ++d;
                else if (s[q] == ')') { --d; if (d == 0) { ++q; break; } }
                ++q;
            }
            s.replace(p, q - p, " ");
        } else {
            s.replace(p, 10, " ");
        }
    }
    for (;;) {
        size_t p = s.find("_Alignas");
        if (p == std::string::npos) break;
        size_t q = p + 8;
        while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) ++q;
        if (q < s.size() && s[q] == '(') {
            int d = 0;
            while (q < s.size()) {
                if (s[q] == '(') ++d;
                else if (s[q] == ')') { --d; if (d == 0) { ++q; break; } }
                ++q;
            }
            s.replace(p, q - p, " ");
        } else {
            s.replace(p, 8, " ");
        }
    }
    // Unwrap C11 `_Atomic(T)` specifier form into `T`.
    for (;;) {
        size_t p = s.find("_Atomic");
        if (p == std::string::npos) break;
        if (p > 0 && is_identc(s[p - 1])) break;
        size_t q = p + 7;
        if (q < s.size() && is_identc(s[q])) break;
        while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) ++q;
        if (q < s.size() && s[q] == '(') {
            size_t lp = q;
            int d = 0;
            while (q < s.size()) {
                if (s[q] == '(') ++d;
                else if (s[q] == ')') { --d; if (d == 0) break; }
                ++q;
            }
            if (q < s.size() && s[q] == ')') {
                std::string inner = s.substr(lp + 1, q - lp - 1);
                s.replace(p, q - p + 1, " " + inner + " ");
                continue;
            }
        }
        break;
    }
    // Strip common noise tokens: __extension__, __restrict, __restrict__, restrict, inline, __inline__, _Nonnull, _Nullable, FAR, ZEXPORT, etc.
    static const std::set<std::string> kNoise = {
        "__extension__", "__restrict", "__restrict__", "restrict",
        "_Atomic", "__volatile", "__volatile__",
        "inline", "__inline", "__inline__", "_Inline",
        "_Nonnull", "_Nullable", "_Null_unspecified",
        "FAR", "NEAR", "ZEXPORT", "ZEXPORTVA", "ZEXTERN", "OF",
        "__cdecl", "_cdecl"
    };
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (is_ident0(s[i])) {
            size_t j = i + 1;
            while (j < s.size() && is_identc(s[j])) ++j;
            std::string w = s.substr(i, j - i);
            if (w == "__const" || w == "__const__") {
                out += "const";
                i = j;
                continue;
            }
            if (w == "__signed" || w == "__signed__") {
                out += "signed";
                i = j;
                continue;
            }
            // Strip ALL_CAPS export macros sitting before return type / calling convention (e.g. SQLITE_API, SQLITE_APICALL, SQLITE_STDCALL, PNGAPI)
            bool all_upper_macro = false;
            if (w.size() >= 3 && (w.find("_API") != std::string::npos || w.find("EXPORT") != std::string::npos ||
                                  w.find("EXTERN") != std::string::npos || w.find("STDCALL") != std::string::npos ||
                                  w.find("CDECL") != std::string::npos || w == "WINAPI" || w == "APIENTRY")) {
                all_upper_macro = true;
                for (char ch : w) if (std::islower(static_cast<unsigned char>(ch))) all_upper_macro = false;
            }
            if (kNoise.count(w) || all_upper_macro) {
                if (w == "OF") {
                    // zlib `OF((args))` -> `(args)`
                    size_t k = j;
                    while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
                    if (k + 1 < s.size() && s[k] == '(' && s[k + 1] == '(') {
                        int d = 0;
                        size_t m = k;
                        while (m < s.size()) {
                            if (s[m] == '(') ++d;
                            else if (s[m] == ')') { --d; if (d == 0) break; }
                            ++m;
                        }
                        if (m < s.size() && m >= 1 && s[m - 1] == ')') {
                            out += s.substr(k + 1, m - k - 1);
                            i = m + 1;
                            continue;
                        }
                    }
                }
                out.push_back(' ');
            } else {
                out += w;
            }
            i = j;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

static std::vector<std::string> split_top_level(std::string_view s, char delim) {
    std::vector<std::string> out;
    int dp = 0, db = 0, dc = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '(') ++dp;
        else if (c == ')') --dp;
        else if (c == '[') ++db;
        else if (c == ']') --db;
        else if (c == '{') ++dc;
        else if (c == '}') --dc;
        else if (c == delim && dp == 0 && db == 0 && dc == 0) {
            out.push_back(trim_sv(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim_sv(s.substr(start)));
    return out;
}

// Parse a function-pointer declarator like `int (*cmp)(const void *, const void *)`
// into normalized type `int (*)(const void *, const void *)` and optional name `cmp`.
// In `struct P` and `const handle_t` the last word is the type, not a parameter name: what precedes it has no type yet.
static bool ends_in_type_name(std::string_view head) {
    std::string last;
    for (const auto &w : ident_words(head)) {
        if (w == "const" || w == "volatile" || w == "register" || w == "restrict") continue;
        last = w;
    }
    return last.empty() || last == "struct" || last == "union" || last == "enum";
}

static bool parse_fnptr_decl(std::string_view raw, std::string &norm_type, std::string &name_out) {
    std::string s = trim_sv(strip_c_attrs(raw));
    size_t lp1 = std::string::npos;
    int d = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') {
            if (d == 0) {
                size_t k = i + 1;
                while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
                if (k < s.size() && s[k] == '*') { lp1 = i; break; }
            }
            ++d;
        } else if (s[i] == ')') --d;
    }
    if (lp1 == std::string::npos) return false;
    int dp = 0;
    size_t rp1 = lp1;
    while (rp1 < s.size()) {
        if (s[rp1] == '(') ++dp;
        else if (s[rp1] == ')') { --dp; if (dp == 0) break; }
        ++rp1;
    }
    if (rp1 >= s.size()) return false;
    std::string inner = trim_sv(std::string_view(s).substr(lp1 + 1, rp1 - lp1 - 1));
    if (inner.empty() || inner[0] != '*') return false;
    // Reject array of function pointers `(*fp[4])(...)`
    if (inner.find('[') != std::string::npos) return false;
    size_t k = 1;
    while (k < inner.size() && (inner[k] == '*' || std::isspace(static_cast<unsigned char>(inner[k])))) ++k;
    // Count stars inside `(*...)`: more than 1 star is pointer-to-fnptr (`ptr`), not callback!
    size_t nstars = 0;
    for (char ch : inner) if (ch == '*') ++nstars;
    if (nstars != 1) return false;
    std::string rest_inner = trim_sv(std::string_view(inner).substr(k));
    if (rest_inner == "const") rest_inner.clear();
    else if (rest_inner.rfind("const ", 0) == 0) rest_inner = trim_sv(std::string_view(rest_inner).substr(6));
    if (!rest_inner.empty() && !is_valid_ident(rest_inner)) return false;

    size_t lp2 = rp1 + 1;
    while (lp2 < s.size() && std::isspace(static_cast<unsigned char>(s[lp2]))) ++lp2;
    if (lp2 >= s.size() || s[lp2] != '(') return false;
    dp = 0;
    size_t rp2 = lp2;
    while (rp2 < s.size()) {
        if (s[rp2] == '(') ++dp;
        else if (s[rp2] == ')') { --dp; if (dp == 0) break; }
        ++rp2;
    }
    if (rp2 >= s.size() || !trim_sv(std::string_view(s).substr(rp2 + 1)).empty()) return false;

    std::string ret = trim_sv(std::string_view(s).substr(0, lp1));
    if (ret.empty()) return false;
    std::string args_raw = trim_sv(std::string_view(s).substr(lp2 + 1, rp2 - lp2 - 1));
    std::vector<std::string> clean_args;
    if (!args_raw.empty() && args_raw != "void") {
        for (const auto &part : split_top_level(args_raw, ',')) {
            if (part.empty()) continue;
            if (part == "...") return false;
            std::string p_norm, p_nm;
            if (parse_fnptr_decl(part, p_norm, p_nm)) {
                clean_args.push_back(p_norm);
                continue;
            }
            // Strip trailing parameter name if present
            std::string pt = part;
            size_t br = pt.find('[');
            if (br != std::string::npos) {
                // `int a[4]` parameter decays to pointer `int *`
                std::string head = trim_sv(std::string_view(pt).substr(0, br));
                size_t id_pos = head.size();
                while (id_pos > 0 && is_identc(head[id_pos - 1])) --id_pos;
                if (id_pos < head.size() && is_ident0(head[id_pos])) head = trim_sv(std::string_view(head).substr(0, id_pos));
                pt = head + " *";
            } else {
                size_t id_pos = pt.size();
                while (id_pos > 0 && is_identc(pt[id_pos - 1])) --id_pos;
                if (id_pos < pt.size() && is_ident0(pt[id_pos])) {
                    std::string head = trim_sv(std::string_view(pt).substr(0, id_pos));
                    std::string tail = pt.substr(id_pos);
                    static const std::set<std::string> kTypeKeywords = {
                        "void", "char", "short", "int", "long", "float", "double",
                        "signed", "unsigned", "const", "volatile", "_Bool", "bool",
                        "size_t", "ssize_t", "intptr_t", "uintptr_t", "ptrdiff_t",
                        "off_t", "time_t", "pid_t", "uid_t", "gid_t", "mode_t", "socklen_t",
                        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t"
                    };
                    if (!head.empty() && !kTypeKeywords.count(tail) && !ends_in_type_name(head)) pt = head;
                }
            }
            clean_args.push_back(trim_sv(pt));
        }
    }
    std::string joined;
    for (size_t idx = 0; idx < clean_args.size(); ++idx) {
        if (idx) joined += ", ";
        joined += clean_args[idx];
    }
    norm_type = ret + " (*)(" + joined + ")";
    name_out = rest_inner;
    return true;
}

// Split a non-fnptr C declaration (`const char *const name[4]`) into `(type, name)`.
static bool split_c_decl_type_and_name(std::string_view raw,
                                       const std::map<std::string, CInt> &consts,
                                       std::string &type_out,
                                       std::string &name_out,
                                       bool decay_param_array) {
    std::string fn_t, fn_nm;
    if (parse_fnptr_decl(raw, fn_t, fn_nm)) {
        type_out = fn_t;
        name_out = fn_nm;
        return true;
    }
    std::string s = trim_sv(strip_c_attrs(raw));
    if (s.empty()) return false;

    std::string arr_suffix;
    bool had_array = false;
    while (!s.empty() && s.back() == ']') {
        size_t lb = s.rfind('[');
        if (lb == std::string::npos) return false;
        std::string dim_expr = trim_sv(std::string_view(s).substr(lb + 1, s.size() - lb - 2));
        s = trim_sv(std::string_view(s).substr(0, lb));
        had_array = true;
        if (decay_param_array) {
            arr_suffix = " *";
        } else {
            if (dim_expr.empty()) {
                arr_suffix = "[]" + arr_suffix;
            } else {
                auto v = eval_c_int(dim_expr, consts);
                if (v && *v >= 0 && fits_signed_i64(*v)) arr_suffix = "[" + std::to_string(static_cast<int64_t>(*v)) + "]" + arr_suffix;
                else arr_suffix = "[" + dim_expr + "]" + arr_suffix;
            }
        }
    }

    // Handle pointer-to-array `int (*p)[4]` -> opaque pointer (`void *`)
    if (!s.empty() && s.back() == ')' && s.find("(*") != std::string::npos) {
        size_t lp = s.rfind("(*");
        std::string nm = trim_sv(std::string_view(s).substr(lp + 2, s.size() - lp - 3));
        name_out = is_valid_ident(nm) ? nm : "";
        type_out = "void *";
        return true;
    }

    size_t id_pos = s.size();
    while (id_pos > 0 && is_identc(s[id_pos - 1])) --id_pos;
    if (id_pos < s.size() && is_ident0(s[id_pos])) {
        std::string cand = s.substr(id_pos);
        std::string head = trim_sv(std::string_view(s).substr(0, id_pos));
        static const std::set<std::string> kTypeWords = {
            "void", "char", "short", "int", "long", "float", "double",
            "signed", "unsigned", "const", "volatile", "_Bool", "bool",
            "size_t", "ssize_t", "intptr_t", "uintptr_t", "ptrdiff_t",
            "off_t", "time_t", "pid_t", "uid_t", "gid_t", "mode_t", "socklen_t",
            "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t"
        };
        if (!head.empty() && !kTypeWords.count(cand) && !ends_in_type_name(head)) {
            name_out = cand;
            type_out = head + (had_array ? arr_suffix : "");
            return true;
        }
    }
    name_out.clear();
    type_out = s + (had_array ? arr_suffix : "");
    return true;
}

static ResolvedCType base_c_type(std::string_view raw, const TargetModel &tm) {
    std::string fn_t, fn_nm;
    if (parse_fnptr_decl(raw, fn_t, fn_nm)) return {"fn", fn_t, {}, {}, 0};

    std::string s = trim_sv(strip_c_attrs(raw));
    if (s.empty()) return {"invalid", {}, {}, {}, 0};

    // Inline C array `T[N]` (including constant expressions and multi-dimensional `T[M][N]`)
    if (s.back() == ']' && s.find('[') != std::string::npos) {
        size_t lb = s.rfind('[');
        std::string elem = trim_sv(std::string_view(s).substr(0, lb));
        std::string cnt_s = trim_sv(std::string_view(s).substr(lb + 1, s.size() - lb - 2));
        if (!elem.empty()) {
            if (auto cnt = eval_pos_dim(cnt_s, nullptr)) {
                auto ek = base_c_type(elem, tm);
                if (ek.kind == "array") {
                    int64_t total = 0;
                    if (!__builtin_mul_overflow(*cnt, ek.arr_count, &total) && total <= (1LL << 30))
                        return {"array", {}, ek.arr_elem_k, ek.arr_elem_e, total};
                    return {"array_unsized", s, {}, {}, 0};
                }
                return {"array", {}, ek.kind, ek.extra, *cnt};
            }
        }
        return {"array_unsized", s, {}, {}, 0};
    }

    size_t stars = 0;
    for (char c : s) if (c == '*') ++stars;
    size_t first_star = s.find('*');
    std::string before_first_star = (first_star == std::string::npos) ? s : s.substr(0, first_star);
    auto before_words = ident_words(before_first_star);
    bool pointee_is_const = false;
    for (const auto &w : before_words) if (w == "const") pointee_is_const = true;

    std::string no_stars;
    for (char c : s) no_stars.push_back(c == '*' ? ' ' : c);
    auto words = ident_words(no_stars);
    std::vector<std::string> toks;
    std::string named;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto &w = words[i];
        if (w == "const" || w == "volatile" || w == "register" || w == "extern" || w == "static") continue;
        if ((w == "struct" || w == "union" || w == "enum") && i + 1 < words.size()) {
            named = words[i + 1];
            toks.push_back(w);
            ++i;
            continue;
        }
        toks.push_back(w);
    }

    auto scalar_of = [&](const std::string &k) -> std::optional<std::string> {
        if (k == "void") return "none";
        if (k == "_Bool" || k == "bool") return "bool";
        if (k == "char" || k == "unsigned char" || k == "uint8_t" || k == "u_int8_t" || k == "Byte" || k == "Bytef") return "u8";
        if (k == "signed char" || k == "int8_t") return "i8";
        if (k == "short" || k == "short int" || k == "signed short" || k == "signed short int" || k == "int16_t") return "i16";
        if (k == "unsigned short" || k == "unsigned short int" || k == "uint16_t") return "u16";
        if (k == "int" || k == "signed" || k == "signed int" || k == "int32_t" || k == "pid_t") return "i32";
        if (k == "unsigned" || k == "unsigned int" || k == "uint32_t" || k == "uInt" || k == "uIntf" ||
            k == "uid_t" || k == "gid_t" || k == "mode_t" || k == "socklen_t") return "u32";
        if (k == "long" || k == "long int" || k == "signed long" || k == "signed long int") return (tm.lng == 8) ? "i64" : "i32";
        if (k == "unsigned long" || k == "unsigned long int" || k == "uLong" || k == "uLongf") return (tm.lng == 8) ? "u64" : "u32";
        if (k == "long long" || k == "long long int" || k == "signed long long" || k == "signed long long int" || k == "int64_t") return "i64";
        if (k == "unsigned long long" || k == "unsigned long long int" || k == "uint64_t") return "u64";
        if (k == "size_t" || k == "uintptr_t") return "usize";
        if (k == "ssize_t" || k == "ptrdiff_t" || k == "intptr_t" || k == "off_t" || k == "time_t") return (tm.ptr == 8) ? "i64" : "i32";
        if (k == "float") return "f32";
        if (k == "double") return "f64";
        if (k == "long double") return "long_double";
        return std::nullopt;
    };

    std::string cleaned;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i) cleaned.push_back(' ');
        cleaned += toks[i];
    }

    if (stars > 0) {
        if (stars == 1 && cleaned == "char" && pointee_is_const) return {"str", {}, {}, {}, 0};
        if (stars == 1) {
            if (!named.empty()) return {"ptr", named, {}, {}, 0};
            auto sc = scalar_of(cleaned);
            if (sc && *sc != "none" && *sc != "long_double") return {"ptr", *sc, {}, {}, 0};
            if (sc && *sc == "none") return {"ptr", {}, {}, {}, 0, true};
            if (!cleaned.empty()) return {"ptr", cleaned, {}, {}, 0};
        }
        return {"ptr", "ptr", {}, {}, 0};
    }

    if (!named.empty()) {
        bool is_rec = (cleaned == "struct" || cleaned == "union");
        return {is_rec ? "struct" : "i32", named, {}, {}, 0};
    }
    if (auto sc = scalar_of(cleaned)) return {*sc, {}, {}, {}, 0};
    return {"typedef", cleaned, {}, {}, 0};
}

static std::string derive_enum_scalar(std::string_view base_type,
                                      bool is_packed,
                                      const std::vector<std::pair<std::string, CInt>> &members,
                                      const CDecl &cdecl,
                                      const TargetModel &tm) {
    if (!base_type.empty()) {
        auto r = resolve_c_type(&cdecl, base_type, tm);
        if (r.kind == "usize") return (tm.ptr == 8) ? "u64" : "u32";
        static const std::set<std::string> kInts = {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"};
        if (kInts.count(r.kind)) return r.kind;
    }
    CInt lo = 0, hi = 0;
    if (!members.empty()) {
        lo = hi = members[0].second;
        for (const auto &kv : members) {
            lo = std::min(lo, kv.second);
            hi = std::max(hi, kv.second);
        }
    }
    if (is_packed) {
        if (lo < 0) {
            if (lo >= -128 && hi <= 127) return "i8";
            if (lo >= -32768 && hi <= 32767) return "i16";
            if (lo >= -(1LL << 31) && hi <= (1LL << 31) - 1) return "i32";
            return "i64";
        }
        if (hi <= 0xFF) return "u8";
        if (hi <= 0xFFFF) return "u16";
        if (hi <= static_cast<CInt>(0xFFFFFFFFULL)) return "u32";
        return "u64";
    }
    if (lo >= -(1LL << 31) && hi <= (1LL << 31) - 1) return "i32";
    if (lo >= 0 && hi <= static_cast<CInt>(0xFFFFFFFFULL)) return "u32";
    return (lo < 0) ? "i64" : "u64";
}

static void parse_struct_body(const std::string &rec_name,
                              std::string_view raw_body,
                              bool is_union,
                              bool is_packed,
                              int active_pack,
                              CDecl &out,
                              const std::map<std::string, CInt> &consts) {
    if (rec_name.empty()) return;
    if (is_union) out.unions.insert(rec_name);
    if (is_packed || active_pack == 1) out.packed.insert(rec_name);
    if (active_pack > 0) out.pack_align[rec_name] = active_pack;

    std::vector<std::pair<std::string, std::string>> fields;
    size_t i = 0;
    int anon_idx = 0;
    while (i < raw_body.size()) {
        while (i < raw_body.size() && (std::isspace(static_cast<unsigned char>(raw_body[i])) || raw_body[i] == ';')) ++i;
        if (i >= raw_body.size()) break;

        // Scan up to matching `;`, tracking nested `{...}`
        size_t start = i;
        int brace_d = 0;
        while (i < raw_body.size()) {
            if (raw_body[i] == '{') ++brace_d;
            else if (raw_body[i] == '}') --brace_d;
            else if (raw_body[i] == ';' && brace_d == 0) break;
            ++i;
        }
        std::string stmt = trim_sv(raw_body.substr(start, i - start));
        if (i < raw_body.size() && raw_body[i] == ';') ++i;
        if (stmt.empty()) continue;

        if (has_aligned_attr(stmt)) {
            out.anon.insert(rec_name);
        }

        // Inline nested `struct`/`union` `{ ... } [field];`
        size_t ob = stmt.find('{');
        size_t cb = stmt.rfind('}');
        if (ob != std::string::npos && cb != std::string::npos && cb > ob) {
            std::string head = trim_sv(std::string_view(stmt).substr(0, ob));
            std::string inner = stmt.substr(ob + 1, cb - ob - 1);
            std::string tail = trim_sv(std::string_view(stmt).substr(cb + 1));
            auto hw = ident_words(strip_c_attrs(head));
            bool sub_union = !hw.empty() && hw[0] == "union";
            bool sub_struct = !hw.empty() && (hw[0] == "struct" || hw[0] == "union");
            bool sub_packed = is_packed || has_packed_attr(head) || has_packed_attr(tail);
            if (sub_struct) {
                if (tail.empty()) {
                    std::string af = "anon" + std::to_string(anon_idx++);
                    std::string synth = rec_name + "__" + af;
                    out.inline_structs.insert(synth);
                    out.aliases.insert(synth);
                    out.anon_members[{rec_name, af}] = synth;
                    parse_struct_body(synth, inner, sub_union, sub_packed, active_pack, out, consts);
                    fields.emplace_back(af, synth);
                } else {
                    std::string ftype, fname;
                    split_c_decl_type_and_name("int " + tail, consts, ftype, fname, false);
                    if (!fname.empty() && tail.find('*') == std::string::npos) {
                        std::string synth = rec_name + "__" + fname;
                        out.inline_structs.insert(synth);
                        out.aliases.insert(synth);
                        parse_struct_body(synth, inner, sub_union, sub_packed, active_pack, out, consts);
                        std::string arr_part;
                        if (ftype.size() > 3 && ftype.rfind("int", 0) == 0) arr_part = trim_sv(std::string_view(ftype).substr(3));
                        if (!arr_part.empty()) out.arrays.insert(rec_name);
                        fields.emplace_back(fname, synth + arr_part);
                    } else {
                        out.anon.insert(rec_name);
                    }
                }
                continue;
            }
            out.anon.insert(rec_name);
            continue;
        }

        // Bitfield check (`int a : 3` or `int : 0`)
        size_t colon = stmt.find(':');
        if (colon != std::string::npos && stmt.find('(') == std::string::npos) {
            std::string lhs = trim_sv(std::string_view(stmt).substr(0, colon));
            std::string rhs = trim_sv(std::string_view(stmt).substr(colon + 1));
            std::string btype, bname;
            if (!split_c_decl_type_and_name(lhs, consts, btype, bname, false)) btype = lhs;
            auto bw = eval_c_int(rhs, consts);
            out.bitfields[rec_name].push_back({bname, btype, (bw && fits_signed_i64(*bw)) ? std::to_string(static_cast<int64_t>(*bw)) : rhs});
            continue;
        }

        // Split comma-separated declarators (`int x, y;` or `int *a, b[4];`)
        auto parts = split_top_level(stmt, ',');
        if (parts.size() == 1) {
            std::string ftype, fname;
            if (split_c_decl_type_and_name(parts[0], consts, ftype, fname, false) && !fname.empty()) {
                if (ftype.find('[') != std::string::npos) out.arrays.insert(rec_name);
                fields.emplace_back(fname, ftype);
            }
        } else {
            // Extract shared base specifier from parts[0] before first `*` or identifier
            std::string p0 = trim_sv(strip_c_attrs(parts[0]));
            std::string t0, n0;
            if (split_c_decl_type_and_name(p0, consts, t0, n0, false) && !n0.empty()) {
                if (t0.find('[') != std::string::npos) out.arrays.insert(rec_name);
                fields.emplace_back(n0, t0);
                std::string base_spec = t0;
                size_t st = base_spec.find('*');
                if (st != std::string::npos) base_spec = trim_sv(std::string_view(base_spec).substr(0, st));
                size_t br = base_spec.find('[');
                if (br != std::string::npos) base_spec = trim_sv(std::string_view(base_spec).substr(0, br));
                for (size_t pi = 1; pi < parts.size(); ++pi) {
                    std::string tn, nn;
                    if (split_c_decl_type_and_name(base_spec + " " + parts[pi], consts, tn, nn, false) && !nn.empty()) {
                        if (tn.find('[') != std::string::npos) out.arrays.insert(rec_name);
                        fields.emplace_back(nn, tn);
                    }
                }
            }
        }
    }
    out.structs[rec_name] = std::move(fields);
}

}  // namespace

bool parse_c_header(std::string_view raw_text,
                    const TargetModel &tm,
                    bool strict_directives,
                    CDecl &out,
                    std::string &err) {
    out = CDecl{};
    err.clear();
    std::string uncommented = strip_include_guard(strip_comments(raw_text));

    // Pass 1: preprocess lines: collect `#define` constants, track `#pragma pack`,
    // strip `#ifdef __cplusplus` `extern "C" {` / `}`, and reject conditional compilation
    // directives when `strict_directives` is true.
    std::map<std::string, CInt> const_env;
    std::map<std::string, std::string> obj_macros;
    std::set<std::string> seen_consts;
    std::vector<std::string> clean_lines;
    {
        // Join `\`-continued lines before scanning directives (`#define` multi-line bodies, etc.)
        std::vector<std::string> raw_lines;
        {
            std::istringstream iss(uncommented);
            std::string line, acc;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && line.back() == '\\') {
                    line.pop_back();
                    acc += line + " ";
                    continue;
                }
                acc += line;
                raw_lines.push_back(std::move(acc));
                acc.clear();
            }
            if (!acc.empty()) raw_lines.push_back(std::move(acc));
        }

        auto expand_obj_macros = [&](const std::string &src_ln) -> std::string {
            if (obj_macros.empty()) return src_ln;
            std::string cur = src_ln;
            for (int depth = 0; depth < 8; ++depth) {
                std::string next;
                next.reserve(cur.size());
                bool changed = false;
                bool in_str = false;
                char q = 0;
                for (size_t i = 0; i < cur.size();) {
                    char c = cur[i];
                    if (in_str) {
                        next.push_back(c);
                        if (c == '\\' && i + 1 < cur.size()) { next.push_back(cur[i + 1]); i += 2; continue; }
                        if (c == q) in_str = false;
                        ++i;
                        continue;
                    }
                    if (c == '"' || c == '\'') { in_str = true; q = c; next.push_back(c); ++i; continue; }
                    if (is_ident0(c)) {
                        size_t j = i + 1;
                        while (j < cur.size() && is_identc(cur[j])) ++j;
                        std::string id = cur.substr(i, j - i);
                        auto mit = obj_macros.find(id);
                        if (mit != obj_macros.end()) {
                            next.push_back(' ');
                            next += mit->second;
                            next.push_back(' ');
                            changed = true;
                        } else {
                            next += id;
                        }
                        i = j;
                        continue;
                    }
                    next.push_back(c);
                    ++i;
                }
                cur = std::move(next);
                if (!changed) break;
            }
            return cur;
        };

        for (size_t li = 0; li < raw_lines.size(); ++li) {
            std::string t = trim_sv(raw_lines[li]);
            // Recognize `#ifdef __cplusplus` / `#if defined(__cplusplus)` wrapping `extern "C" {` or `}`
            if (t == "#ifdef __cplusplus" || t == "#if defined(__cplusplus)" || t == "#if defined (__cplusplus)") {
                size_t j = li + 1;
                while (j < raw_lines.size() && trim_sv(raw_lines[j]).empty()) ++j;
                size_t k = j + 1;
                while (k < raw_lines.size() && trim_sv(raw_lines[k]).empty()) ++k;
                if (j < raw_lines.size() && k < raw_lines.size() && trim_sv(raw_lines[k]).rfind("#endif", 0) == 0) {
                    std::string mid = trim_sv(raw_lines[j]);
                    if (mid == "extern \"C\" {" || mid == "}" || mid == "};") {
                        li = k;
                        continue;
                    }
                }
            }
            if (!t.empty() && t[0] == '#') {
                std::string body = trim_sv(std::string_view(t).substr(1));
                if (body.empty()) continue;
                if (body.rfind("include", 0) == 0) continue;
                if (body.rfind("define", 0) == 0 && (body.size() == 6 || std::isspace(static_cast<unsigned char>(body[6])))) {
                    std::string rest = trim_sv(std::string_view(body).substr(6));
                    size_t np = 0;
                    while (np < rest.size() && is_identc(rest[np])) ++np;
                    if (np > 0 && is_ident0(rest[0]) && (np == rest.size() || rest[np] != '(')) {
                        std::string dname = rest.substr(0, np);
                        std::string drhs = trim_sv(std::string_view(rest).substr(np));
                        if (!drhs.empty()) {
                            auto v = eval_c_int(drhs, const_env);
                            if (v) {
                                const_env[dname] = *v;
                                if (fits_signed_i64(*v) && seen_consts.insert(dname).second)
                                    out.consts.emplace_back(dname, static_cast<int64_t>(*v));
                            } else {
                                obj_macros[dname] = drhs;
                            }
                        } else {
                            obj_macros[dname] = "";
                        }
                    }
                    continue;
                }
                if (body.rfind("undef", 0) == 0 && (body.size() == 5 || std::isspace(static_cast<unsigned char>(body[5])))) {
                    std::string uname = trim_sv(std::string_view(body).substr(5));
                    obj_macros.erase(uname);
                    continue;
                }
                if (body.rfind("pragma", 0) == 0 && (body.size() == 6 || std::isspace(static_cast<unsigned char>(body[6])))) {
                    std::string pr = trim_sv(std::string_view(body).substr(6));
                    if (pr == "once") continue;
                    if (pr.rfind("pack", 0) == 0) {
                        clean_lines.push_back("__BRAT_PRAGMA_PACK__ " + pr + ";");
                        continue;
                    }
                    if (strict_directives) {
                        err = "unsupported C preprocessor directive: #" + body;
                        return false;
                    }
                    continue;
                }
                if (strict_directives) {
                    err = "unsupported C preprocessor directive: #" + body;
                    return false;
                }
                continue;
            }
            clean_lines.push_back(expand_obj_macros(raw_lines[li]));
        }
    }

    std::string flat_src;
    for (const auto &l : clean_lines) {
        flat_src += l;
        flat_src.push_back('\n');
    }

    // Strip outer `extern "C" { ... }` blocks if present.
    for (;;) {
        size_t ec = flat_src.find("extern \"C\"");
        if (ec == std::string::npos) break;
        size_t p = ec + 10;
        while (p < flat_src.size() && std::isspace(static_cast<unsigned char>(flat_src[p]))) ++p;
        if (p < flat_src.size() && flat_src[p] == '{') {
            int d = 0;
            size_t q = p;
            while (q < flat_src.size()) {
                if (flat_src[q] == '{') ++d;
                else if (flat_src[q] == '}') { --d; if (d == 0) break; }
                ++q;
            }
            if (q < flat_src.size()) {
                flat_src[q] = ' ';
                flat_src.replace(ec, p - ec + 1, " ");
                continue;
            }
        }
        flat_src.replace(ec, 10, "extern ");
    }

    // Pass 2: walk top-level statements (`;` or `{...};` or inline function `{...}`).
    struct PendingEnum {
        std::string tag;
        std::string alias;
        std::string base;
        bool packed = false;
        std::vector<std::pair<std::string, CInt>> members;
    };
    std::vector<PendingEnum> pending_enums;
    std::map<std::string, std::vector<std::string>> tag_aliases;
    std::vector<int> pack_stack;
    int active_pack = 0;

    size_t pos = 0;
    while (pos < flat_src.size()) {
        while (pos < flat_src.size() && (std::isspace(static_cast<unsigned char>(flat_src[pos])) || flat_src[pos] == ';')) ++pos;
        if (pos >= flat_src.size()) break;

        if (flat_src.compare(pos, 20, "__BRAT_PRAGMA_PACK__") == 0) {
            size_t sc = flat_src.find(';', pos);
            std::string pr = trim_sv(std::string_view(flat_src).substr(pos + 20, (sc == std::string::npos ? flat_src.size() : sc) - (pos + 20)));
            pos = (sc == std::string::npos) ? flat_src.size() : sc + 1;
            size_t lp = pr.find('('), rp = pr.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
                if (strict_directives) { err = "unsupported C preprocessor directive: #pragma " + pr; return false; }
                continue;
            }
            std::string inside = trim_sv(std::string_view(pr).substr(lp + 1, rp - lp - 1));
            if (inside.empty()) {
                active_pack = 0;
            } else {
                auto args = split_top_level(inside, ',');
                auto parse_pack_n = [&](const std::string &ns) -> int {
                    if (ns == "1" || ns == "2" || ns == "4" || ns == "8" || ns == "16") return std::stoi(ns);
                    return -1;
                };
                if (args[0] == "push") {
                    pack_stack.push_back(active_pack);
                    if (args.size() >= 2) {
                        int n = parse_pack_n(args.back());
                        if (n < 0) {
                            if (strict_directives) { err = "unsupported C preprocessor directive: #pragma " + pr; return false; }
                        } else active_pack = n;
                    }
                } else if (args[0] == "pop") {
                    if (!pack_stack.empty()) { active_pack = pack_stack.back(); pack_stack.pop_back(); }
                    else active_pack = 0;
                } else if (args.size() == 1) {
                    int n = parse_pack_n(args[0]);
                    if (n < 0) {
                        if (strict_directives) { err = "unsupported C preprocessor directive: #pragma " + pr; return false; }
                    } else active_pack = n;
                } else if (strict_directives) {
                    err = "unsupported C preprocessor directive: #pragma " + pr;
                    return false;
                }
            }
            continue;
        }

        // Scan one top-level item
        size_t start = pos;
        int dp = 0, dc = 0;
        bool saw_brace = false;
        size_t first_brace = std::string::npos, matching_brace = std::string::npos;
        while (pos < flat_src.size()) {
            char c = flat_src[pos];
            if (c == '(' && dc == 0) ++dp;
            else if (c == ')' && dc == 0) --dp;
            else if (c == '{') {
                if (dc == 0 && !saw_brace) { saw_brace = true; first_brace = pos; }
                ++dc;
            } else if (c == '}') {
                --dc;
                if (dc == 0 && matching_brace == std::string::npos) {
                    matching_brace = pos;
                    // Look ahead: if this was an inline C function body `static inline int f(...) { ... }`, stop right at `}`!
                    std::string pre = trim_sv(std::string_view(flat_src).substr(start, first_brace - start));
                    if (pre.find('(') != std::string::npos && pre.rfind("typedef", 0) != 0 &&
                        pre.find("struct") == std::string::npos && pre.find("union") == std::string::npos &&
                        pre.find("enum") == std::string::npos) {
                        ++pos;
                        break;
                    }
                }
            } else if (c == ';' && dc == 0 && dp <= 0) {
                ++pos;
                break;
            }
            ++pos;
        }

        std::string item = trim_sv(std::string_view(flat_src).substr(start, pos - start));
        if (!item.empty() && item.back() == ';') item.pop_back();
        item = trim_sv(item);
        if (item.empty()) continue;

        if (strict_directives && has_aligned_attr(item)) {
            err = "unsupported C attribute: aligned";
            return false;
        }

        // Braced item: struct / union / enum / inline function
        if (saw_brace && first_brace != std::string::npos && matching_brace != std::string::npos) {
            size_t rel_ob = first_brace - start;
            size_t rel_cb = matching_brace - start;
            if (rel_cb > item.size()) continue;
            std::string head_raw = trim_sv(std::string_view(item).substr(0, rel_ob));
            std::string body_raw = item.substr(rel_ob + 1, rel_cb - rel_ob - 1);
            std::string tail_raw = (rel_cb + 1 < item.size()) ? trim_sv(std::string_view(item).substr(rel_cb + 1)) : "";

            // Inline function definition (`static inline int foo(int x) { return x; }`) -> skip!
            std::string head_clean = trim_sv(strip_c_attrs(head_raw));
            if (head_clean.find('(') != std::string::npos && head_clean.rfind("typedef", 0) != 0) {
                continue;
            }

            bool is_td = false;
            auto hw = ident_words(head_clean);
            size_t wi = 0;
            if (wi < hw.size() && hw[wi] == "typedef") { is_td = true; ++wi; }

            if (wi < hw.size() && hw[wi] == "enum") {
                ++wi;
                std::string etag, ebase;
                size_t colon = head_clean.find(':');
                if (colon != std::string::npos) {
                    ebase = trim_sv(std::string_view(head_clean).substr(colon + 1));
                    auto pre_w = ident_words(std::string_view(head_clean).substr(0, colon));
                    for (size_t k = 0; k < pre_w.size(); ++k)
                        if (pre_w[k] == "enum" && k + 1 < pre_w.size()) etag = pre_w[k + 1];
                } else if (wi < hw.size()) {
                    etag = hw[wi];
                }
                std::string ealias;
                if (is_td) {
                    auto tw = ident_words(strip_c_attrs(tail_raw));
                    if (!tw.empty()) ealias = tw.back();
                }
                bool epk = (has_packed_attr(head_raw) || has_packed_attr(tail_raw)) && ebase.empty();
                std::vector<std::pair<std::string, CInt>> emembers;
                CInt nxt = 0;
                for (const auto &raw_m : split_top_level(body_raw, ',')) {
                    std::string m = trim_sv(strip_c_attrs(raw_m));
                    if (m.empty()) continue;
                    size_t eq = m.find('=');
                    if (eq != std::string::npos) {
                        std::string nm = trim_sv(std::string_view(m).substr(0, eq));
                        std::string rhs = trim_sv(std::string_view(m).substr(eq + 1));
                        if (!is_valid_ident(nm)) continue;
                        auto v = eval_c_int(rhs, const_env);
                        if (!v) continue;
                        const_env[nm] = *v;
                        emembers.emplace_back(nm, *v);
                        nxt = *v + 1;
                    } else if (is_valid_ident(m)) {
                        const_env[m] = nxt;
                        emembers.emplace_back(m, nxt);
                        nxt = nxt + 1;
                    }
                }
                std::string ename = !ealias.empty() ? ealias : (!etag.empty() ? etag : "_enum");
                std::vector<std::pair<std::string, int64_t>> emembers_i64;
                for (const auto &[mn, mv] : emembers) {
                    if (fits_signed_i64(mv)) emembers_i64.emplace_back(mn, static_cast<int64_t>(mv));
                }
                if (!emembers_i64.empty() || !out.enums.count(ename)) out.enums[ename] = std::move(emembers_i64);
                std::string escalar = derive_enum_scalar(ebase, epk, emembers, out, tm);
                if (!etag.empty()) out.enum_types[etag] = escalar;
                if (!ealias.empty()) { out.enum_types[ealias] = escalar; out.aliases.insert(ealias); }
                for (const auto &[mn, mv] : emembers) {
                    if (fits_signed_i64(mv) && seen_consts.insert(mn).second)
                        out.consts.emplace_back(mn, static_cast<int64_t>(mv));
                }
                pending_enums.push_back({etag, ealias, ebase, epk, std::move(emembers)});
                continue;
            }

            if (wi < hw.size() && (hw[wi] == "struct" || hw[wi] == "union")) {
                bool is_union = (hw[wi] == "union");
                ++wi;
                std::string tag = (wi < hw.size()) ? hw[wi] : "";
                std::string alias;
                std::string tail_clean = trim_sv(strip_c_attrs(tail_raw));
                if (is_td && !tail_clean.empty()) {
                    // Handle `typedef struct z_stream_s { ... } z_stream, *z_streamp;`
                    for (const auto &tp : split_top_level(tail_clean, ',')) {
                        if (tp.find('*') == std::string::npos && is_valid_ident(tp)) {
                            alias = tp;
                            out.aliases.insert(tp);
                        } else if (tp.find('*') != std::string::npos) {
                            std::string ptype, pname;
                            std::string base_nm = !alias.empty() ? alias : (!tag.empty() ? ("struct " + tag) : "void");
                            if (split_c_decl_type_and_name(base_nm + " " + tp, const_env, ptype, pname, false) && !pname.empty()) {
                                out.aliases.insert(pname);
                                out.typedefs[pname] = ptype;
                            }
                        }
                    }
                }
                std::string primary = !alias.empty() ? alias : tag;
                if (!tag.empty()) out.tags.insert(tag);
                bool pk = has_packed_attr(head_raw) || has_packed_attr(tail_raw);
                bool al = has_aligned_attr(head_raw) || has_aligned_attr(tail_raw);
                if (!primary.empty()) {
                    parse_struct_body(primary, body_raw, is_union, pk, active_pack, out, const_env);
                    if (al) out.anon.insert(primary);
                    if (!tag.empty() && tag != primary) {
                        out.structs[tag] = out.structs[primary];
                        if (is_union) out.unions.insert(tag);
                        if (out.packed.count(primary)) out.packed.insert(tag);
                        if (out.pack_align.count(primary)) out.pack_align[tag] = out.pack_align[primary];
                        if (out.bitfields.count(primary)) out.bitfields[tag] = out.bitfields[primary];
                        if (out.anon.count(primary)) out.anon.insert(tag);
                        if (out.arrays.count(primary)) out.arrays.insert(tag);
                    }
                }
                continue;
            }
            continue;
        }

        // Unbraced statement: forward enum with `: base`, `typedef`, opaque `struct Foo;`, or function prototype!
        std::string abi_bad = detect_abi_attr(item);
        std::string clean_item = trim_sv(strip_c_attrs(item));
        if (clean_item.empty()) continue;

        // C23 forward enum `enum Mode : uint8_t;`
        if (clean_item.rfind("enum ", 0) == 0 && clean_item.find(':') != std::string::npos) {
            size_t col = clean_item.find(':');
            auto ew = ident_words(std::string_view(clean_item).substr(0, col));
            std::string ebase = trim_sv(std::string_view(clean_item).substr(col + 1));
            if (ew.size() == 2 && ew[0] == "enum") {
                pending_enums.push_back({ew[1], {}, ebase, false, {}});
                out.enum_types[ew[1]] = derive_enum_scalar(ebase, false, {}, out, tm);
            }
            continue;
        }

        // Typedef
        if (clean_item.rfind("typedef ", 0) == 0 || clean_item.rfind("typedef\t", 0) == 0) {
            std::string rest = trim_sv(std::string_view(clean_item).substr(7));
            std::string fn_t, fn_alias;
            if (parse_fnptr_decl(rest, fn_t, fn_alias) && !fn_alias.empty()) {
                out.aliases.insert(fn_alias);
                out.fn_typedefs[fn_alias] = fn_t;
                continue;
            }
            // Function typedef without pointer: `typedef void *(alloc_func)(void *, unsigned, unsigned);` or `typedef int cb_t(int);`
            if (rest.find('(') != std::string::npos) {
                // Normalize `typedef ret (*alias)(params)` or `typedef ret (alias)(params)`
                size_t lp1 = rest.find('(');
                size_t rp1 = rest.find(')', lp1);
                if (rp1 != std::string::npos && rest.find('(', rp1) != std::string::npos) {
                    std::string inside = trim_sv(std::string_view(rest).substr(lp1 + 1, rp1 - lp1 - 1));
                    if (is_valid_ident(inside)) {
                        std::string ptr_form = rest.substr(0, lp1 + 1) + "*" + inside + rest.substr(rp1);
                        if (parse_fnptr_decl(ptr_form, fn_t, fn_alias) && !fn_alias.empty()) {
                            out.aliases.insert(fn_alias);
                            out.fn_typedefs[fn_alias] = fn_t;
                        }
                    }
                }
                continue;
            }
            std::string t_target, t_alias;
            if (split_c_decl_type_and_name(rest, const_env, t_target, t_alias, false) && !t_alias.empty()) {
                out.aliases.insert(t_alias);
                auto tw = ident_words(t_target);
                if (t_target.find('*') == std::string::npos && tw.size() == 2 && (tw[0] == "struct" || tw[0] == "union")) {
                    out.tags.insert(tw[1]);
                    tag_aliases[tw[1]].push_back(t_alias);
                    if (!out.structs.count(t_alias)) out.structs[t_alias] = out.structs.count(tw[1]) ? out.structs[tw[1]] : std::vector<std::pair<std::string, std::string>>{};
                    if (tw[0] == "union" || out.unions.count(tw[1])) out.unions.insert(t_alias);
                } else if (t_target.find('*') == std::string::npos && tw.size() == 2 && tw[0] == "enum") {
                    out.enum_types[t_alias] = out.enum_types.count(tw[1]) ? out.enum_types[tw[1]] : "i32";
                } else {
                    out.typedefs[t_alias] = t_target;
                }
            }
            continue;
        }

        // Opaque forward `struct Foo;` or `union Bar;`
        {
            auto iw = ident_words(clean_item);
            if (clean_item.find('(') == std::string::npos && clean_item.find('*') == std::string::npos &&
                iw.size() == 2 && (iw[0] == "struct" || iw[0] == "union")) {
                out.tags.insert(iw[1]);
                if (!out.structs.count(iw[1])) out.structs[iw[1]] = {};
                if (iw[0] == "union") out.unions.insert(iw[1]);
                continue;
            }
        }

        // Function prototype `ret name(params)` (also handles function returning fnptr opaque skip cleanly)
        size_t lp = clean_item.find('(');
        if (lp != std::string::npos && clean_item.back() == ')') {
            // Find matching `(` for trailing `)`
            int d = 0;
            size_t match_lp = clean_item.size();
            for (size_t k = clean_item.size(); k > 0; --k) {
                if (clean_item[k - 1] == ')') ++d;
                else if (clean_item[k - 1] == '(') {
                    --d;
                    if (d == 0) { match_lp = k - 1; break; }
                }
            }
            if (match_lp < clean_item.size() && match_lp > 0) {
                std::string head = trim_sv(std::string_view(clean_item).substr(0, match_lp));
                // Skip if `head` itself contains `(` (e.g. `void (*signal(int, void (*)(int)))(int)`)
                if (head.find('(') != std::string::npos) continue;
                std::string ret_t, fn_name;
                if (!split_c_decl_type_and_name(head, const_env, ret_t, fn_name, false) || fn_name.empty()) continue;
                if (fn_name == "if" || fn_name == "while" || fn_name == "for" || fn_name == "switch" || fn_name == "return" || fn_name == "sizeof")
                    continue;
                if (ret_t.rfind("extern ", 0) == 0) ret_t = trim_sv(std::string_view(ret_t).substr(7));
                if (ret_t.rfind("static ", 0) == 0) continue;  // internal static helper declaration

                if (!abi_bad.empty()) out.abi_unsupported[fn_name] = abi_bad;

                std::string args_str = trim_sv(std::string_view(clean_item).substr(match_lp + 1, clean_item.size() - match_lp - 2));
                CFnDecl fn;
                fn.ret = ret_t;
                if (!args_str.empty() && args_str != "void") {
                    for (const auto &part : split_top_level(args_str, ',')) {
                        if (part.empty()) continue;
                        if (part == "...") { fn.variadic = true; continue; }
                        std::string pt, pn;
                        if (split_c_decl_type_and_name(part, const_env, pt, pn, true)) {
                            fn.params.push_back(pt);
                            fn.param_names.push_back(pn);
                        } else {
                            fn.params.push_back(part);
                            fn.param_names.emplace_back();
                        }
                    }
                }
                out.fns[fn_name] = std::move(fn);
            }
        }
    }

    // Re-resolve any fixed-underlying `enum E : MyTypedef` after typedefs are populated.
    for (const auto &pe : pending_enums) {
        if (pe.base.empty()) continue;
        std::string sc = derive_enum_scalar(pe.base, pe.packed, pe.members, out, tm);
        if (!pe.tag.empty()) out.enum_types[pe.tag] = sc;
        if (!pe.alias.empty()) out.enum_types[pe.alias] = sc;
    }
    for (const auto &[tag, als] : tag_aliases) {
        auto sit = out.structs.find(tag);
        if (sit == out.structs.end() || sit->second.empty()) continue;
        for (const auto &al : als) {
            if (out.structs[al].empty()) out.structs[al] = sit->second;
            if (out.unions.count(tag)) out.unions.insert(al);
            if (out.packed.count(tag)) out.packed.insert(al);
            if (out.pack_align.count(tag)) out.pack_align[al] = out.pack_align[tag];
            if (out.bitfields.count(tag)) out.bitfields[al] = out.bitfields[tag];
            if (out.anon.count(tag)) out.anon.insert(al);
            if (out.arrays.count(tag)) out.arrays.insert(al);
        }
    }
    return true;
}

namespace {

static ResolvedCType resolve_c_type_depth(const CDecl *cdecl, std::string_view raw_type, const TargetModel &tm, int depth) {
    if (depth > 32) return {"invalid", {}, {}, {}, 0};
    std::string t = trim_sv(strip_c_attrs(raw_type));
    if (t.empty()) return {"invalid", {}, {}, {}, 0};

    // Inline C array `T[N]` (including constant expressions and multi-dimensional `T[M][N]`)
    if (t.back() == ']' && t.find('[') != std::string::npos) {
        size_t lb = t.rfind('[');
        std::string elem = trim_sv(std::string_view(t).substr(0, lb));
        std::string cnt_s = trim_sv(std::string_view(t).substr(lb + 1, t.size() - lb - 2));
        if (!elem.empty()) {
            if (auto cnt = eval_pos_dim(cnt_s, cdecl)) {
                auto er = resolve_c_type_depth(cdecl, elem, tm, depth + 1);
                if (er.kind == "array") {
                    int64_t total = 0;
                    if (!__builtin_mul_overflow(*cnt, er.arr_count, &total) && total <= (1LL << 30))
                        return {"array", {}, er.arr_elem_k, er.arr_elem_e, total};
                    return {"array_unsized", t, {}, {}, 0};
                }
                return {"array", {}, er.kind, er.extra, *cnt};
            }
        }
        return {"array_unsized", t, {}, {}, 0};
    }

    std::string cb_ret;
    std::vector<std::string> cb_params;
    if (parse_c_callback(t, cdecl, cb_ret, cb_params)) {
        auto words = ident_words(t);
        std::string cleaned;
        for (const auto &w : words) {
            if (w == "const" || w == "volatile" || w == "register") continue;
            if (!cleaned.empty()) cleaned.push_back(' ');
            cleaned += w;
        }
        if (cdecl && cdecl->fn_typedefs.count(cleaned)) return {"fn", cdecl->fn_typedefs.at(cleaned), {}, {}, 0};
        return {"fn", t, {}, {}, 0};
    }

    auto b = base_c_type(t, tm);
    if (!cdecl) return b;

    if (b.kind == "typedef") {
        if (cdecl->fn_typedefs.count(b.extra)) return {"fn", cdecl->fn_typedefs.at(b.extra), {}, {}, 0};
        if (cdecl->enum_types.count(b.extra)) return {cdecl->enum_types.at(b.extra), {}, {}, {}, 0};
        if (cdecl->typedefs.count(b.extra)) {
            std::string target = cdecl->typedefs.at(b.extra);
            // Preserve `const` on typedef alias of pointer (`const char_ptr_t` where `typedef char *char_ptr_t`? No: in C `const char_ptr_t` means `char *const`, NOT `const char *`! Whereas `typedef const char *cstr_t` already has `const` in `target`!)
            return resolve_c_type_depth(cdecl, target, tm, depth + 1);
        }
        if (cdecl->structs.count(b.extra)) return {"struct", b.extra, {}, {}, 0};
    }
    if (b.kind == "i32" && !b.extra.empty() && cdecl->enum_types.count(b.extra)) {
        return {cdecl->enum_types.at(b.extra), {}, {}, {}, 0};
    }
    if (b.kind == "ptr" && !b.extra.empty()) {
        if (cdecl->enum_types.count(b.extra)) return {"ptr", cdecl->enum_types.at(b.extra), {}, {}, 0};
        if (cdecl->fn_typedefs.count(b.extra)) return {"ptr", {}, {}, {}, 0};  // pointer to fnptr = opaque `ptr`
        if (cdecl->typedefs.count(b.extra)) {
            const std::string &td = cdecl->typedefs.at(b.extra);
            if (td.find('*') != std::string::npos) {
                return {"ptr", {}, {}, {}, 0};  // pointer to pointer typedef = multi-level `ptr`
            }
            // Check if `const` appeared before `*` on a `char` typedef (e.g. `const charf *`)
            auto tr = resolve_c_type_depth(cdecl, td, tm, depth + 1);
            if (tr.kind == "u8" && is_const_pointee(t, cdecl)) {
                auto td_w = ident_words(td);
                for (const auto &w : td_w) if (w == "char") return {"str", {}, {}, {}, 0};
            }
            if (tr.kind == "none") return {"ptr", {}, {}, {}, 0, true};
            if (tr.kind == "struct") return {"ptr", tr.extra, {}, {}, 0};
            if (brat_abi_width(tr.kind, tm).has_value() || tr.kind == "usize") return {"ptr", tr.kind, {}, {}, 0};
        }
    }
    return b;
}

}  // namespace

ResolvedCType resolve_c_type(const CDecl *cdecl, std::string_view raw_type, const TargetModel &tm) {
    return resolve_c_type_depth(cdecl, raw_type, tm, 0);
}

bool parse_c_callback(std::string_view ctype,
                      const CDecl *cdecl,
                      std::string &ret_out,
                      std::vector<std::string> &params_out) {
    std::string s = trim_sv(strip_c_attrs(ctype));
    if (cdecl) {
        auto words = ident_words(s);
        std::string cleaned;
        for (const auto &w : words) {
            if (w == "const" || w == "volatile" || w == "register") continue;
            if (!cleaned.empty()) cleaned.push_back(' ');
            cleaned += w;
        }
        if (s.find('*') == std::string::npos) {
            for (int hop = 0; hop < 16; ++hop) {
                auto fit = cdecl->fn_typedefs.find(cleaned);
                if (fit != cdecl->fn_typedefs.end()) { s = fit->second; break; }
                auto tit = cdecl->typedefs.find(cleaned);
                if (tit == cdecl->typedefs.end()) break;
                cleaned = trim_sv(tit->second);
                s = cleaned;
            }
        }
    }
    std::string norm, nm;
    if (!parse_fnptr_decl(s, norm, nm)) return false;
    size_t lp1 = norm.find("(*)(");
    if (lp1 == std::string::npos) return false;
    ret_out = trim_sv(std::string_view(norm).substr(0, lp1));
    std::string inside = trim_sv(std::string_view(norm).substr(lp1 + 4, norm.size() - (lp1 + 4) - 1));
    params_out.clear();
    if (!inside.empty() && inside != "void") {
        for (const auto &p : split_top_level(inside, ','))
            if (!p.empty()) params_out.push_back(p);
    }
    return true;
}

bool parse_brat_callback(std::string_view ann,
                         std::string &ret_out,
                         std::vector<std::string> &params_out) {
    std::string s = trim_sv(ann);
    if (s.rfind("fn(", 0) != 0) return false;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = 2; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            --depth;
            if (depth == 0) { close = i; break; }
        }
    }
    if (close == std::string::npos) return false;
    std::string inside = trim_sv(std::string_view(s).substr(3, close - 3));
    std::string rest = trim_sv(std::string_view(s).substr(close + 1));
    params_out.clear();
    if (!inside.empty()) {
        for (const auto &p : split_top_level(inside, ','))
            if (!p.empty()) params_out.push_back(p);
    }
    if (rest.empty()) {
        ret_out = "none";
        return true;
    }
    if (rest.rfind("->", 0) == 0) {
        ret_out = trim_sv(std::string_view(rest).substr(2));
        return !ret_out.empty();
    }
    return false;
}

bool is_const_pointee(std::string_view raw_ct, const CDecl *cdecl) {
    std::string ct = trim_sv(strip_c_attrs(raw_ct));
    if (ct.find('*') != std::string::npos) {
        // Split at the LAST `*`: the immediate pointee `*p` is qualified by the segment right before the last `*`.
        // - `const char *`   -> segment `"const char "` -> true
        // - `char *const`    -> segment `"char "`       -> false
        // - `const char **`  -> segment `""`            -> false (`*pp` is a writable `const char *`!)
        // - `char *const *`  -> segment `"const "`      -> true
        size_t last_star = ct.rfind('*');
        size_t prev_star = (last_star == 0) ? std::string::npos : ct.rfind('*', last_star - 1);
        std::string_view seg = (prev_star == std::string::npos)
            ? std::string_view(ct).substr(0, last_star)
            : std::string_view(ct).substr(prev_star + 1, last_star - prev_star - 1);
        for (const auto &w : ident_words(seg))
            if (w == "const") return true;
        return false;
    }
    if (cdecl) {
        auto words = ident_words(ct);
        std::string cleaned;
        for (const auto &w : words) {
            if (w == "const" || w == "volatile" || w == "register") continue;
            if (!cleaned.empty()) cleaned.push_back(' ');
            cleaned += w;
        }
        auto it = cdecl->typedefs.find(cleaned);
        if (it != cdecl->typedefs.end()) return is_const_pointee(it->second, cdecl);
    }
    return false;
}

std::optional<std::pair<std::string, int64_t>> parse_array_field_type(std::string_view t) {
    std::string s = trim_sv(t);
    if (s.rfind("ptr[", 0) == 0 || s.find('[') == std::string::npos || s.back() != ']') return std::nullopt;
    size_t lb = s.find('[');
    std::string elem = trim_sv(std::string_view(s).substr(0, lb));
    std::string cnt = trim_sv(std::string_view(s).substr(lb + 1, s.size() - lb - 2));
    if (elem.empty()) return std::nullopt;
    auto n = parse_pos_dim(cnt);
    if (!n) return std::nullopt;
    return std::make_pair(elem, *n);
}

std::string element_of_ptr(std::string_view t) {
    std::string s = trim_sv(t);
    if (s.rfind("ptr[", 0) == 0 && s.back() == ']')
        return trim_sv(std::string_view(s).substr(4, s.size() - 5));
    return {};
}

std::optional<int64_t> brat_abi_width(std::string_view t, const TargetModel &tm) {
    if (t == "bool" || t == "i8" || t == "u8") return 1;
    if (t == "i16" || t == "u16") return 2;
    if (t == "i32" || t == "u32" || t == "f32") return 4;
    if (t == "i64" || t == "u64" || t == "f64") return 8;
    if (t == "usize" || t == "ptr" || t == "str" || t.rfind("ptr[", 0) == 0 || t.rfind("fn(", 0) == 0)
        return tm.ptr;
    return std::nullopt;
}

bool same_brat_width_type(std::string_view a, std::string_view b, const TargetModel &tm) {
    if (a == b) return true;
    std::string u_sz = (tm.ptr == 8) ? "u64" : "u32";
    if ((a == "usize" && b == u_sz) || (b == "usize" && a == u_sz)) return true;
    return false;
}

bool pointee_ok(std::string_view elem,
                std::string_view cextra,
                const std::set<std::string> &brat_structs,
                const TargetModel &tm) {
    if (elem.empty()) return true;   // plain `ptr`: no promise to keep
    if (cextra.empty()) return false;   // `void *`, `T **`: the promise cannot be confirmed
    if (same_brat_width_type(elem, cextra, tm)) return true;
    if (elem == cextra && brat_structs.count(std::string(elem))) return true;
    if ((elem == "u8" && cextra == "i8") || (elem == "i8" && cextra == "u8")) return true;
    return false;
}

namespace {
static int64_t align_up_i64(int64_t off, int64_t al) {
    return (al <= 1) ? off : ((off + al - 1) / al) * al;
}

static int64_t scalar_nat_align(std::string_view k, int64_t sz, const TargetModel &tm) {
    if ((k == "i64" || k == "u64" || k == "f64") && tm.wide_align == 4) return 4;
    return sz;
}
}  // namespace

std::map<std::string, RecordLayout> compute_brat_struct_layout(
    const std::map<std::string, std::vector<std::pair<std::string, std::string>>> &structs,
    const std::set<std::string> &packed,
    const std::set<std::string> &unions,
    const TargetModel &tm) {
    std::map<std::string, RecordLayout> out;
    std::set<std::string> visiting;

    auto measure = [&](auto &self, const std::string &name) -> std::optional<RecordLayout> {
        if (out.count(name)) return out[name];
        if (!structs.count(name) || visiting.count(name)) return std::nullopt;
        const auto &fields = structs.at(name);
        if (fields.empty()) return std::nullopt;
        visiting.insert(name);
        bool is_pk = packed.count(name) > 0;
        bool is_un = unions.count(name) > 0;
        int64_t off = 0, max_end = 0, max_al = 1;
        std::vector<std::pair<std::string, int64_t>> slots;

        std::function<std::optional<std::pair<int64_t, int64_t>>(const std::string &)> field_sa =
            [&](const std::string &ft) -> std::optional<std::pair<int64_t, int64_t>> {
            if (auto arr = parse_array_field_type(ft)) {
                auto sub = field_sa(arr->first);
                if (!sub) return std::nullopt;
                int64_t total_sz = 0;
                if (__builtin_mul_overflow(sub->first, arr->second, &total_sz) || total_sz > (1LL << 40))
                    return std::nullopt;
                return std::make_pair(total_sz, sub->second);
            }
            if (auto w = brat_abi_width(ft, tm)) {
                return std::make_pair(*w, is_pk ? int64_t(1) : scalar_nat_align(ft, *w, tm));
            }
            if (structs.count(ft)) {
                auto sub = self(self, ft);
                if (!sub) return std::nullopt;
                return std::make_pair(sub->size, is_pk ? int64_t(1) : sub->align);
            }
            return std::nullopt;
        };

        for (const auto &[fnm, ft] : fields) {
            auto sa = field_sa(ft);
            if (!sa) { visiting.erase(name); return std::nullopt; }
            auto [sz, al] = *sa;
            if (is_un) {
                slots.emplace_back(fnm, 0);
                max_end = std::max(max_end, sz);
                max_al = std::max(max_al, al);
            } else {
                off = align_up_i64(off, al);
                slots.emplace_back(fnm, off);
                off += sz;
                max_al = std::max(max_al, al);
            }
        }
        visiting.erase(name);
        RecordLayout rl;
        rl.align = is_pk ? 1 : max_al;
        rl.size = align_up_i64(is_un ? max_end : off, rl.align);
        rl.slots = std::move(slots);
        out[name] = rl;
        return out[name];
    };

    for (const auto &[nm, _] : structs) measure(measure, nm);
    return out;
}

std::map<std::string, RecordLayout> compute_c_struct_layout(const CDecl &cdecl,
                                                            const TargetModel &tm) {
    std::map<std::string, RecordLayout> out;
    std::set<std::string> visiting;

    auto measure = [&](auto &self, const std::string &name) -> std::optional<RecordLayout> {
        if (out.count(name)) return out[name];
        if (!cdecl.structs.count(name) || visiting.count(name)) return std::nullopt;
        if (cdecl.anon.count(name) || cdecl.bitfields.count(name)) return std::nullopt;
        const auto &fields = cdecl.structs.at(name);
        if (fields.empty()) return std::nullopt;
        visiting.insert(name);
        bool is_pk = cdecl.packed.count(name) > 0;
        bool is_un = cdecl.unions.count(name) > 0;
        int pack_cap = cdecl.pack_align.count(name) ? cdecl.pack_align.at(name) : 0;

        auto cap_al = [&](int64_t a) -> int64_t {
            if (is_pk) return 1;
            if (pack_cap > 0) return std::min<int64_t>(a, pack_cap);
            return a;
        };

        std::function<std::optional<std::pair<int64_t, int64_t>>(const ResolvedCType &)> res_sa =
            [&](const ResolvedCType &rc) -> std::optional<std::pair<int64_t, int64_t>> {
            if (rc.kind == "array") {
                ResolvedCType sub{rc.arr_elem_k, rc.arr_elem_e, {}, {}, 0};
                if (rc.arr_elem_k == "array") return std::nullopt;
                auto es = res_sa(sub);
                if (!es) return std::nullopt;
                int64_t total_sz = 0;
                if (__builtin_mul_overflow(es->first, rc.arr_count, &total_sz) || total_sz > (1LL << 40))
                    return std::nullopt;
                return std::make_pair(total_sz, es->second);
            }
            if (rc.kind == "fn") return std::make_pair<int64_t, int64_t>(tm.ptr, cap_al(tm.ptr));
            if (auto w = brat_abi_width(rc.kind, tm)) {
                return std::make_pair(*w, cap_al(scalar_nat_align(rc.kind, *w, tm)));
            }
            if (rc.kind == "struct") {
                auto sub = self(self, rc.extra);
                if (!sub) return std::nullopt;
                return std::make_pair(sub->size, cap_al(sub->align));
            }
            return std::nullopt;
        };

        int64_t off = 0, max_end = 0, max_al = 1;
        std::vector<std::pair<std::string, int64_t>> slots;
        for (const auto &[fnm, ctype] : fields) {
            auto rc = resolve_c_type(&cdecl, ctype, tm);
            // Multi-dimensional C array `int m[2][3]`
            std::optional<std::pair<int64_t, int64_t>> sa;
            if (ctype.find('[') != std::string::npos && ctype.back() == ']') {
                int64_t mult = 1;
                std::string cur = ctype;
                bool ok_dims = true;
                while (!cur.empty() && cur.back() == ']') {
                    size_t lb = cur.rfind('[');
                    if (lb == std::string::npos) { ok_dims = false; break; }
                    std::string ds = trim_sv(std::string_view(cur).substr(lb + 1, cur.size() - lb - 2));
                    auto dm = eval_pos_dim(ds, &cdecl);
                    if (!dm || __builtin_mul_overflow(mult, *dm, &mult) || mult > (1LL << 36)) {
                        ok_dims = false;
                        break;
                    }
                    cur = trim_sv(std::string_view(cur).substr(0, lb));
                }
                if (ok_dims) {
                    auto base_rc = resolve_c_type(&cdecl, cur, tm);
                    auto bs = res_sa(base_rc);
                    int64_t total_sz = 0;
                    if (bs && !__builtin_mul_overflow(bs->first, mult, &total_sz) && total_sz <= (1LL << 40))
                        sa = std::make_pair(total_sz, bs->second);
                }
            } else {
                sa = res_sa(rc);
            }
            if (!sa) { visiting.erase(name); return std::nullopt; }
            auto [sz, al] = *sa;
            if (is_un) {
                slots.emplace_back(fnm, 0);
                max_end = std::max(max_end, sz);
                max_al = std::max(max_al, al);
            } else {
                off = align_up_i64(off, al);
                slots.emplace_back(fnm, off);
                off += sz;
                max_al = std::max(max_al, al);
            }
        }
        visiting.erase(name);
        RecordLayout rl;
        rl.align = cap_al(max_al);
        rl.size = align_up_i64(is_un ? max_end : off, rl.align);
        rl.slots = std::move(slots);
        out[name] = rl;
        return out[name];
    };

    for (const auto &[nm, _] : cdecl.structs) measure(measure, nm);
    return out;
}

std::set<std::string> platform_width_uses(const CDecl &cdecl) {
    static const std::set<std::string> kDriftWords = {
        "long", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "off_t", "time_t", "uLong", "uLongf"
    };
    auto has_drift = [&](const std::string &ct, auto &self, int depth) -> bool {
        if (depth > 8) return false;
        auto ws = ident_words(ct);
        int longs = 0;
        for (const auto &w : ws) if (w == "long") ++longs;
        if (longs == 1) return true;  // `long` / `unsigned long` (NOT `long long`!)
        for (const auto &w : ws) {
            if (w != "long" && kDriftWords.count(w)) return true;
            auto it = cdecl.typedefs.find(w);
            if (it != cdecl.typedefs.end() && self(it->second, self, depth + 1)) return true;
        }
        return false;
    };
    std::set<std::string> res;
    for (const auto &[fnm, fn] : cdecl.fns) {
        if (has_drift(fn.ret, has_drift, 0)) res.insert("fn " + fnm);
        for (const auto &p : fn.params)
            if (has_drift(p, has_drift, 0)) res.insert("fn " + fnm);
    }
    for (const auto &[snm, fields] : cdecl.structs) {
        for (const auto &[_, ft] : fields)
            if (has_drift(ft, has_drift, 0)) res.insert("struct " + snm);
    }
    return res;
}

}  // namespace native_ffi



