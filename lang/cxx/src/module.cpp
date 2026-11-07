// Module loader, symbol mangler, and whole-program flattener.
//
// Two views of one program come out of here. `files` is what the checker wants --
// every file on its own, unmangled, with its own source. `flat` is what the
// emitter wants: the root's body with every `import` spliced in place and every
// name qualified, so two modules can define `helper` without colliding.
#include "module.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "lexer.h"
#include "parser.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path g_stdlib_override;

fs::path probe_stdlib_near(const fs::path &raw_exe) {
    std::error_code ec;
    fs::path exe = fs::canonical(raw_exe, ec);
    if (ec) exe = raw_exe;
    const fs::path dir = exe.parent_path();
    const fs::path cands[] = {
        dir.parent_path().parent_path() / "std",
        dir.parent_path() / "lang" / "std",
        dir.parent_path() / "std",
    };
    for (const auto &c : cands) {
        std::error_code ec2;
        fs::path p = fs::weakly_canonical(c, ec2);
        if (!ec2 && fs::is_directory(p, ec2)) return p;
    }
    return {};
}

fs::path stdlib_root() {
    if (const char *env = std::getenv("BRAT_STDLIB"); env && *env) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::absolute(env), ec);
        if (!ec && fs::is_directory(p, ec)) return p;
        return {};
    }
    if (!g_stdlib_override.empty()) {
        std::error_code ec;
        if (fs::is_directory(g_stdlib_override, ec)) return g_stdlib_override;
    }
#if defined(_WIN32)
    wchar_t wbuf[32768];
    DWORD wlen = GetModuleFileNameW(nullptr, wbuf, static_cast<DWORD>(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wlen > 0 && wlen < sizeof(wbuf) / sizeof(wbuf[0])) {
        fs::path found = probe_stdlib_near(fs::path(std::wstring(wbuf, wlen)));
        if (!found.empty()) return found;
    }
#elif defined(__APPLE__)
    uint32_t msz = 0;
    (void)_NSGetExecutablePath(nullptr, &msz);
    if (msz > 0 && msz < 32768) {
        std::string mbuf(msz, '\0');
        if (_NSGetExecutablePath(mbuf.data(), &msz) == 0) {
            fs::path found = probe_stdlib_near(fs::path(mbuf.c_str()));
            if (!found.empty()) return found;
        }
    }
#elif defined(__linux__)
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        fs::path found = probe_stdlib_near(exe);
        if (!found.empty()) return found;
    }
#endif
    return {};
}

fs::path vendor_root(const fs::path &base) {
    std::error_code ec;
    fs::path direct = base / "vendor";
    if (fs::is_directory(direct, ec)) return fs::weakly_canonical(direct, ec);
    fs::path cur = base;
    for (;;) {
        fs::path parent = cur.parent_path();
        if (parent.empty() || parent == cur) return {};
        if (fs::is_regular_file(parent / "brat.lock", ec) &&
            fs::is_directory(parent / "vendor", ec)) {
            return fs::weakly_canonical(parent / "vendor", ec);
        }
        cur = parent;
    }
}

bool confined(const fs::path &base, const fs::path &path) {
    if (base.empty()) return false;
    auto b = base.begin();
    auto p = path.begin();
    for (; b != base.end(); ++b, ++p) {
        if (b->empty()) continue;                  // trailing separator
        if (p == path.end() || *b != *p) return false;
    }
    return true;
}

std::string read_all(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// `a.b` -> {"a", "b"}; anything that could climb out is rejected first.
std::vector<std::string> split_name(const std::string &name) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    for (const auto &p : parts) {
        if (p.empty() || p == "." || p == ".." || p.find('/') != std::string::npos ||
            p.find('\\') != std::string::npos) {
            throw ModuleError("invalid module name " + name);
        }
    }
    return parts;
}

