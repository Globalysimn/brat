#include "cli_tools.h"
#include "ffi_native.h"
#include "lsp_wire.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr size_t kMaxHeaderLine = 8 * 1024;
constexpr size_t kMaxHeaders = 64;
constexpr size_t kMaxBody = 16 * 1024 * 1024;
constexpr size_t kMaxToolOutput = 2 * 1024 * 1024;
constexpr int kDefaultTimeoutMs = 300000;

const char kToolsJson[] = R"json([{"name":"brat_check","description":"Type-check and ownership-check a BRAT file. Returns the diagnostic protocol JSON: every diagnostic has a stable code (E0382 use after move, T0001 type error, P0001 syntax error), a line, a message and a `helps` remedy.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"path to a .brat file"}},"required":["path"]}},{"name":"brat_run","description":"Run a BRAT file through the CLI. Returns stdout, stderr and the exit code.","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},{"name":"brat_build","description":"Compile a BRAT file to a native binary through C. Returns the compiler output and the path written.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"out":{"type":"string","description":"output path, optional"}},"required":["path"]}},{"name":"brat_api","description":"Exported surface of a module and everything it imports, as JSON: module name, file, and each export's name, params and return type.","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},{"name":"brat_deps","description":"Import graph of a program as JSON: root, module count, edges.","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},{"name":"brat_abi_matrix","description":"Verify an FFI module against a C header across LP64, LLP64 and ILP32 data models (`brat ffi --abi-matrix --json`), returning per-model contract proofs or drift mismatches.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"path to a .brat file"},"header":{"type":"string","description":"path to a C .h file"}},"required":["path","header"]}},{"name":"brat_pkg_verify","description":"Verify `brat.lock` and `vendor/` integrity, SHA-256 digests, `.bratbind` proofs and `c_sources` compilation inside a project directory.","inputSchema":{"type":"object","properties":{"dir":{"type":"string","description":"project root containing brat.lock"},"target":{"type":"string","description":"optional target triple"}},"required":["dir"]}},{"name":"brat_doc","description":"Extract structured API documentation (`brat doc --json`) from doc comments, `.bri` interfaces, `pub` items and FFI contracts for a file or directory.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"path to a .brat file or directory"}},"required":["path"]}},{"name":"brat_opt_proofs","description":"Run the proof-carrying abstract interpreter (`brat-cxx cc -O1|-O2 --why-opt`) and return all proven division/modulo and bounds-check elision certificates.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"path to a .brat file"},"level":{"type":"string","description":"optimization level: O1 or O2 (default O2)"}},"required":["path"]}}])json";

struct InputLine {
    std::string text;
    bool terminated = false;
    bool too_long = false;
};

enum class ReadKind { Message, Eof, Invalid };

struct ReadResult {
    ReadKind kind = ReadKind::Invalid;
    std::string body;
};

static InputLine read_line_bounded(std::istream &in, size_t limit, bool detect_jsonl = false) {
    InputLine line;
    bool decided = !detect_jsonl;
    size_t eff_limit = limit;
    char ch = 0;
    while (in.get(ch)) {
        if (ch == '\n') {
            line.terminated = true;
            return line;
        }
        if (!decided && !std::isspace(static_cast<unsigned char>(ch))) {
            decided = true;
            eff_limit = (ch == '{' || ch == '[') ? kMaxBody : limit;
        }
        if (line.text.size() < eff_limit) line.text.push_back(ch);
        else line.too_long = true;
    }
    return line;
}

static std::string_view trim_left(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    return text;
}

static ReadResult read_mcp_message(std::istream &in, bool &jsonl_mode) {
    std::map<std::string, std::string> headers;
    bool first = true;
    while (true) {
        const size_t line_limit = jsonl_mode ? kMaxBody : kMaxHeaderLine;
        InputLine line = read_line_bounded(in, line_limit, first && !jsonl_mode);
        if (!line.terminated && line.text.empty() && !line.too_long) {
            return {ReadKind::Eof, {}};
        }
        if (line.too_long) return {ReadKind::Invalid, {}};
        if (!line.terminated) return {ReadKind::Invalid, {}};
        if (!line.text.empty() && line.text.back() == '\r') line.text.pop_back();
        std::string_view stripped = trim_left(line.text);
        if (first && stripped.empty()) continue;

        if (jsonl_mode || (first && (stripped.front() == '{' || stripped.front() == '['))) {
            jsonl_mode = true;
            if (stripped.empty()) continue;
            if (stripped.size() > kMaxBody) return {ReadKind::Invalid, {}};
            return {ReadKind::Message, std::string(stripped)};
        }
        first = false;
        if (line.text.empty()) break;
        if (line.text.size() > kMaxHeaderLine) return {ReadKind::Invalid, {}};
        for (unsigned char c : line.text) if (c > 0x7f) return {ReadKind::Invalid, {}};
        const size_t colon = line.text.find(':');
        if (colon == std::string::npos) return {ReadKind::Invalid, {}};
        std::string key = line.text.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        size_t begin = colon + 1;
        while (begin < line.text.size() && std::isspace(static_cast<unsigned char>(line.text[begin])))
            ++begin;
        size_t end = line.text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(line.text[end - 1])))
            --end;
        headers[key] = line.text.substr(begin, end - begin);
        if (headers.size() > kMaxHeaders) return {ReadKind::Invalid, {}};
    }

    auto it = headers.find("content-length");
    if (it == headers.end() || it->second.empty()) return {ReadKind::Invalid, {}};
    size_t length = 0;
    const char *begin = it->second.data();
    const char *end = begin + it->second.size();
    auto parsed = std::from_chars(begin, end, length, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || length == 0 || length > kMaxBody)
        return {ReadKind::Invalid, {}};
    std::string body(length, '\0');
    in.read(body.data(), static_cast<std::streamsize>(length));
    if (static_cast<size_t>(in.gcount()) != length) return {ReadKind::Invalid, {}};
    return {ReadKind::Message, std::move(body)};
}

