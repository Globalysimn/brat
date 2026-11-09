#include "pkg_archive.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ffi_native.h"
#include "parser.h"
#include "pkg_io.h"

namespace pkg_archive {

namespace {

std::string ascii_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_win_device(std::string_view low) {
    if (low == "con" || low == "prn" || low == "aux" || low == "nul") return true;
    return low.size() == 4 && (low.rfind("com", 0) == 0 || low.rfind("lpt", 0) == 0) &&
           low[3] >= '1' && low[3] <= '9';
}

bool valid_utf8(std::string_view s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<uint8_t>(s[i]);
        if (c < 0x80u) { ++i; continue; }
        size_t need = 0;
        uint32_t cp = 0, min_cp = 0;
        if ((c & 0xE0u) == 0xC0u) { need = 1; cp = c & 0x1Fu; min_cp = 0x80u; }
        else if ((c & 0xF0u) == 0xE0u) { need = 2; cp = c & 0x0Fu; min_cp = 0x800u; }
        else if ((c & 0xF8u) == 0xF0u) { need = 3; cp = c & 0x07u; min_cp = 0x10000u; }
        else return false;
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; ++k) {
            const auto b = static_cast<uint8_t>(s[i + k]);
            if ((b & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        i += need + 1;
    }
    return true;
}

std::string norm_crlf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out.push_back(s[i]);
    }
    return out;
}

std::string_view archive_file_ext(std::string_view filename) {
    static constexpr std::string_view exts[] = {
        ".bratbind", ".dylib", ".brat", ".bri", ".obj", ".lib", ".dll", ".so", ".a", ".o", ".c", ".h"};
    for (std::string_view ext : exts) {
        if (filename.size() > ext.size() &&
            filename.substr(filename.size() - ext.size()) == ext)
            return ext;
    }
    return {};
}

bool is_text_archive_ext(std::string_view filename) {
    static constexpr std::string_view exts[] = {".bratbind", ".brat", ".bri", ".c", ".h"};
    for (std::string_view ext : exts) {
        if (filename.size() > ext.size() &&
            filename.substr(filename.size() - ext.size()) == ext)
            return true;
    }
    return false;
}

bool is_bin_archive_ext(std::string_view filename) {
    static constexpr std::string_view exts[] = {".dylib", ".obj", ".lib", ".dll", ".so", ".a", ".o"};
    for (std::string_view ext : exts) {
        if (filename.size() > ext.size() &&
            filename.substr(filename.size() - ext.size()) == ext)
            return true;
    }
    return false;
}

std::string percent_decode(std::string_view s) {
    auto hex_v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_v(s[i + 1]);
            const int lo = hex_v(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string url_path_only(std::string_view url) {
    const size_t scheme_sep = url.find("://");
    std::string_view rest = url;
    if (scheme_sep != std::string_view::npos) {
        rest = url.substr(scheme_sep + 3);
        const std::string scheme = ascii_lower(url.substr(0, scheme_sep));
        if (scheme != "file") {
            const size_t slash = rest.find_first_of("/?#");
            if (slash == std::string_view::npos || rest[slash] != '/') return {};
            rest = rest.substr(slash);
        }
    } else if (ascii_lower(url.substr(0, 5)) == "file:") {
        rest = url.substr(5);
    }
    const size_t qf = rest.find_first_of("?#");
    if (qf != std::string_view::npos) rest = rest.substr(0, qf);
    return percent_decode(rest);
}

bool valid_gh_ident(std::string_view s, bool allow_slash) {
    if (s.empty() || s.size() > 64) return false;
    const auto c0 = static_cast<unsigned char>(s[0]);
    if (!std::isalnum(c0)) return false;
    if (allow_slash && s.find("..") != std::string_view::npos) return false;
    for (char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        if (std::isalnum(u) || ch == '_' || ch == '.' || ch == '-') continue;
        if (allow_slash && ch == '/') continue;
        return false;
    }
    return true;
}

std::string sanitize_module_stem(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    for (char ch : raw) {
        const auto u = static_cast<unsigned char>(ch);
        out.push_back((std::isalnum(u) || ch == '_') ? ch : '_');
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(0, "pkg_");
    return out;
}

std::string strip_version_tail(std::string_view stem) {
    for (size_t i = 1; i + 1 < stem.size(); ++i) {
        if (stem[i] != '-') continue;
        size_t p = i + 1;
        if (p < stem.size() && (stem[p] == 'v' || stem[p] == 'V')) ++p;
        if (p >= stem.size() || !std::isdigit(static_cast<unsigned char>(stem[p]))) continue;
        while (p < stem.size() && std::isdigit(static_cast<unsigned char>(stem[p]))) ++p;
        bool ok = true;
        while (p < stem.size()) {
            if (stem[p] != '.') { ok = false; break; }
            ++p;
            if (p >= stem.size()) { ok = false; break; }
            while (p < stem.size() &&
                   (std::isalnum(static_cast<unsigned char>(stem[p])) ||
                    stem[p] == '_' || stem[p] == '.' || stem[p] == '-')) {
                ++p;
            }
        }
        if (ok && p == stem.size()) return std::string(stem.substr(0, i));
    }
    return std::string(stem);
}

bool lower_hex_sha(std::string_view v) {
    if (v.size() != 64) return false;
    for (char c : v)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool is_safe_header_filename(std::string_view name) {
    if (name.size() <= 2 || name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos || name.front() == '.')
        return false;
    const bool ok_ext = (name.size() > 2 && name.substr(name.size() - 2) == ".h") ||
                        (name.size() > 4 && name.substr(name.size() - 4) == ".hpp");
    if (!ok_ext) return false;
    static const std::set<std::string> reserved = {
        "assert.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h",
        "iso646.h", "limits.h", "locale.h", "math.h", "pthread.h", "sched.h",
        "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h",
        "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h",
        "stdnoreturn.h", "string.h", "strings.h", "tgmath.h", "threads.h",
        "time.h", "uchar.h", "unistd.h", "wchar.h", "wctype.h", "crt.h",
    };
    if (reserved.contains(ascii_lower(name))) return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool has_extern_stmts(const std::vector<StmtP> &body) {
    for (const auto &sp : body) {
        if (!sp) continue;
        if (sp->k == SK::Extern) return true;
    }
    return false;
}

struct ScopedTempDir {
    std::filesystem::path path;
    bool ok = false;
    explicit ScopedTempDir(std::string_view prefix) {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
#if defined(_WIN32)
        static unsigned long seq = 0;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto cand = base / (std::string(prefix) + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++seq));
            if (std::filesystem::create_directory(cand, ec)) {
                std::filesystem::permissions(cand, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace, ec);
                path = std::move(cand);
                ok = true;
                return;
            }
        }
#else
        std::string tpl = (base / (std::string(prefix) + "XXXXXX")).string();
        std::vector<char> buf(tpl.begin(), tpl.end());
        buf.push_back('\0');
        if (char *res = mkdtemp(buf.data())) {
            path = res;
            ok = true;
        }
#endif
    }
    ~ScopedTempDir() {
        if (ok) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

int cc_timeout_ms() {
    if (const char *ev = std::getenv("BRAT_CC_TIMEOUT_MS"); ev && *ev) {
        char *end = nullptr;
        const long long v = std::strtoll(ev, &end, 10);
        if (end != ev && *end == '\0' && v > 0 && v <= 3600000LL) return static_cast<int>(v);
    }
    return 120000;
}

#if defined(_WIN32)
std::wstring win_quote_arg(std::string_view arg) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, arg.data(), static_cast<int>(arg.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, wn)), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, arg.data(), static_cast<int>(arg.size()), w.data(), wn);
    if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) return w;
    std::wstring out = L"\"";
    for (size_t i = 0; i < w.size(); ++i) {
        size_t bs = 0;
        while (i < w.size() && w[i] == L'\\') { ++bs; ++i; }
        if (i == w.size()) {
            out.append(bs * 2, L'\\');
            break;
        }
        if (w[i] == L'"') {
            out.append(bs * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(bs, L'\\');
            out.push_back(w[i]);
        }
    }
    out.push_back(L'"');
    return out;
}
#endif

bool exec_capture(const std::vector<std::string> &args, int &exit_code, std::string &output) {
    output.clear();
    if (args.empty()) return false;
    const int timeout_ms = cc_timeout_ms();
#if defined(_WIN32)
    std::wstring cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline.push_back(L' ');
        cmdline += win_quote_arg(args[i]);
    }
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
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
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, flags,
                        nullptr, nullptr, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        CloseHandle(rd);
        CloseHandle(wr);
        if (job) CloseHandle(job);
        return false;
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(wr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    bool over_cap = false;
    std::thread reader([&]() {
        char buf[4096];
        DWORD got = 0;
        size_t total = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            total += got;
            if (total > (4u << 20)) {
                over_cap = true;
                if (job) TerminateJobObject(job, 124);
                else TerminateProcess(pi.hProcess, 124);
                break;
            }
            if (output.size() < 65536) output.append(buf, got);
        }
    });
    const DWORD wrc = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    const bool timed_out = (wrc == WAIT_TIMEOUT);
    if (timed_out) {
        if (job) TerminateJobObject(job, 124);
        else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    if (timed_out || over_cap) CancelIoEx(rd, nullptr);
    if (reader.joinable()) reader.join();
    CloseHandle(rd);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (timed_out) {
        output = "compiler timed out";
        exit_code = 124;
        return true;
    }
    if (over_cap) {
        output = "compiler output exceeded 4 MiB limit";
        exit_code = 125;
        return true;
    }
    exit_code = static_cast<int>(code);
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
#if defined(SIGPIPE)
        signal(SIGPIPE, SIG_DFL);
#endif
        close(pipefd[0]);
        if (pipefd[1] >= 0 && pipefd[1] < 3) {
            int m = fcntl(pipefd[1], F_DUPFD_CLOEXEC, 3);
            if (m < 0) _exit(126);
            close(pipefd[1]);
            pipefd[1] = m;
        }
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            if (devnull == STDIN_FILENO) fcntl(STDIN_FILENO, F_SETFD, 0);
            else { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        std::vector<char *> av;
        av.reserve(args.size() + 1);
        for (const auto &a : args) av.push_back(const_cast<char *>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipefd[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    bool over_cap = false;
    size_t total_out = 0;
    char buf[4096];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { timed_out = true; break; }
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd {};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, static_cast<int>(std::min<long long>(rem, 100)));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
                while (!over_cap) {
                    const ssize_t dn = read(pipefd[0], buf, sizeof(buf));
                    if (dn <= 0) break;
                    total_out += static_cast<size_t>(dn);
                    if (total_out > (4u << 20)) { over_cap = true; break; }
                    if (output.size() < 65536) {
                        const size_t keep = std::min<size_t>(static_cast<size_t>(dn), 65536 - output.size());
                        output.append(buf, keep);
                    }
                }
                close(pipefd[0]);
                if (over_cap) {
                    output = "compiler output exceeded 4 MiB limit";
                    exit_code = 125;
                    return true;
                }
                exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
                return exit_code != 127;
            }
            continue;
        }
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total_out += static_cast<size_t>(n);
        if (total_out > (4u << 20)) { over_cap = true; break; }
        if (output.size() < 65536) {
            const size_t keep = std::min<size_t>(static_cast<size_t>(n), 65536 - output.size());
            output.append(buf, keep);
        }
    }
    close(pipefd[0]);
    if (timed_out || over_cap) {
        if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out) {
        output = "compiler timed out";
        exit_code = 124;
        return true;
    }
    if (over_cap) {
        output = "compiler output exceeded 4 MiB limit";
        exit_code = 125;
        return true;
    }
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return exit_code != 127;
#endif
}

bool find_c_compiler(std::vector<std::string> &cc_out, bool &is_zig, std::string &error) {
    if (const char *env_cc = std::getenv("BRAT_CC"); env_cc && *env_cc) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(env_cc, ec)) {
            cc_out = {env_cc};
        } else {
            cc_out.clear();
            std::string cur;
            char q = 0;
            for (size_t i = 0; env_cc[i]; ++i) {
                char c = env_cc[i];
                if (q) {
                    if (c == q) q = 0;
                    else cur.push_back(c);
                } else if (c == '\'' || c == '"') {
                    q = c;
                } else if (c == ' ' || c == '\t') {
                    if (!cur.empty()) { cc_out.push_back(std::move(cur)); cur.clear(); }
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) cc_out.push_back(std::move(cur));
        }
        if (cc_out.empty()) {
            error = "empty BRAT_CC";
            return false;
        }
        is_zig = std::filesystem::path(cc_out[0]).filename().string().rfind("zig", 0) == 0 &&
                 !(cc_out.size() >= 2 && cc_out[1] == "cc");
        return true;
    }
    for (const char *cand : {"cc", "gcc", "clang"}) {
        int rc = 0;
        std::string out;
        if (exec_capture({cand, "--version"}, rc, out) && rc == 0) {
            cc_out = {cand};
            is_zig = false;
            return true;
        }
    }
    error = "no C compiler found (set BRAT_CC)";
    return false;
}

bool parse_archive_manifest(std::string_view raw_bytes,
                            const std::string &pkg_name,
                            ArchiveManifest &out,
                            std::string &error) {
    out = {};
    if (!valid_utf8(raw_bytes)) {
        error = pkg_name + "/brat.pkg.json is invalid JSON: invalid UTF-8";
        return false;
    }
    lsp_wire::LspJsonTape tape;
    if (!tape.parse(raw_bytes)) {
        error = pkg_name + "/brat.pkg.json is invalid JSON: " + std::string(tape.error());
        return false;
    }
    const uint32_t root = tape.root();
    if (!tape.is_object(root)) {
        error = pkg_name + "/brat.pkg.json must be a JSON object";
        return false;
    }
    const uint32_t mname = tape.get(root, "name");
    if (mname != 0 && !tape.is_null(mname)) {
        if (!tape.is_string(mname) || tape.str_view(mname) != pkg_name) {
            error = pkg_name + "/brat.pkg.json declares mismatched name";
            return false;
        }
    }
    const uint32_t ver_node = tape.get(root, "version");
    if (ver_node != 0 && !tape.is_null(ver_node)) {
        if (!tape.is_string(ver_node)) {
            error = pkg_name + "/brat.pkg.json has invalid version";
            return false;
        }
        const std::string_view v = tape.str_view(ver_node);
        if (v.empty() || v.size() > 64 ||
            std::any_of(v.begin(), v.end(), [](unsigned char c) { return c < 33; })) {
            error = pkg_name + "/brat.pkg.json has invalid version";
            return false;
        }
        out.version.assign(v);
    }
    const uint32_t deps_node = tape.get(root, "dependencies");
    if (deps_node != 0 && !tape.is_null(deps_node)) {
        if (tape.is_array(deps_node)) {
            for (uint32_t cur = tape.at(deps_node).first_child; cur != 0;
                 cur = tape.at(cur).next_sibling) {
                if (tape.is_string(cur) && !tape.str_view(cur).empty()) {
                    out.dependencies.push_back({{}, std::string(tape.str_view(cur)), {}});
                } else if (tape.is_object(cur) && tape.is_string(tape.get(cur, "url")) &&
                           !tape.str_view(tape.get(cur, "url")).empty()) {
                    std::string k;
                    const uint32_t kn = tape.get(cur, "name");
                    if (kn != 0 && !tape.is_null(kn)) {
                        if (!tape.is_string(kn) || !valid_archive_name(tape.str_view(kn))) {
                            error = pkg_name + "/brat.pkg.json has invalid dependency name";
                            return false;
                        }
                        k.assign(tape.str_view(kn));
                    }
                    std::string sha;
                    const uint32_t sn = tape.get(cur, "sha256");
                    if (sn != 0 && !tape.is_null(sn)) {
                        if (!tape.is_string(sn) || !lower_hex_sha(tape.str_view(sn))) {
                            error = pkg_name + "/brat.pkg.json dependency has invalid sha256";
                            return false;
                        }
                        sha.assign(tape.str_view(sn));
                    }
                    out.dependencies.push_back({std::move(k),
                                                std::string(tape.str_view(tape.get(cur, "url"))),
                                                std::move(sha)});
                } else {
                    error = pkg_name + "/brat.pkg.json dependency list entries must be spec strings or {name, url, sha256}";
                    return false;
                }
            }
        } else if (tape.is_object(deps_node)) {
            for (uint32_t key = tape.at(deps_node).first_child; key != 0;) {
                const uint32_t val = tape.at(key).next_sibling;
                if (val == 0 || !tape.is_string(key)) {
                    error = pkg_name + "/brat.pkg.json `dependencies` is malformed";
                    return false;
                }
                const std::string k(tape.str_view(key));
                if (!valid_archive_name(k)) {
                    error = pkg_name + "/brat.pkg.json has invalid dependency name " + k;
                    return false;
                }
                if (tape.is_string(val) && !tape.str_view(val).empty()) {
                    out.dependencies.push_back({k, std::string(tape.str_view(val)), {}});
                } else if (tape.is_object(val) && tape.is_string(tape.get(val, "url")) &&
                           !tape.str_view(tape.get(val, "url")).empty()) {
                    std::string sha;
                    const uint32_t sn = tape.get(val, "sha256");
                    if (sn != 0 && !tape.is_null(sn)) {
                        if (!tape.is_string(sn) || !lower_hex_sha(tape.str_view(sn))) {
                            error = pkg_name + "/brat.pkg.json dependency " + k + " has invalid sha256";
                            return false;
                        }
                        sha.assign(tape.str_view(sn));
                    }
                    out.dependencies.push_back({k, std::string(tape.str_view(tape.get(val, "url"))), std::move(sha)});
                } else {
                    error = pkg_name + "/brat.pkg.json dependency " + k + " must be spec string or {url, sha256}";
                    return false;
                }
                key = tape.at(val).next_sibling;
            }
        } else {
            error = pkg_name + "/brat.pkg.json `dependencies` must be a list or object";
            return false;
        }
    }
    const uint32_t csrc_node = tape.get(root, "c_sources");
    if (csrc_node != 0 && !tape.is_null(csrc_node)) {
        if (!tape.is_array(csrc_node)) {
            error = pkg_name + "/brat.pkg.json `c_sources` must be a list of .c paths";
            return false;
        }
        std::set<std::string> seen;
        const std::string prefix = pkg_name + "/";
        for (uint32_t cs = tape.at(csrc_node).first_child; cs != 0; cs = tape.at(cs).next_sibling) {
            if (!tape.is_string(cs) || tape.str_view(cs).empty()) {
                error = pkg_name + "/brat.pkg.json `c_sources` entries must be non-empty strings";
                return false;
            }
            std::string norm_cs(tape.str_view(cs));
            if (norm_cs.rfind(prefix, 0) == 0) norm_cs.erase(0, prefix.size());
            const std::string full_rel = prefix + norm_cs;
            if (norm_cs.size() < 3 || norm_cs.substr(norm_cs.size() - 2) != ".c" ||
                !valid_archive_relpath(full_rel, pkg_name)) {
                error = pkg_name + "/brat.pkg.json has invalid c_sources path " + std::string(tape.str_view(cs));
                return false;
            }
            if (seen.insert(norm_cs).second) out.c_sources.push_back(norm_cs);
        }
    }
    return true;
}

bool verify_extracted_bratbinds(const std::map<std::string, std::string> &extracted,
                                const std::string &target,
                                const std::string &pkg_name,
                                const std::vector<std::string> &c_sources,
                                std::string &error) {
    std::map<std::string, std::string_view> binds, brats, bins, cfiles;
    for (const auto &[rel, data] : extracted) {
        if (rel.size() > 9 && rel.substr(rel.size() - 9) == ".bratbind") binds[rel] = data;
        else if (rel.size() > 5 && rel.substr(rel.size() - 5) == ".brat") brats[rel] = data;
        else if (is_bin_archive_ext(rel)) bins[rel] = data;
        else if ((rel.size() > 2 && rel.substr(rel.size() - 2) == ".c") ||
                 (rel.size() > 2 && rel.substr(rel.size() - 2) == ".h"))
            cfiles[rel] = data;
    }

    for (const auto &[brel, _] : binds) {
        const std::string mrel = brel.substr(0, brel.size() - 9) + ".brat";
        if (!brats.contains(mrel)) {
            error = brel + " has no matching " + mrel + " in archive";
            return false;
        }
    }

    std::vector<std::string> extern_mods;
    for (const auto &[mrel, mbytes] : brats) {
        ParseOut parsed;
        try {
            parsed = parse(std::string(mbytes));
        } catch (const std::exception &e) {
            error = mrel + ": " + e.what();
            return false;
        }
        if (has_extern_stmts(parsed.body)) extern_mods.push_back(mrel);
    }

    if (binds.empty() && extern_mods.empty() && c_sources.empty() && bins.empty() && cfiles.empty())
        return true;

    for (const auto &mrel : extern_mods) {
        const std::string brel = mrel.substr(0, mrel.size() - 5) + ".bratbind";
        if (!binds.contains(brel)) {
            error = mrel + " declares C externs without a " + brel + " proof";
            return false;
        }
    }
    if (!c_sources.empty() && binds.empty()) {
        error = pkg_name + " declares c_sources without a .bratbind proof";
        return false;
    }
    if ((!bins.empty() || !cfiles.empty()) && binds.empty()) {
        error = pkg_name + " contains native C/object artifacts without a .bratbind proof";
        return false;
    }

    std::string eff_target = target;
    if (eff_target.empty()) {
        if (const char *et = std::getenv("BRAT_TARGET"); et && *et) eff_target = et;
    }

    ScopedTempDir td("brat-bindchk-");
    if (!td.ok) {
        error = "cannot create temporary directory for archive FFI verification";
        return false;
    }

    std::map<std::string, std::map<std::string, std::string>> bin_paths_by_dir;
    auto write_rel = [&](const std::string &rel, std::string_view bytes, std::filesystem::path &out_p) -> bool {
        out_p = td.path / std::filesystem::path(rel);
        std::error_code ec;
        std::filesystem::create_directories(out_p.parent_path(), ec);
        if (ec) return false;
        std::ofstream f(out_p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    };

    for (const auto &[crel, cbytes] : cfiles) {
        std::filesystem::path cp;
        if (!write_rel(crel, cbytes, cp)) {
            error = "cannot stage " + crel;
            return false;
        }
    }
    for (const auto &[orel, obytes] : bins) {
        std::filesystem::path op;
        if (!write_rel(orel, obytes, op)) {
            error = "cannot stage " + orel;
            return false;
        }
        const size_t sl = orel.rfind('/');
        const std::string dkey = sl == std::string::npos ? std::string() : orel.substr(0, sl);
        const std::string fname = sl == std::string::npos ? orel : orel.substr(sl + 1);
        bin_paths_by_dir[dkey][fname] = op.string();
    }

    if (!c_sources.empty() && !pkg_name.empty()) {
        std::vector<std::string> cc_cmd;
        bool is_zig = false;
        if (!find_c_compiler(cc_cmd, is_zig, error)) {
            if (eff_target.empty()) {
                error = "cannot verify c_sources for " + pkg_name + ": " + error;
                return false;
            }
        } else if (eff_target.empty() || is_zig ||
                   (!cc_cmd.empty() && std::filesystem::path(cc_cmd[0]).filename().string().rfind("zig", 0) == 0)) {
            const bool zig_any = !cc_cmd.empty() &&
                                 std::filesystem::path(cc_cmd[0]).filename().string().rfind("zig", 0) == 0;
            const std::filesystem::path pkg_td = td.path / pkg_name;
            std::error_code ec;
            std::filesystem::create_directories(pkg_td, ec);
            for (const auto &[brel, bbytes] : binds) {
                lsp_wire::LspJsonTape bt;
                if (!bt.parse(bbytes) || !bt.is_object(bt.root())) {
                    error = "invalid JSON in " + brel;
                    return false;
                }
                const uint32_t hdr = bt.get(bt.root(), "header");
                if (bt.is_object(hdr) && bt.is_string(bt.get(hdr, "name")) &&
                    bt.is_string(bt.get(hdr, "text"))) {
                    const std::string raw_h(bt.str_view(bt.get(hdr, "name")));
                    const std::string hname = std::filesystem::path(raw_h).filename().string();
                    const std::string_view htext = bt.str_view(bt.get(hdr, "text"));
                    if (!hname.empty()) {
                        if (!is_safe_header_filename(hname)) {
                            error = "unsafe or reserved header name '" + hname + "' in " + brel;
                            return false;
                        }
                        const auto hp = pkg_td / hname;
                        if (!std::filesystem::exists(hp, ec)) {
                            std::ofstream hf(hp, std::ios::binary | std::ios::trunc);
                            hf.write(htext.data(), static_cast<std::streamsize>(htext.size()));
                        }
                    }
                }
            }
            std::string cs_bundle_path;
            std::vector<std::string> built_objs;
            size_t cs_idx = 0;
            for (const auto &cs : c_sources) {
                const std::string rel_cs = pkg_name + "/" + cs;
                if (!valid_archive_relpath(rel_cs, pkg_name)) {
                    error = "unsafe c_source path " + cs + " in " + pkg_name;
                    return false;
                }
                const std::filesystem::path cpath = td.path / std::filesystem::path(rel_cs);
                const std::filesystem::path opath = td.path / ("__cs_" + std::to_string(cs_idx++) + ".o");
                std::vector<std::string> cmd = cc_cmd;
                if (is_zig) cmd.push_back("cc");
                if (zig_any && !eff_target.empty()) cmd.push_back("--target=" + eff_target);
                cmd.insert(cmd.end(), {"-c", "-g", cpath.string(), "-o", opath.string(),
                                       "-std=gnu11", "-O2", "-fwrapv", "-I", pkg_td.string()});
                int rc = 0;
                std::string cc_out;
                if (!exec_capture(cmd, rc, cc_out) || rc != 0) {
                    if (cc_out.size() > 400) cc_out.resize(400);
                    error = "c_sources compile failed for " + rel_cs + ": " + cc_out;
                    return false;
                }
                built_objs.push_back(opath.string());
            }
            if (!built_objs.empty()) {
                cs_bundle_path = (td.path / "__c_sources_bundle__.a").string();
                if (!pkg_io::write_ar_bundle(cs_bundle_path, built_objs, error)) return false;
                for (const auto &[brel, _] : binds) {
                    const std::string mrel = brel.substr(0, brel.size() - 9) + ".brat";
                    const size_t sl = mrel.rfind('/');
                    const std::string dkey = sl == std::string::npos ? std::string() : mrel.substr(0, sl);
                    bin_paths_by_dir[dkey]["__c_sources_bundle__.a"] = cs_bundle_path;
                }
            }
        }
    }

    for (const auto &[brel, bbytes] : binds) {
        const std::string mrel = brel.substr(0, brel.size() - 9) + ".brat";
        const size_t sl = mrel.rfind('/');
        const std::string dkey = sl == std::string::npos ? std::string() : mrel.substr(0, sl);
        const auto d_it = bin_paths_by_dir.find(dkey);
        const std::map<std::string, std::string> empty_objs;
        const auto &objs = d_it == bin_paths_by_dir.end() ? empty_objs : d_it->second;
        if (!native_ffi::verify_bratbind_bytes(brats[mrel], bbytes, eff_target, objs, brel, error))
            return false;
    }
    return true;
}

bool archive_spec_info(std::string_view spec,
                       std::string &pkg_name,
                       std::string &url,
                       std::string &ref,
                       bool &is_local,
                       std::string &error) {
    ref.clear();
    if (spec.rfind("github.com/", 0) == 0) {
        is_local = false;
        return expand_github_spec(spec, pkg_name, url, ref, error);
    }
    if (!is_archive_url(spec)) {
        error = "archive dependency must be github.com/owner/repo[@ref] or a .tar.gz/.tgz URL: " +
                std::string(spec);
        return false;
    }
    if (!archive_pkg_name_from_url(spec, pkg_name, error)) return false;
    url.assign(spec);
    is_local = ascii_lower(spec.substr(0, 5)) == "file:";
    return true;
}

}  // namespace

bool valid_archive_name(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() < 'A' ||
        (value.front() > 'Z' && value.front() < 'a') || value.front() > 'z')
        return false;
    for (char c : value)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    const std::string low = ascii_lower(value);
    if (low == "std" || is_win_device(low)) return false;
    return true;
}

