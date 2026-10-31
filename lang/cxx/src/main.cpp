// `brat-cxx` — the front end and the native editor server.
// the toolchain it needs is the C compiler `brat build` already needs.
//
//   brat-cxx ast FILE      the s-expression of the tree, the diff contract
//   brat-cxx lex FILE      one token per line, for debugging the scanner
//   brat-cxx check FILE    ownership and types on a module set; later files
//                          feed signatures to the earlier ones, like `brat check`
//   brat-cxx flat FILE     the mangled program the emitter sees: imports
//                          spliced in place, names qualified, `pub use` gone
//   brat-cxx cc FILE       the C for FILE, byte for byte what codegen_c.py writes
//   brat-cxx cc -O1 FILE   the same C with the checks ir.cpp proves dead removed;
// why-opt prints the proof of each removed check
//   brat-cxx libs FILE     the libraries its extern blocks ask to link, one per line
//   brat-cxx build FILE    check, emit and link without the Python CLI
//   brat-cxx lsp           the native language server over stdio
//
// `brat-cxx` is also the `brat` the user runs. A command it does not implement
// (`fmt`, `pkg`, `api`, `mcp`, ...) is handed to the Python CLI, so `brat fmt`
// still works and `brat run` never starts an interpreter: 4 ms against 60.

#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ast.h"
#include "borrow.h"
#include "check.h"
#include "emit.h"
#include "ir.h"
#include "lexer.h"
#include "lsp.h"
#include "module.h"
#include "parser.h"

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

static std::string clip(const std::string &text) {
    if (text.size() <= kSnippetMax) return text;
    return text.substr(0, kSnippetMax - 3) + "...";
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
    return spawn_and_wait(args);
#else
    execvp(pass[0], pass.data());
    std::fprintf(stderr, "brat: cannot run %s: %s\n", python.c_str(), std::strerror(errno));
    return 2;
#endif
}