static bool json_complexity_ok(std::string_view input) {
    bool in_string = false;
    bool escape = false;
    size_t tokens = 0;
    for (unsigned char c : input) {
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') {
                in_string = false;
                if (++tokens > 1000000) return false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            if (++tokens > 1000000) return false;
        } else if (c == '{' || c == '[' || c == '}' || c == ']' || c == ',' || c == ':') {
            if (++tokens > 1000000) return false;
        }
    }
    return true;
}

static std::string json_text_result(std::string_view text, bool is_error) {
    lsp_wire::JsonWriter writer;
    writer.begin_obj();
    writer.key_lit("content");
    writer.begin_arr();
    writer.begin_obj();
    writer.key_lit("type"); writer.str("text");
    writer.key_lit("text"); writer.str(text);
    writer.end_obj();
    writer.end_arr();
    writer.key_lit("isError"); writer.boolean(is_error);
    writer.end_obj();
    return std::string(writer.view());
}

static std::string json_cli_result(int rc, std::string_view out, std::string_view err,
                                   std::string_view binary = {}, bool include_binary = false) {
    lsp_wire::JsonWriter writer;
    writer.begin_obj();
    writer.key_lit("exit"); writer.int64(rc);
    writer.key_lit("stdout"); writer.str(out);
    writer.key_lit("stderr"); writer.str(err);
    if (include_binary) {
        writer.key_lit("binary");
        if (rc == 0) writer.str(binary);
        else writer.null_val();
    }
    writer.end_obj();
    return std::string(writer.view());
}

static std::string make_reply(std::string_view raw_id, std::string_view result) {
    lsp_wire::JsonWriter writer;
    writer.begin_obj();
    writer.key_lit("jsonrpc"); writer.str("2.0");
    writer.key_lit("id"); writer.raw_value(raw_id);
    writer.key_lit("result"); writer.raw_value(result);
    writer.end_obj();
    return std::string(writer.view());
}

static std::string make_error(std::string_view raw_id, int code, std::string_view message) {
    lsp_wire::JsonWriter writer;
    writer.begin_obj();
    writer.key_lit("jsonrpc"); writer.str("2.0");
    writer.key_lit("id"); writer.raw_value(raw_id);
    writer.key_lit("error");
    writer.begin_obj();
    writer.key_lit("code"); writer.int64(code);
    writer.key_lit("message"); writer.str(message);
    writer.end_obj();
    writer.end_obj();
    return std::string(writer.view());
}

static bool get_string(const lsp_wire::LspJsonTape &tape, uint32_t object,
                       std::string_view key, std::string &out) {
    const uint32_t node = tape.get(object, key);
    if (!tape.is_string(node)) return false;
    out.assign(tape.str_view(node));
    return true;
}

