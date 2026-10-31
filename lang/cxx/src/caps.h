// Intraprocedural capability lattice (none < read < moved < write < top).
//
// Tracks per-place capabilities across control-flow branches so branch-sensitive
// move/borrow conflicts that a purely lexical live-view list misses are caught,
// reporting the losing branch state when two paths disagree.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> cap_verdicts(
    const std::vector<StmtP> &body,
    const std::map<std::string, std::vector<ParamMode>> &sigs = {});