bool valid_archive_relpath(std::string_view rel, std::string_view archive) {
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
    const std::string_view ext = archive_file_ext(parts.back());
    if (ext.empty()) return false;
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string_view stem = parts[i];
        if (i + 1 == parts.size()) stem = stem.substr(0, stem.size() - ext.size());
        if (stem.empty() || stem.size() > 64 ||
            !((stem.front() >= 'A' && stem.front() <= 'Z') ||
              (stem.front() >= 'a' && stem.front() <= 'z')))
            return false;
        for (char c : stem)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                return false;
        if (is_win_device(ascii_lower(stem))) return false;
    }
    return true;
}

bool expand_github_spec(std::string_view spec,
                        std::string &pkg_name,
                        std::string &url,
                        std::string &ref,
                        std::string &error) {
    if (spec.rfind("github.com/", 0) != 0) {
        error = "not a github.com spec";
        return false;
    }
    std::string_view rest = spec.substr(std::string_view("github.com/").size());
    ref = "main";
    const size_t at = rest.rfind('@');
    if (at != std::string_view::npos) {
        ref.assign(rest.substr(at + 1));
        rest = rest.substr(0, at);
    }
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos || rest.find('/', slash + 1) != std::string_view::npos) {
        error = "GitHub spec wants github.com/owner/repo[@ref]";
        return false;
    }
    const std::string_view owner = rest.substr(0, slash);
    std::string_view repo = rest.substr(slash + 1);
    if (repo.size() > 4 && repo.substr(repo.size() - 4) == ".git")
        repo = repo.substr(0, repo.size() - 4);
    if (!valid_gh_ident(owner, false) || !valid_gh_ident(repo, false) || !valid_gh_ident(ref, true)) {
        error = "invalid GitHub package spec " + std::string(spec);
        return false;
    }
    pkg_name = sanitize_module_stem(repo);
    if (!valid_archive_name(pkg_name)) {
        error = "repository name " + std::string(repo) + " cannot form a valid BRAT module name";
        return false;
    }
    url = "https://codeload.github.com/" + std::string(owner) + "/" +
          std::string(repo) + "/tar.gz/" + ref;
    return true;
}