static std::string absolute_path(std::string_view raw) {
    if (raw.find('\0') != std::string_view::npos) return {};
    std::error_code ec;
    auto full = std::filesystem::absolute(std::filesystem::path(std::string(raw)), ec);
    if (ec) return {};
    return full.lexically_normal().string();
}

struct CommandResult {
    int code = 1;
    std::string out;
    std::string err;
    bool timed_out = false;
    bool too_large = false;
    std::string launch_error;
};

#if !defined(_WIN32)
static bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void append_capped(std::string &dst, const char *data, size_t size, bool &too_large) {
    if (dst.size() > kMaxToolOutput || size > kMaxToolOutput - dst.size()) {
        too_large = true;
        return;
    }
    dst.append(data, size);
}

static CommandResult run_native(const std::string &exe, const std::vector<std::string> &args,
                                int timeout_ms, const std::string &cwd = {}) {
    CommandResult result;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        result.launch_error = std::string("cannot create command pipes: ") + std::strerror(errno);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.launch_error = std::string("cannot start native command: ") + std::strerror(errno);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(err_pipe[1], STDERR_FILENO) < 0)
            _exit(126);
        close(out_pipe[1]);
        close(err_pipe[1]);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        std::vector<char *> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char *>(exe.c_str()));
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    (void)setpgid(pid, pid);
    if (!set_nonblocking(out_pipe[0]) || !set_nonblocking(err_pipe[0])) {
        result.launch_error = "cannot make command pipes non-blocking";
        kill(-pid, SIGKILL);
        close(out_pipe[0]); close(err_pipe[0]);
        (void)waitpid(pid, nullptr, 0);
        return result;
    }

    bool out_open = true;
    bool err_open = true;
    bool child_done = false;
    bool wait_failed = false;
    int wait_status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto drain = [&](int fd, bool &open, std::string &dst) {
        std::array<char, 8192> buffer{};
        while (open) {
            const ssize_t got = read(fd, buffer.data(), buffer.size());
            if (got > 0) {
                append_capped(dst, buffer.data(), static_cast<size_t>(got), result.too_large);
                continue;
            }
            if (got == 0) {
                open = false;
                close(fd);
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            open = false;
            close(fd);
            return;
        }
    };

    while (out_open || err_open || !child_done) {
        if (!result.timed_out && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            (void)kill(-pid, SIGKILL);
        }
        if (result.too_large) (void)kill(-pid, SIGKILL);
        pollfd fds[2] = {{out_open ? out_pipe[0] : -1, POLLIN | POLLHUP, 0},
                         {err_open ? err_pipe[0] : -1, POLLIN | POLLHUP, 0}};
        int wait_ms = 50;
        if (!result.timed_out) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            wait_ms = static_cast<int>(std::clamp<long long>(left, 0, 50));
        }
        (void)poll(fds, 2, wait_ms);
        if (out_open) drain(out_pipe[0], out_open, result.out);
        if (err_open) drain(err_pipe[0], err_open, result.err);
        if (!child_done) {
            const pid_t waited = waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid) child_done = true;
            else if (waited < 0 && errno != EINTR) {
                child_done = true;
                wait_failed = true;
            }
        }
        if ((result.timed_out || result.too_large) && child_done && !out_open && !err_open) break;
    }
    if (!child_done) {
        while (true) {
            const pid_t waited = waitpid(pid, &wait_status, 0);
            if (waited == pid) break;
            if (waited < 0 && errno == EINTR) continue;
            wait_failed = true;
            break;
        }
    }
    if (wait_failed) {
        result.code = 127;
        result.launch_error = "cannot collect native command status";
    } else if (result.timed_out) result.code = 124;
    else if (result.too_large) result.code = 1;
    else if (WIFEXITED(wait_status)) result.code = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) result.code = 128 + WTERMSIG(wait_status);
    else result.code = 1;
    return result;
}
#else
static std::wstring mcp_win_quote(std::string_view arg) {
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

static CommandResult run_native(const std::string &exe, const std::vector<std::string> &args,
                                int timeout_ms, const std::string &cwd = {}) {
    CommandResult result;
    std::wstring cmdline = mcp_win_quote(exe);
    for (const auto &a : args) {
        cmdline.push_back(L' ');
        cmdline += mcp_win_quote(a);
    }
    std::wstring wcwd;
    if (!cwd.empty()) {
        const int cn = MultiByteToWideChar(CP_UTF8, 0, cwd.data(), static_cast<int>(cwd.size()), nullptr, 0);
        wcwd.assign(static_cast<size_t>(std::max(0, cn)), L'\0');
        if (cn > 0) MultiByteToWideChar(CP_UTF8, 0, cwd.data(), static_cast<int>(cwd.size()), wcwd.data(), cn);
    }
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_rd = nullptr, out_wr = nullptr;
    HANDLE err_rd = nullptr, err_wr = nullptr;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        result.launch_error = "cannot create stdout pipe";
        return result;
    }
    if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) {
        CloseHandle(out_rd);
        CloseHandle(out_wr);
        result.launch_error = "cannot create stderr pipe";
        return result;
    }
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
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
    si.hStdOutput = out_wr;
    si.hStdError = err_wr;
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, flags,
                        nullptr, cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        CloseHandle(out_rd); CloseHandle(out_wr);
        CloseHandle(err_rd); CloseHandle(err_wr);
        if (job) CloseHandle(job);
        result.launch_error = "cannot launch native command";
        return result;
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(out_wr);
    CloseHandle(err_wr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto drain_pipe = [&](HANDLE rd, std::string &dst) {
        std::array<char, 8192> buf {};
        DWORD got = 0;
        while (ReadFile(rd, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (dst.size() > kMaxToolOutput || got > kMaxToolOutput - dst.size()) {
                result.too_large = true;
                if (job) TerminateJobObject(job, 1);
                else TerminateProcess(pi.hProcess, 1);
                break;
            }
            dst.append(buf.data(), got);
        }
    };
    std::thread t_out([&]() { drain_pipe(out_rd, result.out); });
    std::thread t_err([&]() { drain_pipe(err_rd, result.err); });

    const DWORD tmo_ms = (timeout_ms > 0)
                             ? static_cast<DWORD>(std::min<long long>(timeout_ms, 3600000LL))
                             : INFINITE;
    const DWORD wrc = WaitForSingleObject(pi.hProcess, tmo_ms);
    if (wrc == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job) TerminateJobObject(job, 124);
        else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(out_rd);
    CloseHandle(err_rd);
    if (t_out.joinable()) t_out.join();
    if (t_err.joinable()) t_err.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (result.timed_out) result.code = 124;
    else if (result.too_large) result.code = 1;
    else result.code = static_cast<int>(code);
    return result;
}
#endif

static std::string resolve_executable(const char *arg0) {
#if defined(_WIN32)
    std::array<char, 32768> buffer{};
    const DWORD len = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size()) return std::string(buffer.data(), len);
#else
    std::array<char, 4096> buffer{};
    const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len > 0) return std::string(buffer.data(), static_cast<size_t>(len));
