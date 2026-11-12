// Intraprocedural ownership and borrow checker.
//
// Tracks per-file function parameter modes, active borrows, moves, and
// diagnostics with optional source-line notes.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"

struct BorrowNote {
    int line = 0;                      // 0 = the note has no line of its own
    std::string msg;
};

struct BorrowDiag {
    int line = 0;                      // 0 = no line; printed as `file: error: ...`
    std::string msg;
    std::optional<BorrowNote> note;

    bool operator==(const BorrowDiag &o) const {
        if (line != o.line || msg != o.msg) return false;
        if (note.has_value() != o.note.has_value()) return false;
        if (!note) return true;
        return note->line == o.note->line && note->msg == o.note->msg;
    }
};

struct ParamMode {
    std::string name;
    Mode mode = Mode::Ref;
};

// Everything `callee_known` and `scan_calls` ask about a function.
struct FnTable {
    std::map<std::string, std::vector<ParamMode>> sigs;
    std::set<std::string> heads;       // legal dotted-callee heads
    std::map<std::string, bool> umeth;  // method name -> takes `mut self`
    std::set<std::string> owned_ret;   // `extern ... -> move`: free it or return it
    std::map<std::string, std::vector<size_t>> borrows_ret;  // `borrows(p)` param indices
    // `extern` results that are scalar slots: a C function returning a number
    // returns a value, so a `let` bound to it is a copy and not a borrow.
    std::set<std::string> scalar_ret;
    std::set<std::string> pub_consts;
    std::map<std::string, std::map<std::string, std::string>> struct_fields;
    std::map<std::string, std::string> fn_rets;
};

struct BorrowOut {
    std::vector<BorrowDiag> errs;
    std::vector<BorrowDiag> warns;
};

// The names another file exports: `pub fn` parameter modes and `pub enum`
// variants, merged per file before the set is checked.
FnTable pub_sigs(const std::vector<StmtP> &body,
                 const std::set<std::string> &exp_fns = {});

// Every `fn` statement in a body, at any depth, in source order.
void nested_fns(const std::vector<StmtP> &body, std::vector<const Stmt *> &out);

// `stray` is what the parser kept as literal `{...}` text for this file.
BorrowOut borrow_check(const std::vector<StmtP> &body,
                       const std::map<std::string, std::vector<ParamMode>> &extra,
                       const std::vector<std::pair<uint32_t, std::string>> &stray,
                       const std::set<std::string> &module_heads = {},
                       const std::set<std::string> &extra_consts = {},
                       const std::set<std::string> &extra_scalar_rets = {},
                       const std::map<std::string, std::vector<size_t>> &extra_borrows_rets = {},
                       const std::set<std::string> &extra_owned_rets = {},
                       const std::map<std::string, std::map<std::string, std::string>> &extra_struct_fields = {},
                       const std::map<std::string, std::string> &extra_fn_rets = {});
