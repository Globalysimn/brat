// Diagnostic code catalog and message classifier shared by CLI and LSP.
#pragma once

#include <string>
#include <string_view>

struct ExplainEntry {
    const char *code;
    const char *title;
    const char *what;
    const char *fix;
};

inline constexpr ExplainEntry kExplainTable[] = {
    {"B0000", "other error", "Unclassified diagnostic.", "See the message text above."},
    {"B0101", "use of undeclared var",
     "Name not declared in this scope (no `let`, parameter, `for`/`match` binding).",
     "Add `let <name> = ...` or import the name from another file."},
    {"B0521", "ref escapes via spawn",
     "A borrow captured by `spawn`; the frame is copied, the borrow hangs on dead storage: data race.",
     "Pass only Copy values or explicitly `move` into `spawn`; do not capture a shared non-Copy binding."},
    {"B0522", "data race between threads",
     "One value is shared with a running `spawn` and both sides touch it while both run; exactly one side may write until they join.",
     "Call `join_all()` (or `join(handle)`) before the second access, or pass the value through a channel with `send`/`recv`."},
    {"B0530", "plugin value used after close",
     "A symbol or a pointer made through a library handle belongs to that library; `lib_close` ends it, and the memory or the code behind it may already be gone.",
     "Move the `lib_close` after the last use, or open the library again before using the value."},
    {"B0531", "plugin signature mismatch",
     "The literal signature in `call` disagrees with the `extern fn` declaration of the same name, so the call would read its arguments through the wrong prototype.",
     "Make the string match the declaration, or let `brat bindgen` write it from the header."},
    {"E0382", "use of moved value",
     "Value was moved (`move`); the source location can no longer be used. Move is linear: one ownership.",
     "Use the variable the value was moved into, or copy it beforehand (Copy types only)."},
    {"E0499", "double mut borrow",
     "One location cannot have two mutable borrows at once (law of exclusivity).",
     "Keep one writer; write through the owner, not an alias copy."},
    {"E0502", "cannot mutate while iterating",
     "A `for` holds a view of the container it walks; writing into that container inside the loop changes the thing being read.",
     "Collect the writes in a second container and apply them after the loop, or iterate a copy (`for x in v.copy()`)."},
    {"E0505", "cannot move while borrowed",
     "Location is held by a live borrow; cannot move until the borrow ends.",
     "Wait for the last use of the borrower, then move."},
    {"E0507", "cannot move out of borrowed parameter",
     "A non-Copy parameter is a view of the caller's storage; moving it would transfer data the function does not own.",
     "Declare the parameter `move` to take ownership before consuming it."},
    {"E0515", "ref escapes via return",
     "A borrow of local storage escapes the function via `return`; the storage dies with the frame.",
     "Return a copy/value, not a borrow; borrowing a parameter is fine: its storage belongs to the caller."},
    {"E0596", "assign to immutable",
     "Assignment to an immutable binding or through an immutable receiver.",
     "Declare `let mut ...` (or a `mut ...` parameter) to write."},
    {"F0001", "unsupported FFI type",
     "A slot of an `extern` declaration names a type the C emitter cannot spell, so a call would read or write memory through a prototype that does not exist.",
     "Use i8..u64, usize, f32, f64, str, ptr, ptr[T], fn(...) or an `extern struct`; `brat bindgen` writes the BRAT name for a header."},
    {"P0001", "syntax error",
     "The file does not parse; nothing after this point was checked.",
     "Fix the syntax first: everything else is reported after it parses."},
    {"T0001", "type error",
     "A value's type does not match what the position requires.",
     "Check the annotation and the expression: an i64 position does not take a str, and a `?` needs a Result."},
    {"W0001", "unused variable",
     "The binding is never read. Not an error; the build goes on.",
     "Remove the binding, or prefix work with `_` if it is intentional."},
    {"W0002", "unused mut",
     "`mut` is declared and never written.",
     "Drop `mut`: an immutable binding is enough here."},
    {"W0003", "unused import",
     "The module is imported and nothing from it is used.",
     "Remove the `import`, or use what it exports."},
    {"W0004", "string placeholder",
     "A `{...}` placeholder looks cut short by a bare quote.",
     "Bind the value first, then interpolate the binding."},
};

inline const ExplainEntry *find_explain_entry(std::string_view code) {
    for (const auto &e : kExplainTable) {
        if (code == e.code) return &e;
    }
    return nullptr;
}

inline std::string classify_diag_code(std::string_view m) {
    const auto has = [&](std::string_view sub) { return m.find(sub) != std::string_view::npos; };
    if (has("use of moved")) return "E0382";
    if (has("cannot move borrowed parameter")) return "E0507";
    if (has("while borrowed") || has("cannot move borrowed pointer") ||
        has("cannot pass") || has("cannot use") || has("cannot assign"))
        return "E0505";
    if (has("while iterating it")) return "E0502";
    if (has("double mut borrow") || has("law of exclusivity")) return "E0499";
    if (has("escapes via return")) return "E0515";
    if (has("escapes via spawn") || has("return escapes spawn")) return "B0521";
    if (has("inside `spawn`") || has("spawn` launched in a loop")) return "B0522";
    if (has("is from library") || has("is closed twice") || has("is closed on line"))
        return "B0530";
    if (has("the signature says") || has("the signature has")) return "B0531";
    if (has("assign to immutable") || has("cannot mutate immutable") ||
        has("pass immutable") || has("assign to const") || has("write through"))
        return "E0596";
    if (has("unsupported FFI type")) return "F0001";
    if (has("use of undeclared var")) return "B0101";
    if (has("unused var")) return "W0001";
    if (has("unused mut")) return "W0002";
    if (has("unused import")) return "W0003";
    if (has("looks cut short")) return "W0004";
    return "B0000";
}