#endif
    if (!arg0 || !*arg0) return {};
    std::error_code ec;
    std::filesystem::path p(arg0);
    if (p.has_parent_path()) {
        auto full = std::filesystem::absolute(p, ec);
        if (!ec) return full.lexically_normal().string();
        return {};
    }
    const char *path_env = std::getenv("PATH");
    if (!path_env) return {};
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::string paths(path_env);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(separator, start);
        const std::string dir = paths.substr(start, end == std::string::npos ? end : end - start);
        auto candidate = std::filesystem::path(dir.empty() ? "." : dir) / arg0;
#if defined(_WIN32)
        if (std::filesystem::is_regular_file(candidate, ec)) return std::filesystem::absolute(candidate, ec).string();
#else
        if (access(candidate.c_str(), X_OK) == 0) return std::filesystem::absolute(candidate, ec).string();
#endif
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

static bool require_path(const lsp_wire::LspJsonTape &tape, uint32_t args,
                         std::string &path, std::string &error) {
    std::string raw;
    if (!get_string(tape, args, "path", raw) || raw.empty()) {
        error = "`path` is required and must be a string";
        return false;
    }
    path = absolute_path(raw);
    if (path.empty()) {
        error = "invalid path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "no such file: " + path;
        return false;
    }
    return true;
}

struct ToolResult {
    std::string text;
    bool failed = false;
};

