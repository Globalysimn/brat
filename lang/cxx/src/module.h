// Module loading: a file plus everything it imports, in the order the reference
// visits them, and the flattened program the emitter works on.
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

struct SourceFile {
    std::string path;                  // real path, as the reference reports it
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
    // A re-export remembers where it came from, so a name two modules claim is a
    // diagnostic instead of whichever statement ran last.
    std::map<std::string, std::pair<std::string, std::string>> reexports;
    // A sibling `.bri` promises names, arity, parameter types and return type.
    std::optional<std::map<std::string, std::pair<std::vector<std::string>, std::string>>> iface;
};

struct ModuleSet {
    std::vector<SourceFile> files;
    // The root's body with every `import` spliced in place and every name
    // mangled: exactly what `codegen_c.py` emits from.
    std::vector<StmtP> flat;
};

// Throws a plain runtime_error for the root file (unreadable, bad syntax) and
// ModuleError for an import.
ModuleSet load_modules(const std::string &path);
ModuleSet load_modules(const std::string &path, const std::string &root_source);
// Mangles names and fills `flat`. Prints nothing; throws ModuleError when the
// program breaks one of the reference's link-time rules.
void flatten(ModuleSet &set);