std::string module_namespace(const fs::path &base, const fs::path &path) {
    fs::path rel = fs::relative(path, base);
    std::string text = rel.generic_string();
    if (text.size() > 5 && text.compare(text.size() - 5, 5, ".brat") == 0) {
        text.resize(text.size() - 5);
    }
    std::string out;
    for (char c : text) {
        if (c == '/' || c == '\\') {
            out += "__";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Dotted name of a var/field chain, or nothing when `e` computes a value.
std::optional<std::string> name_path(const Expr *e) {
    if (!e) return std::nullopt;
    if (e->k == EK::Var) return e->a;
    if (e->k == EK::Field) {
        auto base = name_path(e->kids[0].get());
        if (!base) return std::nullopt;
        return *base + "." + e->b;
    }
    return std::nullopt;
}

const Stmt *inner(const Stmt &s) {
    if (s.k == SK::Pub && !s.body.empty()) return s.body[0].get();
    return &s;
}

std::string param_ann(const Param &p) {
    if (p.ann && !p.ann->empty()) return *p.ann;
    return "none";
}

std::string fn_ret(const FnDecl &f) {
    if (f.ret && !f.ret->empty()) return *f.ret;
    return "none";
}


// Each `fn name(params) -> type` line is parsed with a body appended, so the
// signature syntax is the same one an implementation uses.
ModuleInterface parse_interface(const fs::path &path) {
    ModuleInterface sigs;
    std::string src = read_all(path);
    uint32_t bad_line = 1;
    if (!utf8_valid(src, bad_line, 1))
        throw ModuleError(path.string() + ":" + std::to_string(bad_line) +
                          ": bad interface line: source is not valid UTF-8 at line 1");
    std::vector<std::string> lines;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    int number = 0;
    for (const std::string &raw : lines) {
        number++;
        size_t b = raw.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = raw.find_last_not_of(" \t\r\n");
        std::string text = raw.substr(b, e - b + 1);
        if (text.compare(0, 2, "//") == 0) continue;
        if (text.rfind("struct ", 0) == 0 || text.rfind("pub struct ", 0) == 0) {
            std::vector<StmtP> sbody;
            try {
                sbody = parse(text).body;
            } catch (ParseError &err) {
                std::string detail = err.what();
                if (err.line > 0) detail += " at line " + std::to_string(err.line);
                throw ModuleError(path.string() + ":" + std::to_string(number) +
                                  ": bad interface line: " + detail);
            }
            if (sbody.size() == 1 && inner(*sbody[0])->k == SK::Struct) {
                const Stmt *st = inner(*sbody[0]);
                std::vector<std::string> fspecs;
                for (const auto &fld : st->fields)
                    fspecs.push_back(fld.name + ": " + fld.ann.value_or("none"));
                sigs["struct:" + st->a] = {std::move(fspecs), "@struct"};
                continue;
            }
        }
        std::vector<StmtP> body;
        try {
            body = parse(text + " { return 0 }").body;
        } catch (ParseError &err) {
            std::string detail = err.what();
            if (err.line > 0) detail += " at line " + std::to_string(err.line);
            throw ModuleError(path.string() + ":" + std::to_string(number) +
                              ": bad interface line: " + detail);
        }
        bool ok = body.size() == 1 && inner(*body[0])->k == SK::Fn;
        if (ok) {
            const FnDecl &f = inner(*body[0])->fns[0];
            std::vector<std::string> anns;
            for (const Param &p : f.params) anns.push_back(param_ann(p));
            sigs[f.name.value_or("")] = {anns, fn_ret(f)};
        } else {
            throw ModuleError(path.string() + ":" + std::to_string(number) +
                              ": bad interface line: expected `fn name(params) -> type`");
        }
    }
    return sigs;
}


struct Loader {
    fs::path base;
    fs::path std_base;
    fs::path vendor_base;
    ModuleSet set;
    std::map<std::string, size_t> by_path;

    bool is_allowed(const fs::path &path) const {
        return confined(base, path) ||
               (!std_base.empty() && confined(std_base, path)) ||
               (!vendor_base.empty() && confined(vendor_base, path));
    }

    size_t visit(const fs::path &raw, bool root, const std::string &import_name,
                 const std::string *root_source = nullptr) {
        fs::path path = fs::weakly_canonical(raw);
        std::string key = path.string();
        auto seen = by_path.find(key);
        if (seen != by_path.end()) return seen->second;
        if (!is_allowed(path) || (!(root && root_source) && !fs::is_regular_file(path))) {
            if (root) throw std::runtime_error("cannot read " + key);
            throw ModuleError("no module " + import_name);
        }
        SourceFile f;
        f.path = key;
        f.src = root && root_source ? *root_source : read_all(path);
        try {
            ParseOut parsed = parse(f.src);
            f.body = std::move(parsed.body);
            f.stray = std::move(parsed.stray);
        } catch (ParseError &e) {
            if (root) throw;
            throw ParseError(std::string(e.what()) + " (module " + import_name + ")", e.line);
        }
        if (!root) {
            if (!std_base.empty() && confined(std_base, path) && !confined(base, path)) {
                f.ns = "std__" + module_namespace(std_base, path);
            } else if (!vendor_base.empty() && confined(vendor_base, path) && !confined(base, path)) {
                f.ns = "vendor__" + module_namespace(vendor_base, path);
            } else {
                f.ns = module_namespace(base, path);
            }
        }
        set.files.push_back(std::move(f));
        size_t idx = set.files.size() - 1;
        by_path[key] = idx;
        scan_imports(idx);
        return idx;
    }

    void scan_imports(size_t idx) {
        fs::path dir = fs::path(set.files[idx].path).parent_path();
        std::vector<std::string> names;
        for (const auto &sp : set.files[idx].body) {
            if (sp->k == SK::Import) {
                bool seen = false;
                for (const auto &kv : set.files[idx].imports) seen = seen || kv.first == sp->a;
                if (!seen) names.push_back(sp->a);
            }
        }
        for (const auto &name : names) {
            std::vector<std::string> parts = split_name(name);
            std::string joined;
            for (const auto &part : parts) {
                if (!joined.empty()) joined += "/";
                joined += part;
            }
            fs::path target;
            bool found = false;
            std::vector<std::string> tried_paths;
            for (const fs::path &cand :
                 {dir / (joined + ".brat"), dir / "vendor" / (joined + ".brat")}) {
                fs::path real = fs::weakly_canonical(cand);
                tried_paths.push_back(fs::relative(real, base).generic_string());
                if (is_allowed(real) && fs::is_regular_file(real)) {
                    target = real;
                    found = true;
                    break;
                }
            }
            if (!found && !vendor_base.empty()) {
                fs::path rv = fs::weakly_canonical(vendor_base / (joined + ".brat"));
                if (confined(vendor_base, rv) && fs::is_regular_file(rv)) {
                    target = rv;
                    found = true;
                }
            }
            if (!found && parts[0] == "std" && parts.size() >= 2 && !std_base.empty()) {
                std::string sub;
                for (size_t k = 1; k < parts.size(); k++) {
                    if (!sub.empty()) sub += "/";
                    sub += parts[k];
                }
                fs::path sc = fs::weakly_canonical(std_base / (sub + ".brat"));
                if (confined(std_base, sc) && fs::is_regular_file(sc)) {
                    target = sc;
                    found = true;
                }
            }
            if (!found) {
                std::string dirrel = fs::relative(dir, base).generic_string();
                std::string where = dirrel == "." || dirrel.empty() ? "the program root" : dirrel;
                std::string tried;
                for (const auto &cand : tried_paths) {
                    if (!tried.empty()) tried += "`, `";
                    tried += cand;
                }
                throw ModuleError("no module " + name + " (tried `" + tried + "` from " + where +
                                  "; imports are relative to the importing file, so a module "
                                  "next to it is imported by its short name)");
            }
            size_t child = visit(target, false, name);
            set.files[idx].imports.emplace_back(name, child);
        }
    }
};


struct Mangler {
    explicit Mangler(ModuleSet &s) : set(s) {}

    ModuleSet &set;
    std::map<std::string, std::vector<std::pair<size_t, std::string>>> public_names;
    std::map<std::string, std::pair<size_t, std::string>> private_names;
    std::map<std::string, std::vector<std::optional<std::string>>> callback_params;

    std::string mangle(size_t cur, const std::string &name) {
        const SourceFile &f = set.files[cur];
        if (!f.ns) return name;
        return *f.ns + "__" + name;
    }

    std::string module_name(size_t cur) {
        if (!set.files[cur].ns) {
            if (!set.files[cur].path.empty()) {
                return fs::path(set.files[cur].path).stem().string();
            }
            return "<root>";
        }
        const std::string &ns = *set.files[cur].ns;
        std::string out;
        for (size_t i = 0; i < ns.size(); i++) {
            if (ns.compare(i, 2, "__") == 0) {
                out.push_back('.');
                i++;
            } else {
                out.push_back(ns[i]);
            }
        }
        return out;
    }

    std::optional<std::string> resolve_qualified(const std::string &name, size_t cur) {
        size_t dot = name.rfind('.');
        std::string qualifier = name.substr(0, dot);
        std::string fn = name.substr(dot + 1);
        std::vector<size_t> targets;
        for (const auto &[imported, target] : set.files[cur].imports) {
            bool match = qualifier == imported;
            if (!match) {
                size_t tail = imported.rfind('.');
                match = qualifier == (tail == std::string::npos ? imported
                                                               : imported.substr(tail + 1));
            }
            if (match && std::find(targets.begin(), targets.end(), target) == targets.end()) {
                targets.push_back(target);
            }
        }
        if (targets.empty()) return std::nullopt;
        if (targets.size() != 1) {
            throw ModuleError("ambiguous module qualifier `" + qualifier + "`");
        }
        size_t target = targets[0];
        auto it = set.files[target].functions.find(fn);
        if (it == set.files[target].functions.end()) {
            throw ModuleError("module `" + qualifier + "` has no function `" + fn + "`");
        }
        if (target != cur && !set.files[target].pub.count(fn)) {
            throw ModuleError("function `" + fn + "` is private in module `" +
                              module_name(target) + "`");
        }
        return it->second;
    }

    std::string resolve_call(const std::string &name, size_t cur) {
        if (name.find('.') != std::string::npos) {
            auto got = resolve_qualified(name, cur);
            return got ? *got : name;
        }
        auto own = set.files[cur].functions.find(name);
        if (own != set.files[cur].functions.end()) return own->second;
        auto priv = private_names.find(name);
        if (priv != private_names.end() && priv->second.first != cur) {
            throw ModuleError("function `" + priv->second.second + "` is private in module `" +
                              module_name(priv->second.first) + "`");
        }
        auto pub = public_names.find(name);
        if (pub == public_names.end()) return name;
        std::vector<std::string> unique;
        for (const auto &[owner, full] : pub->second) {
            if (std::find(unique.begin(), unique.end(), full) == unique.end()) {
                unique.push_back(full);
            }
        }
        if (unique.size() == 1) return unique[0];
        if (unique.size() > 1) {
            throw ModuleError("ambiguous unqualified name `" + name + "`: defined in both " +
                              set.files[pub->second[0].first].path + " and " +
                              set.files[pub->second[1].first].path + "; qualify the call");
        }
        return name;
    }


    void resolve_callback_args(std::vector<ExprP> &args, const std::string &original,
                               const std::string &resolved, size_t cur) {
        auto local = set.files[cur].callback_params.find(original);
        const std::vector<std::optional<std::string>> *params = nullptr;
        if (local != set.files[cur].callback_params.end()) {
            params = &local->second;
        } else {
            auto global = callback_params.find(resolved);
            if (global != callback_params.end()) params = &global->second;
        }
        if (!params) return;
        for (size_t i = 0; i < params->size() && i < args.size(); i++) {
            const auto &ann = (*params)[i];
            if (ann && ann->rfind("fn", 0) == 0 && args[i]->k == EK::Str) {
                args[i]->a = resolve_call(args[i]->a, cur);
            }
        }
    }

    ExprP walk_expr(ExprP e, size_t cur) {
        if (!e) return e;
        if (e->k == EK::Call) {
            for (auto &k : e->kids) k = walk_expr(std::move(k), cur);
            const std::string original = e->a;
            e->a = resolve_call(e->a, cur);
            resolve_callback_args(e->kids, original, e->a, cur);
            return e;
        }
        if (e->k == EK::MCall) {
            auto path = name_path(e->kids[0].get());
            if (path) {
                const std::string original = *path + "." + e->b;
                auto name = resolve_qualified(original, cur);
                if (name) {
                    // `mod.f(x)` becomes `call mod.f [x]`: the receiver is the
                    // module qualifier, not a value.
                    e->k = EK::Call;
                    e->a = *name;
                    e->b.clear();
                    e->kids.erase(e->kids.begin());
                    for (auto &k : e->kids) k = walk_expr(std::move(k), cur);
                    resolve_callback_args(e->kids, original, e->a, cur);
                    return e;
                }
            }
        }
        for (auto &k : e->kids) k = walk_expr(std::move(k), cur);
        if ((e->k == EK::Closure || e->k == EK::ParFor) && e->fn) {
            for (auto &p : e->fn->params) {
                if (p.def) p.def = walk_expr(std::move(p.def), cur);
            }
            walk_body(e->fn->body, cur);
        }
        if (e->k == EK::ParFor && !e->b.empty()) {
            e->b = resolve_call(e->b, cur);
        }
        return e;
    }

    void walk_body(std::vector<StmtP> &body, size_t cur) {
        for (auto &s : body) walk_stmt(*s, cur);
    }

    void walk_stmt(Stmt &s, size_t cur) {
        s.e1 = walk_expr(std::move(s.e1), cur);
        s.e2 = walk_expr(std::move(s.e2), cur);
        s.e3 = walk_expr(std::move(s.e3), cur);
        for (auto &p : s.params) {
            if (p.def) p.def = walk_expr(std::move(p.def), cur);
        }
        for (auto &p : s.ext.params) {
            if (p.def) p.def = walk_expr(std::move(p.def), cur);
        }
        for (auto &f : s.fns) {
            for (auto &p : f.params) {
                if (p.def) p.def = walk_expr(std::move(p.def), cur);
            }
            walk_body(f.body, cur);
        }
        walk_body(s.body, cur);
        walk_body(s.body2, cur);
        if (s.els) walk_stmt(*s.els, cur);
    }

    void transform_top(Stmt &s, size_t cur) {
        walk_stmt(s, cur);
        Stmt *fn = nullptr;
        if (s.k == SK::Fn) fn = &s;
        if (s.k == SK::Pub && !s.body.empty() && s.body[0]->k == SK::Fn) fn = s.body[0].get();
        if (!fn) return;
        const std::string original = fn->fns[0].name.value_or("");
        auto it = set.files[cur].functions.find(original);
        if (it != set.files[cur].functions.end()) fn->fns[0].name = it->second;
    }


    void check_interface(size_t cur,
                         const std::vector<std::pair<std::string, std::pair<std::vector<std::string>,
                                                                        std::string>>> &arity) {
        const SourceFile &f = set.files[cur];
        if (!f.iface) return;
        std::string where = module_name(cur);
        auto find = [&](const std::string &name) -> const std::pair<std::vector<std::string>,
                                                                   std::string> * {
            for (const auto &kv : arity) {
                if (kv.first == name) return &kv.second;
            }
            return nullptr;
        };
        for (const auto &[name, want] : *f.iface) {
            const auto *got = find(name);
            const bool is_struct = want.second == "@struct" && name.rfind("struct:", 0) == 0;
            const std::string disp = is_struct ? name.substr(7) : name;
            if (!got) {
                throw ModuleError("The implementation of module `" + where +
                                  "` does not match its interface: `" + disp +
                                  "` is declared in the interface but not defined");
            }
            const std::vector<std::string> &params = got->first;
            const std::vector<std::string> &want_params = want.first;
            if (is_struct) {
                if (params != want_params) {
                    throw ModuleError("The implementation of module `" + where +
                                      "` does not match its interface: struct `" + disp +
                                      "` fields differ from the interface");
                }
                continue;
            }
            if (params.size() != want_params.size()) {
                throw ModuleError(
                    "The implementation of module `" + where +
                    "` does not match its interface: `" + name + "` takes " +
                    std::to_string(params.size()) + " parameter(s) in the implementation, " +
                    std::to_string(want_params.size()) + " in the interface");
            }
            for (size_t i = 0; i < params.size(); i++) {
                if (params[i] != want_params[i]) {
                    throw ModuleError("The implementation of module `" + where +
                                      "` does not match its interface: `" + name + "` parameter " +
                                      std::to_string(i + 1) + " is `" + want_params[i] +
                                      "` in the interface, `" + params[i] +
                                      "` in the implementation");
                }
            }
            if (got->second != want.second) {
                throw ModuleError("The implementation of module `" + where +
                                  "` does not match its interface: `" + name + "` returns `" +
                                  want.second + "` in the interface, `" + got->second +
                                  "` in the implementation");
            }
        }
    }

    void resolve_uses(size_t cur) {
        for (const auto &sp : set.files[cur].body) {
            if (sp->k != SK::PubUse) continue;
            const std::string &target_name = sp->a;
            size_t dot = target_name.rfind('.');
            std::string qualifier = dot == std::string::npos ? "" : target_name.substr(0, dot);
            std::string fn = dot == std::string::npos ? target_name : target_name.substr(dot + 1);
            std::vector<size_t> targets;
            for (const auto &[imported, target] : set.files[cur].imports) {
                if (!qualifier.empty()) {
                    size_t tail = imported.rfind('.');
                    std::string last = tail == std::string::npos ? imported
                                                                 : imported.substr(tail + 1);
                    if (imported == qualifier || last == qualifier) {
                        if (std::find(targets.begin(), targets.end(), target) == targets.end())
                            targets.push_back(target);
                    }
                }
            }
            if (targets.empty()) {
                throw ModuleError("`pub use " + target_name + "`: module `" + qualifier +
                                  "` is not imported");
            }
            if (targets.size() > 1) {
                throw ModuleError("`pub use " + target_name + "`: module `" + qualifier +
                                  "` is ambiguous");
            }
            size_t target = targets[0];
            auto it = set.files[target].functions.find(fn);
            if (it == set.files[target].functions.end()) {
                throw ModuleError("module `" + qualifier + "` has no function `" + fn + "`");
            }
            if (target != cur && !set.files[target].pub.count(fn)) {
                throw ModuleError("function `" + fn + "` is private in module `" +
                                  module_name(target) + "`");
            }
            auto source = std::make_pair(module_name(target), fn);
            auto known = set.files[cur].reexports.find(fn);
            if (known != set.files[cur].reexports.end() && known->second != source) {
                throw ModuleError("`" + fn + "` is re-exported from two modules: `" +
                                  known->second.first + "` and `" + source.first + "`");
            }
            set.files[cur].reexports[fn] = source;
            set.files[cur].functions[fn] = it->second;
            set.files[cur].pub.insert(fn);
            public_names[fn].emplace_back(cur, it->second);
        }
    }

    // A module's re-exports may name another module's re-exports, so a module is
    // resolved after the ones it imports: `facade -> mid -> base` is three levels
    // and one pass in load order reached `mid` too late.
    std::set<size_t> resolved;
    void resolve_module(size_t cur, std::vector<size_t> stack) {
        if (resolved.count(cur)) return;
        resolved.insert(cur);
        for (size_t s : stack)
            if (s == cur) return;   // an import cycle of re-exports
        stack.push_back(cur);
        for (const auto &kv : set.files[cur].imports) resolve_module(kv.second, stack);
        resolve_uses(cur);
    }

    void run() {
        for (size_t i = 0; i < set.files.size(); i++) {
            SourceFile &f = set.files[i];
            if (f.path.size() <= 5 || f.path.compare(f.path.size() - 5, 5, ".brat") != 0) continue;
            std::string bri = f.path.substr(0, f.path.size() - 5) + ".bri";
            std::error_code ec;
            if (fs::is_regular_file(bri, ec)) f.iface = parse_interface(bri);
        }
        for (size_t i = 0; i < set.files.size(); i++) {
            std::vector<std::pair<std::string, std::pair<std::vector<std::string>, std::string>>>
                arity;
            for (const auto &sp : set.files[i].body) {
                const Stmt *node = inner(*sp);
                if (node->k == SK::Extern) {
                    std::vector<std::optional<std::string>> params;
                    for (const Param &p : node->ext.params) params.push_back(p.ann);
                    set.files[i].callback_params[node->ext.name] = params;
                    callback_params[node->ext.name] = std::move(params);
                    continue;
                }
                if (node->k == SK::Struct) {
                    std::vector<std::string> fspecs;
                    for (const auto &fld : node->fields)
                        fspecs.push_back(fld.name + ": " + fld.ann.value_or("none"));
                    arity.emplace_back("struct:" + node->a,
                                       std::make_pair(std::move(fspecs), std::string("@struct")));
                    continue;
                }
                if (node->k != SK::Fn) continue;
                const FnDecl &fn = node->fns[0];
                const std::string original = fn.name.value_or("");
                const std::string full = mangle(i, original);
                set.files[i].functions[original] = full;
                std::vector<std::optional<std::string>> params;
                for (const Param &p : fn.params) params.push_back(p.ann);
                set.files[i].callback_params[original] = params;
                set.files[i].callback_params[full] = params;
                callback_params[full] = std::move(params);
                std::vector<std::string> anns;
                for (const Param &p : fn.params) anns.push_back(param_ann(p));
                arity.emplace_back(original, std::make_pair(anns, fn_ret(fn)));
                bool exported = set.files[i].iface ? set.files[i].iface->count(original) > 0
                                                   : sp->k == SK::Pub;
                if (exported) {
                    set.files[i].pub.insert(original);
                    public_names[original].emplace_back(i, full);
                } else if (set.files[i].ns) {
                    private_names[full] = {i, original};
                }
            }
            check_interface(i, arity);
        }
        for (size_t i = 0; i < set.files.size(); i++) resolve_module(i, {});
        for (size_t i = 0; i < set.files.size(); i++) {
            for (auto &sp : set.files[i].body) transform_top(*sp, i);
        }
    }
};

}  // namespace

ModuleInterface read_interface(const std::string &path) {
    return parse_interface(fs::path(path));
}

void set_stdlib_root(const std::string &dir) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(dir), ec);
    if (!ec) g_stdlib_override = p;
}