static ToolResult invoke_tool(std::string_view name, const lsp_wire::LspJsonTape &tape,
                              uint32_t args, const std::string &exe) {
    ToolResult result;
    std::string path;
    std::string error;
    auto call = [&](std::vector<std::string> command, int timeout = kDefaultTimeoutMs,
                    const std::string &cwd = {}) {
        return run_native(exe, command, timeout, cwd);
    };
    auto cli_text = [](const CommandResult &r, bool fallback_json = false) {
        if (!r.launch_error.empty()) return r.launch_error;
        if (r.timed_out) return std::string("timed out");
        if (r.too_large) return std::string("native command output exceeds limit");
        if (!r.out.empty()) return r.out;
        if (!r.err.empty()) return r.err;
        return fallback_json ? std::string("{}") : std::string();
    };
    auto finish_cli = [&](const CommandResult &r, bool fallback_json = false) {
        result.text = cli_text(r, fallback_json);
        result.failed = r.code != 0 || !r.launch_error.empty() || r.timed_out || r.too_large;
    };

    if (name == "brat_check" || name == "brat_run" || name == "brat_build" ||
        name == "brat_api" || name == "brat_deps" ||
        name == "brat_opt_proofs" || name == "brat_abi_matrix") {
        if (!require_path(tape, args, path, error)) return {error, true};
    }

    if (name == "brat_check") {
        CommandResult r = call({"check", path, "--json"});
        finish_cli(r, true);
        return result;
    }
    if (name == "brat_run") {
        CommandResult r = call({"run", path});
        if (!r.launch_error.empty() || r.timed_out || r.too_large) {
            return {r.launch_error.empty() ? (r.timed_out ? "timed out" : "native command output exceeds limit")
                                           : r.launch_error, true};
        }
        result.text = json_cli_result(r.code, r.out, r.err);
        result.failed = r.code != 0;
        return result;
    }
    if (name == "brat_build") {
        uint32_t out_node = tape.get(args, "out");
        std::string out_path;
        if (out_node != 0 && !tape.is_null(out_node)) {
            if (!tape.is_string(out_node)) return {"`out` must be a non-empty string", true};
            const std::string raw(tape.str_view(out_node));
            if (raw.empty()) return {"`out` must be a non-empty string", true};
            if (raw.front() == '-') return {"`out` must not start with '-'", true};
            out_path = absolute_path(raw);
            if (out_path.empty()) return {"invalid output path", true};
        } else {
            out_path = std::filesystem::path(path).replace_extension(".exe").string();
        }
        CommandResult r = call({"build", path, "-o", out_path}, 600000);
        if (!r.launch_error.empty() || r.timed_out || r.too_large) {
            return {r.launch_error.empty() ? (r.timed_out ? "timed out" : "native command output exceeds limit")
                                           : r.launch_error, true};
        }
        result.text = json_cli_result(r.code, r.out, r.err, out_path, true);
        result.failed = r.code != 0;
        return result;
    }
    if (name == "brat_api") {
        CommandResult r = call({"api", path, "--json"});
        finish_cli(r);
        return result;
    }
    if (name == "brat_deps") {
        CommandResult r = call({"deps", path, "--json"});
        finish_cli(r);
        return result;
    }
    if (name == "brat_doc") {
        std::string raw;
        if (!get_string(tape, args, "path", raw) || raw.empty())
            return {"`path` is required and must be a string", true};
        path = absolute_path(raw);
        if (path.empty()) return {"invalid path", true};
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return {"no such file or directory: " + path, true};
        CommandResult r = call({"doc", path, "--json"});
        finish_cli(r, true);
        return result;
    }
    if (name == "brat_opt_proofs") {
        std::string level = "O2";
        const uint32_t level_node = tape.get(args, "level");
        if (level_node != 0) {
            if (!tape.is_string(level_node)) return {"`level` must be 'O1' or 'O2'", true};
            level.assign(tape.str_view(level_node));
        }
        if (level != "O1" && level != "O2" && level != "-O1" && level != "-O2")
            return {"`level` must be 'O1' or 'O2'", true};
        const std::string flag = level.front() == '-' ? level : "-" + level;
        CommandResult r = call({"cc", flag, "--why-opt", "--no-check", path}, 60000);
        if (!r.launch_error.empty() || r.timed_out || r.too_large)
            return {r.launch_error.empty() ? (r.timed_out ? "timed out" : "native command output exceeds limit")
                                           : r.launch_error, true};
        std::vector<std::string> groups;
        std::string current;
        std::istringstream lines(r.code == 0 ? r.err : std::string());
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("fn ", 0) == 0 && !current.empty()) {
                groups.push_back(std::move(current));
                current.clear();
            }
            if (!line.empty()) {
                if (!current.empty()) current.push_back('\n');
                current += line;
            }
        }
        if (!current.empty()) groups.push_back(std::move(current));
        lsp_wire::JsonWriter writer;
        writer.begin_obj();
        writer.key_lit("exit"); writer.int64(r.code);
        writer.key_lit("level"); writer.str(flag);
        writer.key_lit("count"); writer.uint64(groups.size());
        writer.key_lit("proofs"); writer.begin_arr();
        for (const auto &proof : groups) writer.str(proof);
        writer.end_arr();
        writer.key_lit("stderr");
        writer.str(r.code != 0 ? std::string_view(r.err) : std::string_view());
        writer.end_obj();
        result.text = std::string(writer.view());
        result.failed = r.code != 0;
        return result;
    }
    if (name == "brat_abi_matrix") {
        std::string header;
        if (!get_string(tape, args, "header", header) || header.empty())
            return {"`header` is required and must be a string", true};
        const std::string header_path = absolute_path(header);
        std::error_code ec;
        if (header_path.empty() || !std::filesystem::is_regular_file(header_path, ec))
            return {"no such header: " + (header_path.empty() ? header : header_path), true};
        native_ffi::AbiMatrixResult matrix = native_ffi::abi_matrix(path, header_path);
        if (!matrix.error.empty()) return {matrix.error, true};
        result.text = std::move(matrix.json);
        result.failed = matrix.status != 0;
        return result;
    }
    if (name == "brat_pkg_verify") {
        std::string raw_dir;
        if (!get_string(tape, args, "dir", raw_dir) || raw_dir.empty())
            return {"`dir` is required and must be a string", true};
        const std::string dir = absolute_path(raw_dir);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec))
            return {"no such directory: " + (dir.empty() ? raw_dir : dir), true};
        const uint32_t target_node = tape.get(args, "target");
        std::vector<std::string> command{"pkg", "verify"};
        if (target_node != 0 && !tape.is_null(target_node)) {
            if (!tape.is_string(target_node) || tape.str_view(target_node).empty() ||
                tape.str_view(target_node).front() == '-')
                return {"`target` must be a safe target triple string", true};
            command.push_back("--target");
            command.emplace_back(tape.str_view(target_node));
        }
        CommandResult r = call(std::move(command), kDefaultTimeoutMs, dir);
        if (!r.launch_error.empty() || r.timed_out || r.too_large)
            return {r.launch_error.empty() ? (r.timed_out ? "timed out" : "native command output exceeds limit")
                                           : r.launch_error, true};
        result.text = json_cli_result(r.code, r.out, r.err);
        result.failed = r.code != 0;
        return result;
    }
    return {"unknown tool", true};
}

