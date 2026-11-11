#include "cli_tools.h"
#include "ffi_native.h"
#include "lsp_wire.h"
#include "pkg_io.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <fcntl.h>
#include <io.h>
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
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

enum class WireMode : uint8_t { Auto, Framed, Jsonl };

static ReadResult read_mcp_message(std::istream &in, WireMode &mode) {
    std::map<std::string, std::string> headers;
    bool first = true;
    while (true) {
        const size_t line_limit = (mode == WireMode::Jsonl) ? kMaxBody : kMaxHeaderLine;
        InputLine line = read_line_bounded(in, line_limit, first && mode == WireMode::Auto);
        if (!line.terminated && line.text.empty() && !line.too_long) {
            return {ReadKind::Eof, {}};
        }
        if (line.too_long || !line.terminated) return {ReadKind::Invalid, {}};
        if (!line.text.empty() && line.text.back() == '\r') line.text.pop_back();
        std::string_view stripped = trim_left(line.text);
        if (first && stripped.empty()) continue;

        if (mode == WireMode::Jsonl ||
            (first && mode == WireMode::Auto && (stripped.front() == '{' || stripped.front() == '['))) {
            mode = WireMode::Jsonl;
            if (stripped.empty()) continue;
            if (stripped.size() > kMaxBody) return {ReadKind::Invalid, {}};
            return {ReadKind::Message, std::string(stripped)};
        }
        mode = WireMode::Framed;
        first = false;
        if (line.text.empty()) break;
        if (line.text.size() > kMaxHeaderLine) return {ReadKind::Invalid, {}};
        for (unsigned char c : line.text) if (c > 0x7f) return {ReadKind::Invalid, {}};
        const size_t colon = line.text.find(':');
        if (colon == 0 || colon == std::string::npos) return {ReadKind::Invalid, {}};
        std::string key = line.text.substr(0, colon);
        for (unsigned char c : key) {
            if (!std::isalnum(c) && c != '-' && c != '_') return {ReadKind::Invalid, {}};
        }
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        size_t begin = colon + 1;
        while (begin < line.text.size() && std::isspace(static_cast<unsigned char>(line.text[begin])))
            ++begin;
        size_t end = line.text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(line.text[end - 1])))
            --end;
        if (key == "content-length" && headers.count(key)) return {ReadKind::Invalid, {}};
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

static CommandResult run_native(const std::string &exe, const std::vector<std::string> &args,
                                int timeout_ms, const std::string &cwd = {}) {
    CommandResult result;
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(exe);
    argv.insert(argv.end(), args.begin(), args.end());

    pkg_io::ProcOptions opts;
    opts.timeout_ms = timeout_ms;
    opts.cwd = cwd;
    opts.search_path = false;
    opts.capture_output = true;
    opts.merge_stderr = false;
    opts.null_stdin = true;
    opts.max_out_total = kMaxToolOutput;
    opts.max_err_total = kMaxToolOutput;

    pkg_io::ProcResult pr = pkg_io::run_proc(argv, opts);
    result.out = std::move(pr.out);
    result.err = std::move(pr.err);
    result.timed_out = pr.timed_out;
    result.too_large = pr.over_limit;
    if (!pr.launched) {
        result.code = 127;
        result.launch_error = pr.launch_error.empty() ? "cannot launch native command" : pr.launch_error;
        return result;
    }
    if (result.timed_out) result.code = 124;
    else if (result.too_large) result.code = 1;
    else result.code = pr.exit_code;
    return result;
}

static std::string resolve_executable(const char *arg0) {
    return pkg_io::self_exe_path(arg0, false).string();
}

static bool path_is_within(const std::filesystem::path &child, const std::filesystem::path &root) {
    auto ci = child.begin(), ce = child.end();
    auto ri = root.begin(), re = root.end();
    for (; ri != re; ++ri, ++ci) {
        if (ci == ce || *ci != *ri) return false;
    }
    return true;
}

static bool mcp_unconfined_enabled() {
    const char *ev = std::getenv("BRAT_MCP_UNCONFINED");
    return ev && (std::strcmp(ev, "1") == 0 || std::strcmp(ev, "true") == 0 || std::strcmp(ev, "yes") == 0);
}

static bool is_protected_source_ext(std::string ext) {
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char *const kProtected[] = {
        ".brat", ".bri", ".c", ".h", ".cc", ".cpp", ".hpp", ".rs", ".py",
        ".sh", ".json", ".lock", ".toml", ".yaml", ".yml", ".md"
    };
    for (const char *p : kProtected) {
        if (ext == p) return true;
    }
    return false;
}

