#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"

namespace native_ffi {

struct AbiMatrixResult {
    int status = 1;
    std::string json;
    std::string text;
    std::string error;
};

AbiMatrixResult abi_matrix(const std::string &source_path,
                           const std::string &header_path,
                           bool preprocess = false,
                           const std::vector<std::string> &include_dirs = {},
                           const std::vector<std::string> &defines = {});

bool verify_module_header(const std::vector<StmtP> &body,
                          std::string_view header_text,
                          std::string_view header_name,
                          std::string_view target,
                          std::string &error);

bool verify_bratbind_bytes(std::string_view brat_bytes,
                           std::string_view bind_bytes,
                           std::string_view target,
                           const std::map<std::string, std::string> &object_files,
                           std::string_view label,
                           std::string &error);

bool verify_module_bratbind_file(const std::string &brat_path,
                                 std::string_view brat_src,
                                 std::string_view target,
                                 bool &proved_out,
                                 std::string &error_out);

bool has_any_ffi_surface(const std::vector<StmtP> &body);

int ffi_gate(const std::string &entry_path,
             const std::vector<std::pair<std::string, std::string>> &module_sources,
             const std::string &header_path,
             const std::vector<std::string> &object_paths,
             const std::string &target,
             const std::string &who,
             bool unsafe_ffi,
             bool quiet = false);

int cmd_ffi(int argc, char **argv);
int cmd_bindgen(int argc, char **argv);

}  // namespace native_ffi
