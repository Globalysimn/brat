// The SSA form and proof-carrying abstract interpretation passes:
// - `-O1` (`ir::analyze`): wrapping `i64` intervals on fast `i64` functions.
// - `-O2` (`ir::analyze_o2`): reduced-product relational abstract interpreter
//   (`IV × Granger Congruence mZ+r × Potential-Graph DBM`) with interprocedural
//   return summaries, nested/strided loops, and `vec[i64]` bounds-check elision.
#pragma once

#include <cstdint>
#include <map>
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
    // The `Bin` nodes whose div/mod check this pass proved unnecessary (raw C `/` or `%`).
    std::set<const Expr *> unchecked;
    // `-O2`: `Bin` nodes whose divisor is proven non-zero (`bf_idiv_nz` / `bf_imod_nz`, no trap).
    std::set<const Expr *> nonzero_div;
    // `-O2`: `EK::Idx` (`v[i]`) and `SK::SetIdx` (`v[i] = x`) nodes proven in-bounds.
    std::set<const Expr *> unchecked_idx;
    std::set<const Stmt *> unchecked_setidx;
    // One proof per elided check, in the order they were proven.
    std::vector<Proof> proofs;
};

struct FnSummary {
    bool valid = false;
    bool top = true;
    int64_t lo = 0, hi = 0;
    uint64_t mod = 1;   // 0 => constant (int64_t)rem, 1 => top, >=2 => mod*Z + rem
    uint64_t rem = 0;
};

// `-O1` interval pass (preserved byte-for-byte for `-O1` callers).
bool analyze(const std::string &fn, const std::vector<Param> &params,
             const std::vector<StmtP> &body, Result &out);

// `-O2` reduced-product relational pass (`IV × Cong × DBM` + summaries).
bool analyze_o2(const std::string &fn, const std::vector<Param> &params,
                const std::set<std::string> &vec_params,
                const std::vector<StmtP> &body,
                const std::map<std::string, FnSummary> &summaries,
                Result &out, FnSummary *out_summary = nullptr);

}  // namespace ir
