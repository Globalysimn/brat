// Channel send/recv protocol balance checker.
//
// A `chan(n)` has two ends in one scope and nothing in the syntax says how many
// values cross. A straight-line pipe whose send and receive counts differ cannot
// finish, and that is what this pass refuses.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> session_check(const std::vector<StmtP> &body);
