// Inferred channel protocols: `lang/engine/session.py` in C++.
//
// A `chan(n)` has two ends in one scope and nothing in the syntax says how many
// values cross. A straight-line pipe whose send and receive counts differ cannot
// finish, and that is what this pass refuses. Loops, branches and channels handed
// to a helper are printed by `brat check --proto` but never counted.
// The message is byte-identical to the reference implementation.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> session_check(const std::vector<StmtP> &body);