static bool send_body(std::string_view body, bool jsonl_mode) {
    std::string fallback;
    if (body.size() > kMaxBody) {
        fallback = make_error("null", -32603, "response exceeds maximum body size");
        body = fallback;
    }
    if (jsonl_mode) {
        std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
        std::cout.put('\n');
    } else {
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
        std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    std::cout.flush();
    return static_cast<bool>(std::cout);
}

} // namespace

int cmd_mcp(int argc, char **argv) {
    if (argc == 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
        std::fprintf(stderr, "usage: brat-cxx mcp\n");
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: brat-cxx mcp\n");
        return 2;
    }
    const std::string exe = resolve_executable(argv[0]);
    if (exe.empty()) {
        std::fprintf(stderr, "brat-cxx mcp: cannot resolve executable path\n");
        return 1;
    }
    bool initialized = false;
    bool jsonl_mode = false;
    while (true) {
        ReadResult input = read_mcp_message(std::cin, jsonl_mode);
        if (input.kind == ReadKind::Eof) return 0;
        if (input.kind == ReadKind::Invalid) {
            if (!send_body(make_error("null", -32700, "parse error"), jsonl_mode)) return 1;
            continue;
        }
        lsp_wire::LspJsonTape tape;
        if (!json_complexity_ok(input.body) || !tape.parse(input.body)) {
            if (!send_body(make_error("null", -32700, "parse error"), jsonl_mode)) return 1;
            continue;
        }
        const uint32_t root = tape.root();
        if (!tape.is_object(root)) {
            if (!send_body(make_error("null", -32600, "invalid request"), jsonl_mode)) return 1;
            continue;
        }
        const uint32_t id_node = tape.get(root, "id");
        if (id_node != 0) {
            const auto ik = tape.at(id_node).kind;
            if (ik != lsp_wire::JKind::Null && ik != lsp_wire::JKind::Int &&
                ik != lsp_wire::JKind::Float && !tape.is_string(id_node)) {
                if (!send_body(make_error("null", -32600, "invalid request"), jsonl_mode)) return 1;
                continue;
            }
        }
        const std::string raw_id = id_node == 0 ? "null" : std::string(tape.raw_json_slice(id_node));
        const uint32_t version_node = tape.get(root, "jsonrpc");
        const uint32_t method_node = tape.get(root, "method");
        if (!tape.is_string(version_node) || tape.str_view(version_node) != "2.0" ||
            !tape.is_string(method_node)) {
            if (!send_body(make_error(raw_id, -32600, "invalid request"), jsonl_mode)) return 1;
            continue;
        }
        const std::string method(tape.str_view(method_node));
        uint32_t params = tape.get(root, "params");
        if (params != 0 && tape.is_null(params)) params = 0;
        if (params != 0 && !tape.is_object(params)) {
            if (!send_body(make_error(raw_id, -32602, "params must be an object"), jsonl_mode)) return 1;
            continue;
        }
        if (method == "initialize") {
            initialized = true;
            lsp_wire::JsonWriter result;
            result.begin_obj();
            result.key_lit("protocolVersion"); result.str("2025-06-18");
            result.key_lit("capabilities"); result.begin_obj();
            result.key_lit("tools"); result.begin_obj();
            result.key_lit("listChanged"); result.boolean(false);
            result.end_obj(); result.end_obj();
            result.key_lit("serverInfo"); result.begin_obj();
            result.key_lit("name"); result.str("brat-mcp");
            result.key_lit("version"); result.str("0.1");
            result.end_obj();
            result.end_obj();
            if (!send_body(make_reply(raw_id, result.view()), jsonl_mode)) return 1;
            continue;
        }
        if (method == "notifications/initialized" || method == "initialized") continue;
        if (method == "ping") {
            if (!send_body(make_reply(raw_id, "{}"), jsonl_mode)) return 1;
            continue;
        }
        if (method == "tools/list") {
            std::string result = std::string("{\"tools\":") + kToolsJson + "}";
            if (!send_body(make_reply(raw_id, result), jsonl_mode)) return 1;
            continue;
        }
        if (method == "tools/call") {
            if (!initialized) {
                if (!send_body(make_error(raw_id, -32002, "server not initialized"), jsonl_mode)) return 1;
                continue;
            }
            std::string name;
            if (!get_string(tape, params, "name", name)) {
                if (!send_body(make_error(raw_id, -32602, "tools/call needs a tool name"), jsonl_mode)) return 1;
                continue;
            }
            static const std::set<std::string> known = {
                "brat_check", "brat_run", "brat_build", "brat_api", "brat_deps",
                "brat_abi_matrix", "brat_pkg_verify", "brat_doc", "brat_opt_proofs"};
            if (known.find(name) == known.end()) {
                if (!send_body(make_error(raw_id, -32602, "unknown tool `" + name + "`"), jsonl_mode)) return 1;
                continue;
            }
            uint32_t args_node = tape.get(params, "arguments");
            if (args_node != 0 && tape.is_null(args_node)) args_node = 0;
            if (args_node != 0 && !tape.is_object(args_node)) {
                if (!send_body(make_error(raw_id, -32602, "arguments must be an object"), jsonl_mode)) return 1;
                continue;
            }
            ToolResult tool = invoke_tool(name, tape, args_node, exe);
            const std::string result = json_text_result(tool.text, tool.failed);
            if (!send_body(make_reply(raw_id, result), jsonl_mode)) return 1;
            continue;
        }
        if (id_node != 0 && !tape.is_null(id_node)) {
            if (!send_body(make_error(raw_id, -32601, "unknown method `" + method + "`"), jsonl_mode)) return 1;
        }
    }
}