static bool require_path(const lsp_wire::LspJsonTape &tape, uint32_t args,
                         std::string &path, std::string &error) {
    std::string raw;
    if (!get_string(tape, args, "path", raw) || raw.empty()) {
        error = "`path` is required and must be a string";
        return false;
    }
    if (raw.front() == '-') {
        error = "`path` must not start with '-'";
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
    auto launch_err = [](const CommandResult &r) -> std::string {
        if (!r.launch_error.empty()) return r.launch_error;
        if (r.timed_out) return "timed out";
        if (r.too_large) return "native command output exceeds limit";
        return {};
    };
    auto cli_text = [&](const CommandResult &r, bool fallback_json = false) {
        std::string fail = launch_err(r);
        if (!fail.empty()) return fail;
        if (!r.out.empty()) return r.out;
        if (!r.err.empty()) return r.err;
        return fallback_json ? std::string("{}") : std::string();
    };
    auto finish_cli = [&](const CommandResult &r, bool fallback_json = false) {
        result.text = cli_text(r, fallback_json);
        result.failed = r.code != 0 || !r.launch_error.empty() || r.timed_out || r.too_large;
    };
    auto finish_cli_json = [&](const CommandResult &r, std::string_view bin = {},
                               bool include_bin = false) -> ToolResult {
        std::string fail = launch_err(r);
        if (!fail.empty()) return {std::move(fail), true};
        return {json_cli_result(r.code, r.out, r.err, bin, include_bin), r.code != 0};
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
        return finish_cli_json(call({"run", path}));
    }
    if (name == "brat_build") {
        uint32_t out_node = tape.get(args, "out");
        std::string out_path;
        const bool explicit_out = (out_node != 0 && !tape.is_null(out_node));
        if (explicit_out) {
            if (!tape.is_string(out_node)) return {"`out` must be a non-empty string", true};
            const std::string raw(tape.str_view(out_node));
            if (raw.empty()) return {"`out` must be a non-empty string", true};
            if (raw.front() == '-') return {"`out` must not start with '-'", true};
            out_path = absolute_path(raw);
            if (out_path.empty()) return {"invalid output path", true};
        } else {
            out_path = std::filesystem::path(path).replace_extension(".exe").string();
        }
        {
            std::error_code ec;
            const std::filesystem::path out_fs(out_path);
            if (std::filesystem::is_symlink(out_fs, ec))
                return {"refusing to overwrite symlink output path: " + out_path, true};
            if (std::filesystem::is_directory(out_fs, ec))
                return {"output path is a directory: " + out_path, true};
            const auto src_canon = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
            const auto out_canon = std::filesystem::weakly_canonical(out_fs, ec);
            if (!ec && out_canon == src_canon)
                return {"`out` must not overwrite input source file", true};
            if (explicit_out && is_protected_source_ext(out_fs.extension().string()))
                return {"refusing to overwrite source or config file extension: " + out_path, true};
            if (explicit_out && !mcp_unconfined_enabled()) {
                auto src_dir = std::filesystem::weakly_canonical(std::filesystem::path(path).parent_path(), ec);
                auto cwd_dir = std::filesystem::weakly_canonical(std::filesystem::current_path(ec), ec);
                // Walk up from src_dir to nearest ancestor with brat.lock if any.
                std::filesystem::path proj_root = src_dir;
                for (auto cur = src_dir; !cur.empty() && cur != cur.root_path(); cur = cur.parent_path()) {
                    if (std::filesystem::is_regular_file(cur / "brat.lock", ec)) {
                        proj_root = cur;
                        break;
                    }
                }
                const bool ok_proj = !proj_root.empty() && path_is_within(out_canon, proj_root);
                const bool ok_cwd = !cwd_dir.empty() && path_is_within(out_canon, cwd_dir);
                if (!ok_proj && !ok_cwd) {
                    return {"refusing `out` path outside source project or working directory: " + out_path, true};
                }
            }
        }
        return finish_cli_json(call({"build", path, "-o", out_path}, 600000), out_path, true);
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
        if (raw.front() == '-') return {"`path` must not start with '-'", true};
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
        if (std::string fail = launch_err(r); !fail.empty()) return {std::move(fail), true};
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
        if (header.front() == '-') return {"`header` must not start with '-'", true};
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
        if (raw_dir.front() == '-') return {"`dir` must not start with '-'", true};
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
        return finish_cli_json(call(std::move(command), kDefaultTimeoutMs, dir));
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
        std::fprintf(stdout, "usage: brat mcp\n");
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: brat mcp\n");
        return 2;
    }
    const std::string exe = resolve_executable(argv[0]);
    if (exe.empty()) {
        std::fprintf(stderr, "brat mcp: cannot resolve executable path\n");
        return 1;
    }
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    bool initialized = false;
    WireMode wire_mode = WireMode::Auto;
    while (true) {
        ReadResult input = read_mcp_message(std::cin, wire_mode);
        const bool jsonl_mode = (wire_mode == WireMode::Jsonl);
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
