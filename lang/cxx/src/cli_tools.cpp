// Native `deps` and `api` commands.
#include "cli_tools.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "module.h"
#include "parser.h"

namespace {

std::string json_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
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
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

std::string dot_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string module_name(const SourceFile &file) {
    if (!file.ns) return "main";
    std::string out;
    out.reserve(file.ns->size());
    for (size_t i = 0; i < file.ns->size(); ++i) {
        if (file.ns->compare(i, 2, "__") == 0) {
            out.push_back('.');
            ++i;
        } else {
            out.push_back((*file.ns)[i]);
        }
    }
    return out;
}

std::string type_name(const std::optional<std::string> &type, const char *fallback) {
    return type && !type->empty() ? *type : fallback;
}

struct Edge {
    std::string from;
    std::string imported;
    std::string to;
};

std::vector<Edge> collect_edges(const ModuleSet &set) {
    std::vector<Edge> edges;
    for (const SourceFile &file : set.files) {
        const std::string from = module_name(file);
        for (const auto &[imported, target] : file.imports) {
            if (target >= set.files.size()) continue;
            edges.push_back({from, imported, module_name(set.files[target])});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) {
        if (a.from != b.from) return a.from < b.from;
        return a.imported < b.imported;
    });
    return edges;
}

std::string deps_json(const std::string &root, size_t modules, const std::vector<Edge> &edges) {
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n"
        << "  \"root\": " << json_quote(root) << ",\n"
        << "  \"modules\": " << modules << ",\n"
        << "  \"edges\": [";
    if (edges.empty()) {
        out << "]\n}\n";
        return out.str();
    }
    out << '\n';
    for (size_t i = 0; i < edges.size(); ++i) {
        const Edge &edge = edges[i];
        out << "    {\n      \"from\": " << json_quote(edge.from) << ",\n"
            << "      \"import\": " << json_quote(edge.imported) << ",\n"
            << "      \"to\": " << json_quote(edge.to) << "\n    }";
        out << (i + 1 == edges.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string deps_dot(const ModuleSet &set, const std::vector<Edge> &edges) {
    std::set<std::string> nodes;
    for (const SourceFile &file : set.files) nodes.insert(module_name(file));
    std::ostringstream out;
    out << "digraph dependencies {\n";
    for (const std::string &node : nodes) out << "  " << dot_quote(node) << ";\n";
    for (const Edge &edge : edges)
        out << "  " << dot_quote(edge.from) << " -> " << dot_quote(edge.to)
            << " [label=" << dot_quote(edge.imported) << "];\n";
    out << "}\n";
    return out.str();
}

bool write_output(const std::string &path, const std::string &text, std::string &error) {
    namespace fs = std::filesystem;
    fs::path output(path);
    if (output.empty()) {
        error = "empty output path";
        return false;
    }
    fs::path parent = output.parent_path();
    if (parent.empty()) parent = ".";
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        error = "output directory does not exist: " + parent.string();
        return false;
    }

    static std::atomic<std::uint64_t> sequence{0};
#if defined(_WIN32)
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    fs::path temp;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temp = parent / (".brat-output-" + std::to_string(pid) + "-" +
                         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
#if defined(_WIN32)
        fd = _open(temp.string().c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                    | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                    | O_NOFOLLOW
#endif
                    , 0666);
#endif
        if (fd >= 0) break;
        if (errno != EEXIST) {
            error = "cannot create temporary output in " + parent.string() + ": " +
                    std::strerror(errno);
            return false;
        }
    }
    if (fd < 0) {
        error = "cannot reserve temporary output in " + parent.string();
        return false;
    }

    size_t written = 0;
    int write_error = 0;
    while (written < text.size()) {
#if defined(_WIN32)
        const unsigned int remaining = static_cast<unsigned int>(
            std::min<size_t>(text.size() - written, 0x7fffffffU));
        const int n = _write(fd, text.data() + written, remaining);
#else
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            write_error = errno;
            break;
        }
        if (n == 0) {
            write_error = EIO;
            break;
        }
        written += static_cast<size_t>(n);
    }
    if (!write_error) {
#if defined(_WIN32)
        if (_commit(fd) != 0) write_error = errno;
#else
        if (::fsync(fd) != 0) write_error = errno;
#endif
    }
#if defined(_WIN32)
    const int close_result = _close(fd);
#else
    const int close_result = ::close(fd);
#endif
    if (!write_error && close_result != 0) write_error = errno;
    if (write_error) {
        fs::remove(temp, ec);
        error = "cannot write " + temp.string() + ": " + std::strerror(write_error);
        return false;
    }

#if defined(_WIN32)
    if (!MoveFileExW(temp.wstring().c_str(), output.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        fs::remove(temp, ec);
        error = "cannot install " + output.string() + ": Windows error " +
                std::to_string(static_cast<unsigned long>(code));
        return false;
    }
#else
    fs::rename(temp, output, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(temp, rm_ec);
        error = "cannot install " + output.string() + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

struct ApiParam {
    std::string name;
    std::string type;
};

struct ExportedFn {
    std::string name;
    std::vector<ApiParam> params;
    std::string result;
    int line = 0;
};

struct ApiModule {
    std::string name;
    std::string file;
    std::vector<ExportedFn> exports;
};

std::vector<ApiModule> collect_api(ModuleSet &set) {
    flatten(set);
    std::map<std::string, std::pair<const FnDecl *, int>> definitions;
    for (const StmtP &sp : set.flat) {
        if (!sp) continue;
        const Stmt *node = sp.get();
        if (node->k == SK::Pub && !node->body.empty() && node->body.front())
            node = node->body.front().get();
        if (node->k != SK::Fn && node->k != SK::ExportFn) continue;
        for (const FnDecl &fn : node->fns) {
            if (!fn.name) continue;
            definitions[*fn.name] = {&fn, node->line > 0 ? node->line : sp->line};
        }
    }

    std::vector<ApiModule> modules;
    for (const SourceFile &file : set.files) {
        if (!file.ns) continue;
        ApiModule module;
        module.name = module_name(file);
        module.file = file.path;
        const std::string prefix = *file.ns + "__";
        for (const auto &[original, full] : file.functions) {
            if (!file.pub.count(original) || full.rfind(prefix, 0) != 0) continue;
            auto definition = definitions.find(full);
            if (definition == definitions.end()) continue;
            ExportedFn entry;
            entry.name = original;
            for (const Param &param : definition->second.first->params)
                entry.params.push_back({param.name, type_name(param.ann, "?")});
            entry.result = type_name(definition->second.first->ret, "none");
            entry.line = definition->second.second;
            module.exports.push_back(std::move(entry));
        }
        std::sort(module.exports.begin(), module.exports.end(),
                  [](const ExportedFn &a, const ExportedFn &b) { return a.name < b.name; });
        modules.push_back(std::move(module));
    }
    return modules;
}

std::string api_json(const std::string &root, const std::vector<ApiModule> &modules) {
    std::vector<ApiModule> sorted = modules;
    std::sort(sorted.begin(), sorted.end(),
              [](const ApiModule &a, const ApiModule &b) { return a.name < b.name; });
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n"
        << "  \"root\": " << json_quote(root) << ",\n"
        << "  \"modules\": [";
    if (sorted.empty()) {
        out << "]\n}\n";
        return out.str();
    }
    out << '\n';
    for (size_t i = 0; i < sorted.size(); ++i) {
        const ApiModule &module = sorted[i];
        out << "    {\n      \"module\": " << json_quote(module.name) << ",\n"
            << "      \"file\": " << json_quote(module.file) << ",\n"
            << "      \"exports\": ";
        if (module.exports.empty()) {
            out << "[]";
        } else {
            out << "[\n";
            for (size_t j = 0; j < module.exports.size(); ++j) {
                const ExportedFn &fn = module.exports[j];
                out << "        {\n          \"name\": " << json_quote(fn.name) << ",\n"
                    << "          \"params\": ";
                if (fn.params.empty()) {
                    out << "[]";
                } else {
                    out << "[\n";
                    for (size_t k = 0; k < fn.params.size(); ++k) {
                        const ApiParam &param = fn.params[k];
                        out << "            {\n              \"name\": " << json_quote(param.name) << ",\n"
                            << "              \"type\": " << json_quote(param.type) << "\n            }";
                        out << (k + 1 == fn.params.size() ? "\n" : ",\n");
                    }
                    out << "          ]";
                }
                out << ",\n          \"ret\": " << json_quote(fn.result) << ",\n"
                    << "          \"line\": ";
                if (fn.line > 0) out << fn.line;
                else out << "null";
                out << "\n        }" << (j + 1 == module.exports.size() ? "\n" : ",\n");
            }
            out << "      ]";
        }
        out << "\n    }" << (i + 1 == sorted.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string api_markdown(const std::string &root, const std::vector<ApiModule> &modules) {
    std::ostringstream out;
    out << "# " << std::filesystem::path(root).filename().string() << "\n\n";
    for (const ApiModule &module : modules) {
        out << "## " << module.name << "\n\n";
        if (module.exports.empty()) {
            out << "- (no exported functions)\n\n";
            continue;
        }
        std::vector<std::string> signatures;
        signatures.reserve(module.exports.size());
        for (const ExportedFn &fn : module.exports) {
            std::ostringstream sig;
            sig << '`' << fn.name << '(';
            for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i) sig << ", ";
                sig << fn.params[i].name << ": " << fn.params[i].type;
            }
            sig << ") -> " << fn.result << '`';
            signatures.push_back(sig.str());
        }
        std::sort(signatures.begin(), signatures.end());
        for (const std::string &signature : signatures) out << "- " << signature << '\n';
        out << '\n';
    }
    return out.str();
}

struct ApiChange {
    std::string kind;
    std::string name;
    std::string old_signature;
    std::string new_signature;
};

std::string interface_signature(const std::string &name,
                               const std::pair<std::vector<std::string>, std::string> &sig) {
    if (sig.second == "@struct" && name.rfind("struct:", 0) == 0) {
        std::ostringstream out;
        out << "struct " << name.substr(7) << " { ";
        for (size_t i = 0; i < sig.first.size(); ++i) {
            if (i) out << ", ";
            out << sig.first[i];
        }
        out << " }";
        return out.str();
    }
    if (sig.second == "@enum" && name.rfind("enum:", 0) == 0) {
        std::ostringstream out;
        out << "enum " << name.substr(5) << " { ";
        for (size_t i = 0; i < sig.first.size(); ++i) {
            if (i) out << ", ";
            out << sig.first[i];
        }
        out << " }";
        return out.str();
    }
    std::ostringstream out;
    out << "fn " << name << '(';
    for (size_t i = 0; i < sig.first.size(); ++i) {
        if (i) out << ", ";
        out << sig.first[i];
    }
    out << ") -> " << sig.second;
    return out.str();
}

std::vector<ApiChange> api_changes(const ModuleInterface &old_api,
                                   const ModuleInterface &new_api) {
    std::vector<ApiChange> changes;
    for (const auto &[name, old_sig] : old_api) {
        auto current = new_api.find(name);
        if (current == new_api.end()) {
            changes.push_back({"removed", name, interface_signature(name, old_sig), ""});
        } else if (current->second != old_sig) {
            changes.push_back({"changed", name, interface_signature(name, old_sig),
                               interface_signature(name, current->second)});
        }
    }
    for (const auto &[name, new_sig] : new_api) {
        if (!old_api.count(name))
            changes.push_back({"added", name, "", interface_signature(name, new_sig)});
    }
    std::sort(changes.begin(), changes.end(), [](const ApiChange &a, const ApiChange &b) {
        return a.name < b.name;
    });
    return changes;
}

std::string recommended_bump(const std::vector<ApiChange> &changes) {
    if (std::any_of(changes.begin(), changes.end(), [](const ApiChange &change) {
            return change.kind != "added";
        })) return "major";
    return changes.empty() ? "none" : "minor";
}

std::string api_diff_text(const std::string &old_path, const std::string &new_path,
                          const std::vector<ApiChange> &changes) {
    std::ostringstream out;
    out << "API diff: " << std::filesystem::path(old_path).filename().string()
        << " -> " << std::filesystem::path(new_path).filename().string() << "\n"
        << "Recommended version bump: " << recommended_bump(changes) << "\n";
    if (changes.empty()) {
        out << "No public signature changes.\n";
        return out.str();
    }
    for (const ApiChange &change : changes) {
        if (change.kind == "added") out << "+ " << change.new_signature << '\n';
        else if (change.kind == "removed") out << "- " << change.old_signature << '\n';
        else out << "~ " << change.old_signature << " => " << change.new_signature << '\n';
    }
    return out.str();
}

std::string api_diff_json(const std::string &old_path, const std::string &new_path,
                          const std::vector<ApiChange> &changes) {
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n"
        << "  \"old\": " << json_quote(old_path) << ",\n"
        << "  \"new\": " << json_quote(new_path) << ",\n"
        << "  \"recommended_bump\": " << json_quote(recommended_bump(changes)) << ",\n"
        << "  \"changes\": [";
    if (changes.empty()) {
        out << "]\n}\n";
        return out.str();
    }
    out << '\n';
    for (size_t i = 0; i < changes.size(); ++i) {
        const ApiChange &change = changes[i];
        out << "    {\n      \"kind\": " << json_quote(change.kind) << ",\n"
            << "      \"name\": " << json_quote(change.name);
        if (!change.old_signature.empty())
            out << ",\n      \"old\": " << json_quote(change.old_signature);
        if (!change.new_signature.empty())
            out << ",\n      \"new\": " << json_quote(change.new_signature);
        out << "\n    }" << (i + 1 == changes.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string root_path(const std::string &path) {
    std::error_code ec;
    auto out = std::filesystem::absolute(path, ec).lexically_normal();
    return ec ? path : out.string();
}

std::string load_error_message(const std::string &path, const std::exception &error) {
    if (dynamic_cast<const ModuleError *>(&error))
        return std::string("ModuleError: ") + error.what();
    if (const auto *parse = dynamic_cast<const ParseError *>(&error)) {
        std::string message = path + ":" + error.what();
        if (parse->line > 0) message += " at line " + std::to_string(parse->line);
        return message;
    }
    if (std::string_view(error.what()).rfind("cannot read ", 0) == 0) return error.what();
    return std::string("RuntimeError: ") + error.what();
}

void print_load_error(const char *command, const std::string &path, const std::exception &error) {
    const std::string message = load_error_message(path, error);
    std::fprintf(stderr, "brat %s: %s\n", command, message.c_str());
}

}  // namespace

int cmd_deps(int argc, char **argv) {
    bool want_json = false;
    bool want_dot = false;
    std::string output;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") continue;
        if (arg == "--json") { want_json = true; continue; }
        if (arg == "--dot") { want_dot = true; continue; }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                std::fputs("brat deps: -o requires a path\n", stderr);
                return 2;
            }
            output = argv[++i];
            continue;
        }
        if (arg == "--target") {
            if (i + 1 >= argc) {
                std::fputs("brat deps: --target requires a value\n", stderr);
                return 2;
            }
            ++i;
            continue;
        }
        if (arg.rfind("--target=", 0) == 0) continue;
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "brat deps: unknown flag %s\n", arg.c_str());
            return 2;
        }
        files.push_back(arg);
    }
    if (want_json && want_dot) {
        std::fputs("brat deps: --json and --dot cannot be combined\n", stderr);
        return 2;
    }
    if (files.size() != 1) {
        std::fprintf(stderr, "usage: brat deps FILE [--json|--dot] [-o OUT]\n");
        return 2;
    }
    const std::string root = root_path(files.front());
    ModuleSet set;
    try {
        set = load_modules(files.front());
    } catch (const std::exception &error) {
        print_load_error("deps", files.front(), error);
        return 1;
    }
    const std::vector<Edge> edges = collect_edges(set);
    const std::string dot = want_dot ? deps_dot(set, edges) : std::string();
    if (!output.empty()) {
        const std::string text = want_dot ? dot : deps_json(root, set.files.size(), edges);
        std::string error;
        if (!write_output(output, text, error)) {
            std::fprintf(stderr, "brat deps: %s\n", error.c_str());
            return 1;
        }
        std::printf("deps ok %s (%zu module(s), %zu edge(s))\n",
                    output.c_str(), set.files.size(), edges.size());
        return 0;
    }
    if (want_json) {
        std::fputs(deps_json(root, set.files.size(), edges).c_str(), stdout);
        return 0;
    }
    if (want_dot) {
        std::fputs(dot.c_str(), stdout);
        return 0;
    }
    for (const Edge &edge : edges) {
        std::printf("%s -> %s (%s)\n", edge.from.c_str(), edge.to.c_str(), edge.imported.c_str());
    }
    std::printf("%zu module(s), %zu import(s)\n", set.files.size(), edges.size());
    return 0;
}

int cmd_api(int argc, char **argv) {
    bool want_json = false;
    bool want_diff = false;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") continue;
        if (arg == "--json") { want_json = true; continue; }
        if (arg == "--diff") { want_diff = true; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "brat api: unknown flag %s\n", arg.c_str());
            return 2;
        }
        files.push_back(arg);
    }
    if (want_diff) {
        if (files.size() != 2) {
            std::fputs("usage: brat api --diff OLD.bri NEW.bri [--json]\n", stderr);
            return 2;
        }
        const std::string old_path = root_path(files[0]);
        const std::string new_path = root_path(files[1]);
        try {
            const ModuleInterface old_api = read_interface(files[0]);
            const ModuleInterface new_api = read_interface(files[1]);
            const std::vector<ApiChange> changes = api_changes(old_api, new_api);
            const std::string rendered = want_json
                ? api_diff_json(old_path, new_path, changes)
                : api_diff_text(files[0], files[1], changes);
            std::fputs(rendered.c_str(), stdout);
            return 0;
        } catch (const std::exception &error) {
            if (want_json) {
                std::ostringstream out;
                out << "{\n  \"version\": 1,\n"
                    << "  \"old\": " << json_quote(old_path) << ",\n"
                    << "  \"new\": " << json_quote(new_path) << ",\n"
                    << "  \"recommended_bump\": null,\n"
                    << "  \"changes\": [],\n"
                    << "  \"error\": " << json_quote(error.what()) << "\n}\n";
                std::fputs(out.str().c_str(), stdout);
            } else {
                std::fprintf(stderr, "brat api: %s\n", error.what());
            }
            return 1;
        }
    }
    if (files.size() != 1) {
        std::fputs("usage: brat api FILE [--json]\n       brat api --diff OLD.bri NEW.bri [--json]\n",
                   stderr);
        return 2;
    }
    const std::string root = root_path(files.front());
    ModuleSet set;
    try {
        set = load_modules(files.front());
        const std::vector<ApiModule> modules = collect_api(set);
        const std::string rendered = want_json ? api_json(root, modules) : api_markdown(root, modules);
        std::fputs(rendered.c_str(), stdout);
        return 0;
    } catch (const std::exception &error) {
        if (want_json) {
            std::ostringstream out;
            out << "{\n  \"version\": 1,\n  \"root\": " << json_quote(root)
                << ",\n  \"modules\": [],\n  \"error\": "
                << json_quote(load_error_message(files.front(), error)) << "\n}\n";
            std::fputs(out.str().c_str(), stdout);
        } else {
            print_load_error("api", files.front(), error);
        }
        return 1;
    }
}
