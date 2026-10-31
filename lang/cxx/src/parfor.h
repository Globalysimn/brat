// Monoid reducer validation for `par for`.
//
// Two refusals and one prohibition, all of them about purity:
//   * the folding function must be a two-parameter function of this program;
//   * it must have no effects (its row is empty), because a fold whose operation
//     prints or spawns depends on the thread order;
//   * the body must have no effects either, and must not write a place that
//     lives outside the loop.
// The messages are byte-identical to the reference implementation.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> parfor_rules(const std::vector<StmtP> &body,
                                     const FnTable &funcs);
