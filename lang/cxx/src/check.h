// Gradual static type checker (`i64`, `f64`, `bool`, `str`, `vec[T]`, `map[K,V]`, structs, traits).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ast.h"

// A type is a small tagged value. Payloads: a callback keeps its arity and the
// signature as written, ptr/slice/vec keep an element, a struct keeps its fields
// in declaration order (the order shows up in diagnostics).
struct TyField;

struct Ty {
    enum K : uint8_t {
        Dyn, Int, Float, Str, Bool, None, Tag, Map, Cb, Ptr, Slice, Vec, Struct,
        Option, Result, Enum, Other
    };
    K k = Dyn;
    int arity = 0;
    bool is_union = false;
    std::string text;                             // callback signature, `Enum`, or `Other`
    std::shared_ptr<const Ty> inner;              // ptr/slice/vec/option/result-ok element
    std::shared_ptr<const Ty> inner2;             // result-err element
    std::vector<TyField> fields;

    // Defined below `TyField`: a `Ty` cannot be built before its field type is complete.
    static Ty dyn();
    static Ty of(K k);
    static Ty wrap(K k, Ty in);
    static Ty option(Ty in);
    static Ty result(Ty ok, Ty err);
    static Ty enm(std::string name);
    static Ty cb(int arity, std::string text);
    static Ty strct(std::vector<TyField> &&f);
    static Ty other(std::string name);

    bool struct_has(const std::string &f, Ty *out = nullptr) const;
    bool operator==(const Ty &o) const;
    bool operator!=(const Ty &o) const { return !(*this == o); }
};

// A struct member. Not a `std::pair<std::string, Ty>`: that pair cannot be complete inside `Ty` itself, which
// libstdc++ tolerates under GCC and rejects under clang.
struct TyField {
    std::string name;
    Ty type;
};

inline Ty Ty::dyn() { return Ty{}; }
inline Ty Ty::of(K k) { Ty t; t.k = k; return t; }
inline Ty Ty::wrap(K k, Ty in) {
    Ty t;
    t.k = k;
    t.inner = std::make_shared<const Ty>(std::move(in));
    return t;
}
inline Ty Ty::option(Ty in) {
    Ty t;
    t.k = Option;
    t.inner = std::make_shared<const Ty>(std::move(in));
    return t;
}
inline Ty Ty::result(Ty ok, Ty err) {
    Ty t;
    t.k = Result;
    t.inner = std::make_shared<const Ty>(std::move(ok));
    t.inner2 = std::make_shared<const Ty>(std::move(err));
    return t;
}
inline Ty Ty::enm(std::string name) {
    Ty t;
    t.k = Enum;
    t.text = std::move(name);
    return t;
}
inline Ty Ty::cb(int arity, std::string text) {
    Ty t;
    t.k = Cb;
    t.arity = arity;
    t.text = std::move(text);
    return t;
}
inline Ty Ty::strct(std::vector<TyField> &&f) {
    Ty t;
    t.k = Struct;
    t.fields = std::move(f);
    return t;
}
inline Ty Ty::other(std::string name) {
    Ty t;
    t.k = Other;
    t.text = std::move(name);
    return t;
}

std::string type_name(const Ty &t);
bool type_ok(const Ty &want, const Ty &got);
bool numeric(const Ty &t);

struct TypeErr {
    int line = 0;
    std::string msg;
};

struct SinkEdge {
    std::string callee;
    size_t arg_idx = 0;
    std::set<size_t> caller_params;
};

struct FnSig {
    std::vector<Ty> params;
    std::vector<std::optional<std::string>> param_anns;
    Ty ret;
    std::optional<std::string> ret_ann;
    int ndef = 0;
    std::optional<Ty> varargs;
    std::string varargs_ann;   // as written: `..ptr` may take a `str`, `..i64` may not
    // A C function from `extern`: its `ptr[T]` slots are where `&x` may be
    // written, and nowhere else.
    bool extern_fn = false;
    bool is_ctor = false;
    // A `raise` in the body leaves the fn as an `Err` the caller has to look at.
    bool raises = false;
    std::set<size_t> mut_params;
    std::set<size_t> sink_params;
    std::vector<SinkEdge> sink_edges;
};

using Sigs = std::map<std::string, FnSig>;

// Every function an `import` can reach: the file itself plus what it imports.
void collect_sigs(const std::vector<StmtP> &prog, Sigs &sigs);
// A `raise` that leaves the fn: one inside a `try` body is caught there.
bool body_raises(const std::vector<StmtP> &body);
// The expression statements a `try` body runs itself: those of a closure, a
// nested fn or a `spawn` in it are not among them.
void try_stmts(const std::vector<StmtP> &body, bool in_try, std::vector<Stmt *> &out);
void collect_exports(const std::vector<StmtP> &prog,
                     std::map<std::string, Ty> &consts,
                     std::map<std::string, std::map<std::string, Ty>> &structs);

// The ABI spelling of one annotation, and a parsed `fn(...) -> T` signature.
//
// The emitter uses these for the same reason the typer does: a callback slot
// must be checked against the same rule in both, or `cc` writes a `brat_cb_x`
// call for a function that does not exist and hands the user C that does not
// compile.
std::string normalize_abi_type(const std::optional<std::string> &text);
std::optional<std::pair<std::vector<std::string>, std::string>>
callback_signature(const std::string &text);

// Diagnostics in emission order with duplicates suppressed.
std::vector<TypeErr> type_check(const std::vector<StmtP> &prog, const Sigs &extra,
                                const std::map<std::string, Ty> &extra_consts = {},
                                const std::map<std::string, std::map<std::string, Ty>> &extra_structs = {});
