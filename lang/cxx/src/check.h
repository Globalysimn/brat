// The type pass: node for node the same walk as the Python typer, because the
// contract is not "roughly the same errors" but the same bytes on the corpus.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"

// A type is a small tagged value. Payloads: a callback keeps its arity and the
// signature as written, ptr/slice/vec keep an element, a struct keeps its fields
// in declaration order (the order shows up in diagnostics).
struct Ty {
    enum K : uint8_t {
        Dyn, Int, Float, Str, Bool, None, Tag, Map, Cb, Ptr, Slice, Vec, Struct, Other
    };
    K k = Dyn;
    int arity = 0;
    std::string text;                             // callback signature, or `Other`
    std::shared_ptr<const Ty> inner;              // ptr/slice/vec element
    std::vector<std::pair<std::string, Ty>> fields;

    static Ty dyn() { return Ty{}; }
    static Ty of(K k) { Ty t; t.k = k; return t; }
    static Ty wrap(K k, Ty in) {
        Ty t;
        t.k = k;
        t.inner = std::make_shared<const Ty>(std::move(in));
        return t;
    }
    static Ty cb(int arity, std::string text) {
        Ty t;
        t.k = Cb;
        t.arity = arity;
        t.text = std::move(text);
        return t;
    }
    static Ty strct(std::vector<std::pair<std::string, Ty>> f) {
        Ty t;
        t.k = Struct;
        t.fields = std::move(f);
        return t;
    }
    static Ty other(std::string name) {
        Ty t;
        t.k = Other;
        t.text = std::move(name);
        return t;
    }

    bool struct_has(const std::string &f, Ty *out = nullptr) const;
    bool operator==(const Ty &o) const;
    bool operator!=(const Ty &o) const { return !(*this == o); }
};

std::string type_name(const Ty &t);
bool type_ok(const Ty &want, const Ty &got);
bool numeric(const Ty &t);

struct TypeErr {
    int line = 0;
    std::string msg;
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
};

using Sigs = std::map<std::string, FnSig>;

// Every function an `import` can reach: the file itself plus what it imports.
void collect_sigs(const std::vector<StmtP> &prog, Sigs &sigs);

// The ABI spelling of one annotation, and a parsed `fn(...) -> T` signature.
//
// The emitter uses these for the same reason the typer does: a callback slot
// must be checked against the same rule in both, or `cc` writes a `brat_cb_x`
// call for a function that does not exist and hands the user C that does not
// compile.
std::string normalize_abi_type(const std::optional<std::string> &text);
std::optional<std::pair<std::vector<std::string>, std::string>>
callback_signature(const std::string &text);

// Diagnostics in emission order, duplicates dropped like the reference does.
std::vector<TypeErr> type_check(const std::vector<StmtP> &prog, const Sigs &extra);