static int check_programs(const std::vector<SourceFile> &progs, bool quiet = false) {
    int bad = 0;
    for (size_t i = 0; i < progs.size(); i++) {
        std::map<std::string, std::vector<ParamMode>> extra;
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            for (auto &kv : pub_sigs(progs[j].body).sigs) extra[kv.first] = kv.second;
        }
        std::set<std::string> module_heads = imported_module_heads(progs[i]);
        BorrowOut out = borrow_check(progs[i].body, extra, progs[i].stray, module_heads);
        // Quiet on success: `run` is not `check`, and a program that runs must
        // not print a report first.
        if (!quiet || !out.errs.empty() || !out.warns.empty())
            std::printf("%s: %d error(s), %d warning(s)\n", progs[i].path.c_str(),
                        static_cast<int>(out.errs.size()), static_cast<int>(out.warns.size()));
        report(progs[i].path, progs[i].src, out.errs, "error");
        report(progs[i].path, progs[i].src, out.warns, "warning");
        if (!out.errs.empty()) bad++;
    }

    Sigs merged;
    for (size_t i = 1; i < progs.size(); i++) collect_sigs(progs[i].body, merged);
    for (const SourceFile &f : progs) {
        std::vector<TypeErr> errs = type_check(f.body, merged);
        if (!quiet || !errs.empty())
            std::printf("%s: %d type error(s)\n", f.path.c_str(),
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

static std::filesystem::path cache_root() {
    if (const char *env = std::getenv("BRAT_CACHE"); env && *env) return env;
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "brat";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "brat";
    return std::filesystem::temp_directory_path() / "brat-cache";
}

static int run_process(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const std::string &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
#if defined(_WIN32)
    const intptr_t result = _spawnvp(_P_WAIT, argv[0], argv.data());
    return result < 0 ? 127 : static_cast<int>(result);
#else
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        std::perror("brat-cxx: exec compiler");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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

static int build_one(const char *arg0, const std::string &file,
                     const std::string &output, const std::string &target,
                     bool static_link, bool static_explicit, bool shared, int opt,
                     uint64_t memory_limit,
                     const std::vector<std::string> &link_flags = {}) {
    ModuleSet set = load_modules(file);
    if (check_programs(set.files)) {
        std::fprintf(stderr, "brat-cxx build: refusing to build after checker errors\n");
        return 1;
    }
    flatten(set);
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
        const std::string c = codegen::emit_program(set.flat, dir_of(file), opt);
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

    std::vector<std::string> args{compiler};
    if (zig) args.push_back("cc");
    args.push_back(cfile.string());
    args.push_back("-o");
    args.push_back(temporary_output.string());
    if (target.size()) {
        args.push_back("-target");
        args.push_back(target);
    }
    if (static_link && !shared) args.push_back("-static");
    if (shared) {
        args.push_back("-shared");
        args.push_back("-fPIC");
    }
    // `-fwrapv` is what makes the native build agree with the interpreter on
    // integer overflow. Signed overflow is undefined in C: without the flag the
    // answer is the optimizer's, and an i64 sum that wraps is only "correct"
    // until the next compiler version notices the loop bound cannot overflow.
    args.insert(args.end(), {"-O2", "-fwrapv", "-I", engine.string(), "-lm"});
    if (memory_limit)
        args.push_back("-DBRAT_MEM_BYTES=" + std::to_string(memory_limit));
    for (const std::string &lib : libs) {
        if (std::filesystem::path(lib).is_absolute() || std::filesystem::exists(lib))
            args.push_back(lib);
        else
            args.push_back("-l:" + lib);
    }
    // `--link -lcudart`: the flags a library needs that a name cannot express
    // (`-L`, `-Wl,-rpath,...`, `-lcudart` beside a `.so` with no `lib` prefix).
    for (const std::string &flag : link_flags) args.push_back(flag);

     const int result = run_process(args);
     if (result != 0) {
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
                   uint64_t memory_limit, char **pass_argv) {
    ModuleSet set = load_modules(file);
    // Quiet on success: `run` is not `check`, and a program that runs must not
    // print a report first.
    if (check_programs(set.files, true)) {
        std::fflush(stdout);
        std::fprintf(stderr, "brat-cxx run: refusing to run after checker errors\n");
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
    std::string source = codegen::emit_program(set.flat, dir_of(file), opt);
    // The key covers the program, the runtime it is compiled against, and the
    // flags that change the binary. Cwd and file names are not in it on
    // purpose: `vbase_set` is baked into the emitted C.
    std::string key_material = sha256_hex(source) + sha256_hex(header) +
                               "|opt=" + std::to_string(opt) +
                               "|mem=" + std::to_string(memory_limit) + "|libs=";
    for (const auto &lib : libs) key_material += lib + ",";
    const std::string key = sha256_hex(key_material).substr(0, 32);

    std::error_code ec;
    const auto dir = cache_root() / "run";
    std::filesystem::create_directories(dir, ec);
    const auto binary = dir / key;
    const auto cfile = dir / (key + ".c");

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

    std::string compiler;
    if (const char *override = std::getenv("BRAT_CC"); override && *override) compiler = override;
    else if (executable_on_path("cc")) compiler = "cc";
    else if (executable_on_path("gcc")) compiler = "gcc";
    else {
        std::fprintf(stderr, "brat-cxx run: no C compiler found (set BRAT_CC)\n");
        return 1;
    }
    {
        std::ofstream out(cfile, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "brat-cxx run: cannot write %s\n", cfile.string().c_str());
            return 1;
        }
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
    }
    const auto temporary = dir / (key + ".tmp");
    std::vector<std::string> args{compiler, cfile.string(), "-o", temporary.string(),
                                  "-O2", "-fwrapv", "-I", engine.string(), "-lm"};
    if (memory_limit) args.push_back("-DBRAT_MEM_BYTES=" + std::to_string(memory_limit));
    for (const std::string &lib : libs) {
        if (std::filesystem::path(lib).is_absolute() || std::filesystem::exists(lib))
            args.push_back(lib);
        else
            args.push_back("-l:" + lib);
    }
    const int built = run_process(args);
    std::filesystem::remove(cfile, ec);
    if (built != 0) {
        std::filesystem::remove(temporary, ec);
        std::fprintf(stderr, "brat-cxx run: C compiler exited with status %d\n", built);
        return 1;
    }
#if defined(_WIN32)
    return run_process({temporary.string()});
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
    for (const Tok &t : lex(src)) {
        std::printf("%d:%zu@%u %.*s\n", static_cast<int>(t.kind), t.text.size(), t.line,
                    static_cast<int>(t.text.size()), t.text.data());
    }
}

// `cc` takes flags; the other commands take only files, so a flag there is a
// file that does not exist and is reported as one.
struct CcFlags {
    int opt = 0;
    bool why = false;
    bool no_check = false;
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
        } else if (a == "--why-opt") {
            f.why = true;
            f.opt = 1;
        } else if (a == "--no-check") {
            f.no_check = true;
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
    for (std::size_t j = 0; j < files.size(); j++) {
        if (j == i) continue;
        for (auto &kv : pub_sigs(files[j].body).sigs) extra[kv.first] = kv.second;
    }
    std::set<std::string> module_heads = imported_module_heads(files[i]);
    BorrowOut out = borrow_check(files[i].body, extra, files[i].stray, module_heads);
    report(files[i].path, files[i].src, out.errs, "error");
    return out.errs.size();
}

int main(int argc, char **argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::fprintf(stderr, "usage: brat-cxx ast|lex|check|flat|cc|libs|build FILE...\n"
                             "       brat-cxx run FILE [-- args...] [-O0|-O1] [--mem N]\n"
                             "       brat-cxx build FILE -o OUT [--link FLAG] [--mem N] [--header H]\n"
                             "       brat-cxx bundle OUTPUT_DIR\n");
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "bundle") {
        if (argc != 3) {
            std::fprintf(stderr, "usage: brat-cxx bundle OUTPUT_DIR\n");
            return 2;
        }
        return bundle_driver(argv[0], argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "lsp") {
        if (argc != 2) {
            std::fprintf(stderr, "usage: brat-cxx lsp\n");
            return 2;
        }
        return run_lsp();
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: brat-cxx ast|lex|check|flat|cc|libs|build FILE...\n"
                             "       brat-cxx run FILE [-- args...] [-O0|-O1] [--mem N]\n"
                             "       brat-cxx bundle OUTPUT_DIR\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const bool native = cmd == "ast" || cmd == "lex" || cmd == "check" || cmd == "flat" ||
                        cmd == "cc" || cmd == "libs" || cmd == "build" || cmd == "run";
    if (!native) return python_cli(argc, argv);
    if (cmd == "run") {
        // `--interp` asks for the interpreter itself, which is the one thing
        // this binary does not have: the request is handed over whole.
        for (int i = 2; i < argc; ++i)
            if (std::strcmp(argv[i], "--interp") == 0) return python_cli(argc, argv);
        std::string file;
        uint64_t memory_limit = 0;
        int opt = 0;
        std::vector<std::string> program_args;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--") {
                for (int j = i + 1; j < argc; ++j) program_args.push_back(argv[j]);
                break;
            }
            if (arg == "-O0") opt = 0;
            else if (arg == "-O1") opt = 1;
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
            return run_one(argv[0], file, opt, memory_limit, pass.data());
        } catch (const ParseError &e) {
            print_parse_error(file, e);
            return 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "brat-cxx run: %s\n", e.what());
            return 1;
        }
    }
    if (cmd == "build") {
        // `--header` asks for the bindings to be proven against a C header,
        // which is the one thing this binary cannot do (it has no C parser for
        // headers and should not grow one): the whole build goes to the CLI,
        // which proves first and only then calls back here.
        for (int i = 2; i < argc; ++i)
            if (std::strcmp(argv[i], "--header") == 0 || std::strncmp(argv[i], "--header=", 9) == 0)
                return python_cli(argc, argv);
        std::string file, output, target;
#if defined(__linux__)
        bool static_link = true, static_explicit = false, shared = false;
#else
        bool static_link = false, static_explicit = false, shared = false;
#endif
        uint64_t memory_limit = 0;
        int opt = 0;
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
            path.replace_extension(shared ? ".so" : ".exe");
            output = path.string();
        }
        try {
            return build_one(argv[0], file, output, target, static_link,
                             static_explicit, shared, opt, memory_limit, link_flags);
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
                    if (!flags.no_check) {
                        ModuleSet fresh = load_modules(file);
                        g_report = stderr;
                        nerr = ownership_errors(fresh.files, 0);
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
                        codegen::emit_program(set.flat, dir_of(file), flags.opt, &proofs);
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
        // `--json` is the machine-readable bill, and the Python CLI owns that
        // format: a second implementation is a second set of codes to keep in
        // step. A flag the Python CLI understands is handed over whole; anything
        // else is a usage error rather than a file named `--json`.
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--json" || arg == "--effects" || arg == "--proto" ||
                arg == "--caps" || arg == "--ownership" || arg == "--py")
                return python_cli(argc, argv);
            if (!arg.empty() && arg[0] == '-' && arg != "-") {
                std::fprintf(stderr, "brat-cxx check: unknown flag %s\n", arg.c_str());
                return 2;
            }
        }
    }

    std::vector<SourceFile> progs;
    for (int i = 2; i < argc; i++) {
        try {
            // The link rules (interfaces, visibility, re-exports) live in
            // `flatten`, which splices the modules; run it once and drop the tree,
            // or `check` accepts programs `cc` refuses.
            ModuleSet link = load_modules(argv[i]);
            flatten(link);
            for (auto &f : load_modules(argv[i]).files) progs.push_back(std::move(f));
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
        for (size_t j = 0; j < progs.size(); j++) {
            if (j == i) continue;
            for (auto &kv : pub_sigs(progs[j].body).sigs) extra[kv.first] = kv.second;
        }
        std::set<std::string> module_heads = imported_module_heads(progs[i]);
        BorrowOut out = borrow_check(progs[i].body, extra, progs[i].stray, module_heads);
        std::printf("%s: %d error(s), %d warning(s)\n", progs[i].path.c_str(),
                    static_cast<int>(out.errs.size()), static_cast<int>(out.warns.size()));
        report(progs[i].path, progs[i].src, out.errs, "error");
        report(progs[i].path, progs[i].src, out.warns, "warning");
        if (!out.errs.empty()) bad++;
    }

    Sigs merged;
    for (size_t i = 1; i < progs.size(); i++) collect_sigs(progs[i].body, merged);
    for (const SourceFile &f : progs) {
        std::vector<TypeErr> errs = type_check(f.body, merged);
        std::printf("%s: %d type error(s)\n", f.path.c_str(),
                    static_cast<int>(errs.size()));
        report(f.path, f.src, errs);
        if (!errs.empty()) bad++;
    }
    return bad ? 1 : 0;
}
