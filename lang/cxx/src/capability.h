// Concurrency sharing and capture rules.
//
// Two refusals, both about the heap graph rather than about mutability:
// an insert that closes a reference cycle, and a `spawn` write that the parent
// can reach before a `join_all()`. The messages are byte-identical to the
// reference implementation because the suite diffs them.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> sharing_check(
    const std::vector<StmtP> &body,
    const std::map<std::string, std::map<std::string, std::string>> &extra_structs = {});