ModuleSet load_modules(const std::string &path) {
    fs::path abs = fs::weakly_canonical(fs::absolute(path));
    Loader l;
    l.base = abs.parent_path();
    if (l.base.empty()) l.base = ".";
    l.std_base = stdlib_root();
    l.vendor_base = vendor_root(l.base);
    l.visit(abs, true, "");
    return std::move(l.set);
}

ModuleSet load_modules(const std::string &path, const std::string &root_source) {
    fs::path abs = fs::weakly_canonical(fs::absolute(path));
    Loader l;
    l.base = abs.parent_path();
    if (l.base.empty()) l.base = ".";
    l.std_base = stdlib_root();
    l.vendor_base = vendor_root(l.base);
    l.visit(abs, true, "", &root_source);
    return std::move(l.set);
}

void mangle_modules(ModuleSet &set) {
    if (set.mangled) return;
    Mangler m{set};
    m.run();
    set.mangled = true;
}

static const std::string *prunable_import_fn(const Stmt &s) {
    const Stmt *fn = nullptr;
    if (s.k == SK::Fn) fn = &s;
    else if (s.k == SK::Pub && !s.body.empty() && s.body[0] && s.body[0]->k == SK::Fn)
        fn = s.body[0].get();
    if (!fn || fn->fns.empty() || !fn->fns[0].name || fn->fns[0].name->empty()) return nullptr;
    if (fn->fns[0].name->rfind("__test__", 0) == 0) return nullptr;
    return &(*fn->fns[0].name);
}