std::string url_unquoted_basename(std::string_view url) {
    const std::string p = url_path_only(url);
    const size_t sl = p.rfind('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

bool is_archive_url(std::string_view url) {
    const std::string low = ascii_lower(url_unquoted_basename(url));
    return (low.size() > 7 && low.substr(low.size() - 7) == ".tar.gz") ||
           (low.size() > 4 && low.substr(low.size() - 4) == ".tgz");
}

bool archive_pkg_name_from_url(std::string_view url,
                               std::string &pkg_name,
                               std::string &error) {
    const std::string tail = url_unquoted_basename(url);
    const std::string low = ascii_lower(tail);
    std::string stem;
    if (low.size() > 7 && low.substr(low.size() - 7) == ".tar.gz")
        stem = tail.substr(0, tail.size() - 7);
    else if (low.size() > 4 && low.substr(low.size() - 4) == ".tgz")
        stem = tail.substr(0, tail.size() - 4);
    else {
        error = "cannot derive safe package name from archive URL " + std::string(url);
        return false;
    }
    stem = strip_version_tail(stem);
    pkg_name = sanitize_module_stem(stem);
    if (!valid_archive_name(pkg_name)) {
        error = "cannot derive safe package name from archive URL " + std::string(url);
        return false;
    }
    return true;
}

bool extract_tar_package(std::string_view raw_bytes,
                         const std::string &pkg_name,
                         const std::string &target,
                         std::map<std::string, std::string> &extracted,
                         std::map<std::string, std::string> &file_hashes,
                         ArchiveManifest &manifest,
                         std::string &error) {
    extracted.clear();
    file_hashes.clear();
    manifest = {};
    if (!valid_archive_name(pkg_name)) {
        error = "invalid archive package name " + pkg_name;
        return false;
    }
    std::vector<pkg_io::TarMember> members;
    if (!pkg_io::extract_targz_raw(raw_bytes, members, error, pkg_io::kMaxPackageBytes, 512))
        return false;

    std::vector<std::pair<std::vector<std::string>, std::string>> kept;
    for (auto &m : members) {
        if (m.segs.empty()) continue;
        const std::string &leaf = m.segs.back();
        if (archive_file_ext(leaf).empty() && leaf != "brat.pkg.json") continue;
        kept.emplace_back(std::move(m.segs), std::move(m.data));
    }
    if (kept.empty()) {
        error = "archive contains no .brat files";
        return false;
    }

    bool all_multi = true;
    std::set<std::string> top_heads;
    for (const auto &[segs, _] : kept) {
        if (segs.size() < 2) { all_multi = false; break; }
        top_heads.insert(segs[0]);
    }
    if (all_multi && top_heads.size() == 1) {
        for (auto &[segs, _] : kept) segs.erase(segs.begin());
    }

    std::string manifest_raw;
    bool has_manifest_raw = false;
    std::vector<std::pair<std::vector<std::string>, std::string>> raw_entries;
    for (auto &[segs, data] : kept) {
        if ((segs.size() == 1 && segs[0] == "brat.pkg.json") ||
            (segs.size() == 2 && segs[0] == pkg_name && segs[1] == "brat.pkg.json")) {
            if (has_manifest_raw) {
                error = "duplicate file in archive: " + pkg_name + "/brat.pkg.json";
                return false;
            }
            if (!parse_archive_manifest(data, pkg_name, manifest, error)) return false;
            manifest_raw = std::move(data);
            has_manifest_raw = true;
        } else if (!archive_file_ext(segs.back()).empty()) {
            raw_entries.emplace_back(std::move(segs), std::move(data));
        }
    }
    if (raw_entries.empty()) {
        error = "archive contains no .brat files";
        return false;
    }

    bool has_brat = false;
    for (auto &[segs, data] : raw_entries) {
        std::vector<std::string> rel_segs;
        if (segs.size() >= 2 && segs[0] == pkg_name) rel_segs = std::move(segs);
        else {
            rel_segs.push_back(pkg_name);
            rel_segs.insert(rel_segs.end(), segs.begin(), segs.end());
        }
        std::string rel;
        for (size_t i = 0; i < rel_segs.size(); ++i) {
            if (i) rel.push_back('/');
            rel += rel_segs[i];
        }
        if (!valid_archive_relpath(rel, pkg_name)) {
            error = "unsafe module path in archive: " + rel;
            return false;
        }
        if (extracted.contains(rel)) {
            error = "duplicate file in archive: " + rel;
            return false;
        }
        if (is_text_archive_ext(rel)) {
            if (!valid_utf8(data)) {
                error = rel + " is not valid UTF-8";
                return false;
            }
            std::string norm = norm_crlf(data);
            if (rel.size() > 5 && rel.substr(rel.size() - 5) == ".brat") {
                has_brat = true;
                try {
                    (void)parse(norm);
                } catch (const std::exception &e) {
                    error = rel + ": " + e.what();
                    return false;
                }
            }
            file_hashes[rel] = pkg_io::sha256_hex(norm);
            extracted[rel] = std::move(norm);
        } else {
            file_hashes[rel] = pkg_io::sha256_hex(data);
            extracted[rel] = std::move(data);
        }
    }

    if (!has_brat) {
        error = "archive contains no .brat files";
        return false;
    }
    if (!manifest.c_sources.empty()) {
        for (const auto &cs : manifest.c_sources) {
            const std::string rel_cs = pkg_name + "/" + cs;
            if (!extracted.contains(rel_cs)) {
                error = pkg_name + "/brat.pkg.json names missing c_source " + cs;
                return false;
            }
        }
        if (has_manifest_raw) {
            const std::string mrel = pkg_name + "/brat.pkg.json";
            std::string norm_m = norm_crlf(manifest_raw);
            file_hashes[mrel] = pkg_io::sha256_hex(norm_m);
            extracted[mrel] = std::move(norm_m);
        }
    }
    return verify_extracted_bratbinds(extracted, target, pkg_name, manifest.c_sources, error);
}

bool resolve_archive_graph(std::string_view root_spec,
                           bool allow_file_root,
                           const std::string &target,
                           const lsp_wire::LspJsonTape &lock_tape,
                           bool lock_exists,
                           std::vector<ArchivePlanItem> &out_plan,
                           std::string &error) {
    out_plan.clear();
    std::map<std::string, std::pair<std::string, std::string>> locked_by_name;
    if (lock_exists) {
        for (uint32_t item = lock_tape.at(lock_tape.root()).first_child; item != 0;
             item = lock_tape.at(item).next_sibling) {
            const uint32_t an = lock_tape.get(item, "archive");
            if (!lock_tape.is_string(an) || lock_tape.str_view(an).empty()) continue;
            locked_by_name[std::string(lock_tape.str_view(an))] = {
                std::string(lock_tape.str_view(lock_tape.get(item, "url"))),
                std::string(lock_tape.str_view(lock_tape.get(item, "sha256"))),
            };
        }
    }

    std::map<std::string, ArchivePlanItem> resolved;
    std::vector<std::string> order;
    std::vector<std::string> stack;
    size_t graph_archive_bytes = 0;
    size_t graph_extracted_bytes = 0;
    size_t graph_files = 0;

    auto visit = [&](auto &&self, std::string_view spec, bool allow_file,
                     bool block_internal_root,
                     std::string_view expected_name, std::string_view pinned_sha,
                     std::string &resolved_name) -> bool {
        std::string pkg_name, url, ref;
        bool is_local = false;
        if (!archive_spec_info(spec, pkg_name, url, ref, is_local, error)) return false;
        if (!expected_name.empty() && expected_name != pkg_name) {
            error = "dependency " + std::string(expected_name) +
                    " resolved to mismatched archive name " + pkg_name;
            return false;
        }
        if (is_local && !allow_file) {
            error = "a remote archive may not name a file:// dependency (" + std::string(spec) + ")";
            return false;
        }
        auto cyc = std::find(stack.begin(), stack.end(), pkg_name);
        if (cyc != stack.end()) {
            std::string chain;
            for (auto it = cyc; it != stack.end(); ++it) chain += *it + " -> ";
            chain += pkg_name;
            error = "archive dependency cycle: " + chain;
            return false;
        }
        auto prev = resolved.find(pkg_name);
        if (prev != resolved.end()) {
            if (prev->second.url != url) {
                error = "conflicting URLs for archive " + pkg_name + ": " +
                        prev->second.url + " vs " + url;
                return false;
            }
            if (!pinned_sha.empty() && prev->second.sha256 != pinned_sha) {
                error = "HASH MISMATCH " + pkg_name;
                return false;
            }
            resolved_name = pkg_name;
            return true;
        }
        if (!stack.empty()) {
            auto lprev = locked_by_name.find(pkg_name);
            if (lprev != locked_by_name.end()) {
                if (lprev->second.first != url) {
                    error = "conflicting URLs for locked archive " + pkg_name + ": " +
                            lprev->second.first + " vs " + url;
                    return false;
                }
                if (!pinned_sha.empty() && lprev->second.second != pinned_sha) {
                    error = "HASH MISMATCH " + pkg_name;
                    return false;
                }
            }
        }
        if (resolved.size() >= 64) {
            error = "archive dependency graph exceeds 64 packages";
            return false;
        }

        std::string raw;
        bool conn_internal = false;
        if (!pkg_io::fetch_url(url, raw, error, allow_file && is_local,
                               pkg_io::kMaxPackageBytes, block_internal_root, &conn_internal)) {
            error = "fetch " + url + ": " + error;
            return false;
        }
        if (raw.size() > pkg_io::kMaxGraphArchiveBytes - graph_archive_bytes) {
            error = "archive dependency graph exceeds 64 MiB total download budget";
            return false;
        }
        graph_archive_bytes += raw.size();
        const std::string digest = pkg_io::sha256_hex(raw);
        if (!pinned_sha.empty() && digest != pinned_sha) {
            error = "HASH MISMATCH " + pkg_name;
            return false;
        }

        std::map<std::string, std::string> extracted, file_hashes;
        ArchiveManifest manifest;
        if (!extract_tar_package(raw, pkg_name, target, extracted, file_hashes, manifest, error))
            return false;
        if (extracted.size() > pkg_io::kMaxGraphFiles - graph_files) {
            error = "archive dependency graph exceeds 4096 total extracted files";
            return false;
        }
        graph_files += extracted.size();
        for (const auto &[_, fbytes] : extracted) {
            if (fbytes.size() > pkg_io::kMaxGraphExtractedBytes - graph_extracted_bytes) {
                error = "archive dependency graph exceeds 128 MiB total extracted size";
                return false;
            }
            graph_extracted_bytes += fbytes.size();
        }

        stack.push_back(pkg_name);
        std::vector<std::string> dep_names;
        const bool child_allow_file = allow_file && is_local;
        const bool child_block_internal = block_internal_root || (!is_local && !conn_internal);
        for (const auto &dep : manifest.dependencies) {
            std::string dname;
            if (!self(self, dep.spec, child_allow_file, child_block_internal,
                      dep.expected_name, dep.sha256, dname))
                return false;
            if (std::find(dep_names.begin(), dep_names.end(), dname) == dep_names.end())
                dep_names.push_back(std::move(dname));
        }
        stack.pop_back();

        ArchivePlanItem item;
        item.archive = pkg_name;
        item.url = std::move(url);
        item.sha256 = digest;
        item.extracted = std::move(extracted);
        item.files = std::move(file_hashes);
        item.version = !ref.empty() ? ref : manifest.version;
        item.dependencies = std::move(dep_names);
        for (const auto &cs : manifest.c_sources) item.c_sources.push_back(pkg_name + "/" + cs);
        resolved.emplace(pkg_name, std::move(item));
        order.push_back(pkg_name);
        resolved_name = pkg_name;
        return true;
    };

    std::string root_name;
    if (!visit(visit, root_spec, allow_file_root, false, {}, {}, root_name)) return false;
    for (const auto &n : order) out_plan.push_back(std::move(resolved[n]));
    return true;
}

}  // namespace pkg_archive
