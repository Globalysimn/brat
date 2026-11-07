// The tree, and nothing else: no visitor, no RTTI, one allocation per node.
//
// Every node kind is an enumerator of its own enum, so `switch` in a pass is
// exhaustive: with -Werror=switch a forgotten kind is a compile error, which is
// the one property worth having from a sum type. The payload is a fixed set of
// slots instead of a union: a node is 300-odd bytes, which keeps the whole tree
// of a 68 KB program in a few megabytes and the allocator out of the hot path.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class Mode : uint8_t { Ref, Mut, Move };

const char *mode_str(Mode m);
std::optional<Mode> mode_of(const std::string &s);

enum class EK : uint8_t {
    Num, Str, RStr, Bool, None, Var, Bin, Un, Call, MCall, Field, Idx, VecLit,
    MapLit, StructLit, Closure, Quest, Move, Or, ParFor,
};

enum class SK : uint8_t {
    Let, LetTmp, Set, SetIdx, SetField, Expr, Block, If, While, ForRange, ForIter,
    Return, Break, Continue, Spawn, Scope, Unsafe, Defer, Try, Fn, ExportFn, Impl,
    Trait, Enum, Struct, Pub, PubUse, Import, Extern, ExternStruct,
};

enum class PK : uint8_t { Wild, None, Lit, Bind, Variant, Struct };

struct Expr;
struct Stmt;
struct Pat;
struct FnDecl;

using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;
using PatP = std::unique_ptr<Pat>;

struct Param {
    std::string name;
    Mode mode = Mode::Ref;
    std::optional<std::string> ann;
    ExprP def;
};

struct SigParam {
    std::string name;
    Mode mode = Mode::Ref;
    std::optional<std::string> ann;
};

struct Sig {
    std::string name;
    std::vector<SigParam> params;
    std::optional<std::string> ret;
};

struct FnDecl {
    std::optional<std::string> name;   // empty for an anonymous closure
    std::vector<Param> params;
    std::vector<StmtP> body;
    std::vector<std::string> tparams;
    std::optional<std::string> ret;
};

struct Variant {
    std::string name;
    std::vector<std::string> fields;
    std::vector<std::optional<std::string>> field_anns;
};

struct StructField {
    std::string name;
    std::optional<std::string> ann;
};

struct ExternDecl {
    std::optional<std::string> lib;
    std::string name;
    std::vector<Param> params;
    std::optional<std::string> ret;
    std::optional<std::string> varargs;
    bool owned = false;
    bool checked = false;
    // `pure`, `nothrow`, `reads:<p>`, `writes:<p>` in source order: the only
    // promise the language can make about a C function. Declared via `@contract`
    // EFFECTS for why each of them is checked where it is checked.
    std::vector<std::string> effects;
    // `thread:any`, `reenter:no` in source order: what the callee may do with
    // the callback it is handed. Written on the callback parameter, stored on
    // the declaration -- collected before the `extern fn` node.
    std::vector<std::string> contracts;
};

struct Expr {
    EK k;
    int64_t i = 0;
    double f = 0;
    bool is_float = false;
    bool flag = false;                 // Bool value
    std::string a;                     // name / operator / tag / field
    std::string b;                     // method name
    std::vector<ExprP> kids;           // operands, args, items, flat pairs
    std::vector<std::string> names;    // struct field names
    std::unique_ptr<FnDecl> fn;        // Closure
};

struct Stmt {
    SK k;
    int line = 0;                      // first token of the statement; 0 = unknown
    std::string a;                     // binding / loop var / field / path / type
    std::optional<std::string> ann;
    bool mut = false;
    bool konst = false;
    bool from_match = false;
    bool pat_uncond = false;
    int pat_arity = -1;
    ExprP e1, e2, e3;
    std::vector<StmtP> body, body2;
    StmtP els;
    std::vector<Param> params;
    std::vector<std::string> names;
    std::vector<Variant> variants;
    std::vector<FnDecl> fns;
    std::vector<Sig> sigs;
    std::vector<StructField> fields;
    std::optional<std::string> opt1;   // trait name, error binding, use path
    ExternDecl ext;
};

struct Pat {
    PK k;
    std::string a;                     // variant/struct tag or binding name
    ExprP lit;
    std::vector<PatP> subs;
    std::vector<std::string> names;    // struct/map pattern field names
    bool mut = false;
    bool open = false;
};

// Constructors: the only place that knows how a node is filled.
ExprP mk_num(int64_t v);
ExprP mk_float(double v);
ExprP mk_str(std::string v);
ExprP mk_rstr(std::string v);
ExprP mk_bool(bool v);
ExprP mk_none();
ExprP mk_var(std::string v);
ExprP mk_bin(const char *op, ExprP l, ExprP r);
ExprP mk_un(const char *op, ExprP x);
ExprP mk_call(std::string name, std::vector<ExprP> args);
ExprP mk_mcall(ExprP obj, std::string meth, std::vector<ExprP> args);
ExprP mk_field(ExprP obj, std::string name);
ExprP mk_idx(ExprP obj, ExprP ix);
ExprP mk_veclit(std::vector<ExprP> items);
ExprP mk_quest(ExprP x);
ExprP mk_move(ExprP x);
ExprP mk_or(ExprP l, ExprP r);

StmtP mk_let(std::string name, ExprP init, bool mut, bool konst,
             std::optional<std::string> ann);
StmtP mk_let_tmp(std::string name, ExprP init, bool mut);
StmtP mk_expr(ExprP e);
StmtP mk_block(std::vector<StmtP> v);
StmtP mk_if(ExprP cond, std::vector<StmtP> then_body, std::optional<StmtP> els);
StmtP mk_while(ExprP cond, std::vector<StmtP> body);

// Printing: the s-expression the old implementation produces, byte for byte.
std::string dump(const std::vector<StmtP> &prog);
void print_expr(const Expr &e, std::string &out);
void print_stmt(const Stmt &s, std::string &out);
std::string pat_text(const Pat &p);
std::string repr_f64(double v);
