// The capability lattice: `lang/engine/caps.py` in C++.
//
// One answer per place, computed over the control flow instead of over a list of
// live views. `borrow.cpp` decides a write by asking "is any view of this root
// live here", which is a lexical approximation: the list cannot say whether the
// view is live on this path or on some other one. This pass keeps, per place,
// the weakest capability any path may have left behind — none < read <
// moved < write < top — and joins the two sides of a branch with that order.
//
// `borrow_check` runs first and a verdict here is kept only when the walk made no
// finding on the same line, so the diagnostics only ever gain what a lexical walk
// cannot see. `lang/tests/test_caps_diff.py` holds the two to the same words on
// 207 programs.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> cap_verdicts(
    const std::vector<StmtP> &body,
    const std::map<std::string, std::vector<ParamMode>> &sigs = {});
