#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lsp_wire.h"

namespace pkg_archive {

struct ArchiveDepSpec {
    std::string expected_name;
    std::string spec;
    std::string sha256;
};

struct ArchiveManifest {
    std::string version;
    std::vector<ArchiveDepSpec> dependencies;
    std::vector<std::string> c_sources;
};

struct ArchivePlanItem {
    std::string archive;
    std::string url;
    std::string sha256;
    std::map<std::string, std::string> extracted;
    std::map<std::string, std::string> files;
    std::string version;
    std::vector<std::string> dependencies;
    std::vector<std::string> c_sources;
};

bool valid_archive_name(std::string_view value);
bool valid_archive_relpath(std::string_view rel, std::string_view pkg_name);

bool expand_github_spec(std::string_view spec,
                        std::string &pkg_name,
                        std::string &url,
                        std::string &ref,
                        std::string &error);

bool is_archive_url(std::string_view url);
bool archive_pkg_name_from_url(std::string_view url,
                               std::string &pkg_name,
                               std::string &error);
std::string url_unquoted_basename(std::string_view url);

bool extract_tar_package(std::string_view raw_bytes,
                         const std::string &pkg_name,
                         const std::string &target,
                         std::map<std::string, std::string> &extracted,
                         std::map<std::string, std::string> &file_hashes,
                         ArchiveManifest &manifest,
                         std::string &error);

bool resolve_archive_graph(std::string_view root_spec,
                           bool allow_file_root,
                           const std::string &target,
                           const lsp_wire::LspJsonTape &lock_tape,
                           bool lock_exists,
                           std::vector<ArchivePlanItem> &out_plan,
                           std::string &error);

}  // namespace pkg_archive
