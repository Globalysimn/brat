// The SSA form and the first optimization pass: a proof-carrying elision of
// division checks over wrapping i64 intervals.
//
// Scope of this first step, on purpose: one function, i64 values, no loops, no
// ownership, no containers, no floats. A body with a shape outside that set is
// refused whole and keeps every check, so `-O1` can only ever change a function
// it models completely.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "ast.h"

namespace ir {

// One removed check and the reason, formatted for `--why-opt`.
struct Proof {
    std::string fn;
    int line = 0;
    std::string text;
};

struct Result {
    // The `Bin` nodes whose div/mod check this pass proved unnecessary.
    std::set<const Expr *> unchecked;
    // One proof per element of `unchecked`, in the order they were proven.
    std::vector<Proof> proofs;
};

// Builds the IR for `body` and runs the interval pass over it. Returns false
// when the function falls outside the modelled subset, in which case `out` is
// empty: no proof, no elision, no change to the emitted C.
bool analyze(const std::string &fn, const std::vector<Param> &params,
             const std::vector<StmtP> &body, Result &out);

}  // namespace ir
