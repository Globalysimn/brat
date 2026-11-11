// Package manager and lockfile verification (`brat pkg`).

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
    } else if (executable_on_path("clang")) {
        compiler = "clang";
    } else {
        error = "no C compiler found (set BRAT_CC)";
        return false;
    }
    const auto cc_parts = split_compiler_cmd(compiler);
    if (cc_parts.empty()) {
        error = "no C compiler found (set BRAT_CC)";
        return false;
    }
    zig = std::filesystem::path(cc_parts[0]).filename().string().rfind("zig", 0) == 0 &&
          !(cc_parts.size() >= 2 && cc_parts[1] == "cc");
    const bool zig_any = std::filesystem::path(cc_parts[0]).filename().string().rfind("zig", 0) == 0;
    if (!target.empty() && !zig_any) {
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
        std::vector<std::string> args = cc_parts;
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
            if (!tape.is_object(files_node)) {
                std::fprintf(stderr, "brat pkg: invalid lock: %s missing files object\n", pkg_name.c_str());
                return 1;
            }
            for (uint32_t k = tape.at(files_node).first_child; k != 0;) {
                const uint32_t v = tape.at(k).next_sibling;
                if (v == 0) break;
                if (tape.is_string(k) && tape.is_string(v))
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
        std::fprintf(stderr, "usage: brat pkg update [NAME] [--dry-run]\n");
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
        std::fprintf(stderr, "usage: brat pkg publish OWNER/NAME@VERSION SOURCE [--depends OWNER/NAME@CONSTRAINT]...\n");
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

int cmd_pkg(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: brat pkg {add|install|update|verify|list|remove|publish} [ARGS]\n");
        return 2;
    }
    const std::string sub = argv[2];
    if (sub == "--help" || sub == "-h" || sub == "help") {
        std::fprintf(stdout,
                     "usage: brat pkg {add|install|update|verify|list|remove|publish} [ARGS]\n"
                     "       brat pkg add <SPEC|URL> [--target TRIPLE]\n"
                     "       brat pkg install [--target TRIPLE]\n"
                     "       brat pkg update [NAME] [--dry-run]\n"
                     "       brat pkg verify [--target TRIPLE]\n"
                     "       brat pkg list\n"
                     "       brat pkg remove NAME\n"
                     "       brat pkg publish OWNER/NAME@VERSION SOURCE [--depends OWNER/NAME@CONSTRAINT]...\n");
        return 0;
    }
    if (sub == "publish") return native_pkg_publish(argc, argv);
    if (sub == "update") return native_pkg_update(argc, argv);
    if (sub == "add") {
        std::string target;
        std::vector<std::string> pos;
        if (!parse_pkg_target_args(argc, argv, 3, target, pos) || pos.size() != 1) {
            std::fprintf(stderr, "usage: brat pkg add <SPEC|URL> [--target TRIPLE]\n");
            return 2;
        }
        return native_pkg_add(pos[0], target);
    }
    if (sub == "install") {
        std::string target;
        std::vector<std::string> pos;
        if (!parse_pkg_target_args(argc, argv, 3, target, pos) || !pos.empty()) {
            std::fprintf(stderr, "usage: brat pkg install [--target TRIPLE]\n");
            return 2;
        }
        return native_pkg_install(target);
    }
    if (sub == "list") {
        if (argc != 3) {
            std::fprintf(stderr, "usage: brat pkg list\n");
            return 2;
        }
        return native_pkg_list();
    }
    if (sub == "remove") {
        if (argc != 4) {
            std::fprintf(stderr, "usage: brat pkg remove NAME\n");
            return 2;
        }
        return native_pkg_remove(argv[3]);
    }
    if (sub != "verify") {
        std::fprintf(stderr,
                     "brat pkg: unknown subcommand '%s'\n"
                     "usage: brat pkg {add|install|update|verify|list|remove|publish} [ARGS]\n",
                     sub.c_str());
        return 2;
    }
    std::string target;
    std::vector<std::string> pos;
    if (!parse_pkg_target_args(argc, argv, 3, target, pos) || !pos.empty()) {
        std::fprintf(stderr, "usage: brat pkg verify [--target TRIPLE]\n");
        return 2;
    }
    return native_pkg_verify(target);
}

