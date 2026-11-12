// Module loading: a file plus everything it imports, depth first, and the
// flattened program the emitter works on.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"

struct ModuleError : std::runtime_error {
    explicit ModuleError(const std::string &msg) : std::runtime_error(msg) {}
};

using ModuleInterface =
    std::map<std::string, std::pair<std::vector<std::string>, std::string>>;

struct SourceFile {
    std::string path;                  // real path, as diagnostics report it
    std::string src;
    std::vector<StmtP> body;
    // `{...}` chunks the parser kept as literal text: the checker warns once
    // per file, so they travel with the file they came from.
    std::vector<std::pair<uint32_t, std::string>> stray;
    std::optional<std::string> ns;     // `a__b` for a module two levels down
    std::vector<std::pair<std::string, size_t>> imports;  // dotted name -> files[]
    std::map<std::string, std::string> functions;         // original -> mangled
    std::map<std::string, std::vector<std::optional<std::string>>> callback_params;
    std::set<std::string> pub;
    std::map<std::string, int> pub_ctors;                 // variant -> arity for exported enums
    std::set<std::string> priv_ctors;                     // variant names of private enums
    // A re-export remembers where it came from, so a name two modules claim is a
    // diagnostic instead of whichever statement ran last.
    std::map<std::string, std::pair<std::string, std::string>> reexports;
    // A sibling `.bri` promises names, arity, parameter types and return type.
    std::optional<ModuleInterface> iface;
};

struct ModuleSet {
    std::vector<SourceFile> files;
    // The root's body with every `import` spliced in place and every name
    // mangled: exactly what the native emitter consumes.
    std::vector<StmtP> flat;
    std::map<const Stmt *, std::string> stmt_files;
    std::map<std::string, std::string> fn_files;
    bool mangled = false;
};

// Throws a plain runtime_error for the root file (unreadable, bad syntax) and
// ModuleError for an import.
void set_stdlib_root(const std::string &dir);
ModuleInterface read_interface(const std::string &path);
ModuleSet load_modules(const std::string &path);
ModuleSet load_modules(const std::string &path, const std::string &root_source);
void mangle_modules(ModuleSet &set);
// Mangles names and fills `flat`. Prints nothing; throws ModuleError when the
// program breaks a link-time rule. When `prune_imports` is true, unreferenced
// non-exported functions from imported modules (`idx > 0`) are left out of
// `flat`; the root module is kept whole.
void flatten(ModuleSet &set, bool prune_imports = false);
