#ifndef BRAT_DRIVER_COMMON_H
#define BRAT_DRIVER_COMMON_H

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

#include "ast.h"
#include "module.h"
#include "parser.h"

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


extern FILE *g_report;

bool read_file(const std::string &path, std::string &out);
bool read_crt_bundle(const std::filesystem::path &engine_dir, std::string &out);
std::string dir_of(const std::string &path);
std::vector<std::string> split_lines(const std::string &src);
std::string rstrip(const std::string &s);

void emit_diag_line(const std::string &path, const std::vector<std::string> &lines,
                    const char *kind, int line, const std::string &msg);
std::set<std::string> imported_module_heads(const SourceFile &file);
std::set<std::string> exported_fn_names(const SourceFile &sf);
int check_programs(const std::vector<SourceFile> &progs, bool quiet = false);

std::filesystem::path executable_path(const char *arg0);
std::filesystem::path self_executable_path(const char *arg0);
std::filesystem::path locate_lang_dir(const char *arg0);
bool executable_on_path(const std::string &name);

std::string sha256_hex(const std::string &data);
bool sha256_file(const std::filesystem::path &path, std::string &digest,
                 uint64_t &size, std::string &error);

std::filesystem::path cache_root();
int main_cc_timeout_ms();
int run_process(const std::vector<std::string> &args, int timeout_ms = 0,
                bool *timed_out = nullptr);

bool has_extern_decls(const std::vector<StmtP> &body);
bool is_safe_pkg_ident(std::string_view s);
bool is_safe_flat_filename(std::string_view s);
bool is_safe_header_filename(std::string_view s);
bool confined_subpath(const std::filesystem::path &base_dir,
                      std::string_view rel_str,
                      std::filesystem::path &out);
bool verify_bratbind_bytes(const SourceFile &sf, const std::string &target,
                           std::string_view raw_bind);
bool module_has_valid_bratbind(const SourceFile &sf, const std::string &target);
bool has_unproven_externs(const std::vector<SourceFile> &files, const std::string &target);
std::vector<std::pair<std::string, std::string>> module_source_pairs(
    const std::vector<SourceFile> &files);
std::string host_default_triple();
std::string platform_kind(const std::string &target);
std::string build_ffi_contract_c(const std::vector<SourceFile> &files,
                                 const std::vector<StmtP> &flat_body,
                                 const std::string &target,
                                 const std::string &explicit_header = {},
                                 const std::vector<std::string> &explicit_objects = {});

bool parse_memory(const std::string &input, uint64_t &value);
void print_parse_error(const std::string &path, const ParseError &e);
std::size_t ownership_errors(const std::vector<SourceFile> &files, std::size_t i);
std::size_t type_errors(const std::vector<SourceFile> &files, std::size_t i,
                        bool arena_only = false);
void print_cli_usage();
int brat_cli_main(int argc, char **argv);

#endif