static std::set<std::string> live_imported_functions(const ModuleSet &set) {
    std::map<std::string, std::vector<const Stmt *>> cands;
    std::vector<const Stmt *> queue;
    for (size_t idx = 0; idx < set.files.size(); ++idx) {
        for (const auto &sp : set.files[idx].body) {
            if (!sp || sp->k == SK::Import || sp->k == SK::PubUse) continue;
            if (idx > 0) {
                if (const std::string *nm = prunable_import_fn(*sp)) {
                    cands[*nm].push_back(sp.get());
                    continue;
                }
            }
            queue.push_back(sp.get());
        }
    }
    std::set<std::string> live;
    if (cands.empty()) return live;

    auto mark = [&](const std::string &name) {
        if (name.empty()) return;
        auto it = cands.find(name);
        if (it == cands.end()) return;
        if (!live.insert(name).second) return;
        for (const Stmt *st : it->second) queue.push_back(st);
    };

    struct Scan {
        decltype(mark) &mark_fn;
        void expr(const Expr *e) {
            if (!e) return;
            if (e->k == EK::Call || e->k == EK::Var || e->k == EK::Str || e->k == EK::RStr)
                mark_fn(e->a);
            if (e->k == EK::ParFor && !e->b.empty())
                mark_fn(e->b);
            for (const auto &k : e->kids) expr(k.get());
            if (e->fn) {
                for (const auto &p : e->fn->params) expr(p.def.get());
                for (const auto &st : e->fn->body) stmt(st.get());
            }
        }
        void stmt(const Stmt *s) {
            if (!s) return;
            expr(s->e1.get());
            expr(s->e2.get());
            expr(s->e3.get());
            for (const auto &p : s->params) expr(p.def.get());
            for (const auto &p : s->ext.params) expr(p.def.get());
            for (const auto &f : s->fns) {
                for (const auto &p : f.params) expr(p.def.get());
                for (const auto &st : f.body) stmt(st.get());
            }
            for (const auto &st : s->body) stmt(st.get());
            for (const auto &st : s->body2) stmt(st.get());
            if (s->els) stmt(s->els.get());
        }
    } scan{mark};

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        scan.stmt(queue[qi]);
    }
    return live;
}

