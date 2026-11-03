#ifndef BRAT_DRIVER_TOOLS_H
#define BRAT_DRIVER_TOOLS_H

#include <string>
#include <string_view>
#include <vector>

bool has_arena_block_tokens(std::string_view src);
int cmd_check_json(const std::vector<std::string> &files);
int cmd_fmt(int argc, char **argv);
int cmd_explain(int argc, char **argv);
int cmd_new(int argc, char **argv);
int cmd_doc(int argc, char **argv);
int cmd_test(int argc, char **argv);
int cmd_emit_c(int argc, char **argv);
int cmd_dist(int argc, char **argv);
int cmd_ffi_native(int argc, char **argv);

#endif
