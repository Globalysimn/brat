// Plugins at run time: `lang/engine/plugin.py` in C++.
//
// A symbol or a pointer made through a symbol belongs to the library that made
// it, so `lib_close` ends every value derived from that handle, and using one
// afterwards is an error that names the value, the library and the line of the
// close. When the program also declares the function (`extern fn`), the literal
// signature in `call` is compared against that declaration instead of being
// believed. The wording is byte-identical to the reference implementation
// because the suite diffs the two.
#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "borrow.h"

std::vector<BorrowDiag> plugin_check(const std::vector<StmtP> &body);