void flatten(ModuleSet &set, bool prune_imports) {
    mangle_modules(set);
    const bool do_prune = prune_imports && set.files.size() > 1;
    const std::set<std::string> live_fns = do_prune ? live_imported_functions(set)
                                                    : std::set<std::string>{};
    std::set<std::string> expanded;
    std::vector<StmtP> flat;
    // A module's statements land where its `import` sits in the importer, not
    // at the end: the reference splices depth first, each module once.
    struct Walk {
        ModuleSet &set;
        std::set<std::string> &expanded;
        std::vector<StmtP> &out;
        bool do_prune;
        const std::set<std::string> &live_fns;

        void go(size_t idx) {
            const std::string &path = set.files[idx].path;
            if (expanded.count(path)) return;
            expanded.insert(path);
            for (auto &sp : set.files[idx].body) {
                if (sp->k == SK::Import) {
                    for (const auto &[name, target] : set.files[idx].imports) {
                        if (name == sp->a) {
                            go(target);
                            break;
                        }
                    }
                    continue;
                }
                if (sp->k == SK::PubUse) continue;
                if (do_prune && idx > 0) {
                    if (const std::string *nm = prunable_import_fn(*sp)) {
                        if (!live_fns.count(*nm)) continue;
                    }
                }
                out.push_back(std::move(sp));
            }
        }
    } walk{set, expanded, flat, do_prune, live_fns};
    walk.go(0);
    set.flat = std::move(flat);
}
