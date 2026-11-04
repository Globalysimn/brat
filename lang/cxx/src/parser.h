// Recursive descent, one token of lookahead, no backtracking.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"

struct ParseError : std::runtime_error {
    uint32_t line;
    ParseError(std::string msg, uint32_t line)
        : std::runtime_error(std::move(msg)), line(line) {}
};

struct ParseOut {
    std::vector<StmtP> body;
    // `{...}` chunks that stayed literal but look like a failed placeholder.
    std::vector<std::pair<uint32_t, std::string>> stray;
};

ParseOut parse(std::string_view src, uint32_t start_line = 1);
StmtP clone_stmt_header(const Stmt &s);
