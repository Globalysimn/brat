#include "lexer.h"

#include <algorithm>

#include "parser.h"

bool is_keyword(std::string_view s) {
    static const std::string_view words[] = {
        "let", "mut", "fn", "if", "else", "while", "return", "true", "false",
        "or", "try", "catch", "spawn", "match", "for", "in", "import", "defer",
        "move", "break", "continue", "impl", "self", "trait", "enum", "const",
        "pub", "extern", "unsafe", "export", "use",
    };
    return std::find(std::begin(words), std::end(words), s) != std::end(words);
}

namespace {

// Two-byte operators, longest match first: `..` beats `.`, `=>` beats `=`.
const std::string_view PAIRS[] = {"..", "=>", "->", "==", "!=", "<=", ">=",
                                  "&&", "||", "+=", "-=", "*=", "/=", "%="};

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }

// Index of the quote that closes the literal opened at `start`.
//
// A `{...}` placeholder may hold quotes, so the scan tracks how deep it is
// inside placeholders and inside their parentheses. A quote closes the literal
// at depth 0, or at depth > 0 with no open `(` — that is what `"{"` looks like:
// a JSON template whose braces are meant literally.
size_t str_end(std::string_view s, size_t start) {
    int depth = 0, paren = 0;
    size_t j = start + 1;
    while (j < s.size()) {
        char c = s[j];
        if (c == '\\' && j + 1 < s.size()) {
            j += 2;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}' && depth > 0) {
            depth--;
        } else if (c == '(') {
            paren++;
        } else if (c == ')' && paren > 0) {
            paren--;
        } else if (c == '"' && (depth == 0 || paren == 0)) {
            return j;
        }
        j++;
    }
    return std::string_view::npos;
}

}  // namespace

namespace {

bool seq_ok(std::string_view s, size_t i, size_t n, uint32_t min_cp) {
    if (i + n > s.size()) return false;
    uint32_t cp = static_cast<unsigned char>(s[i]) & (0xFFu >> (n + 1));
    for (size_t k = 1; k < n; k++) {
        unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min_cp) return false;                      // overlong form
    if (cp > 0x10FFFF) return false;
    return !(cp >= 0xD800 && cp <= 0xDFFF);             // a lone surrogate
}

}  // namespace

// Source is UTF-8: the Python front end decodes it with `encoding="utf-8"`, so a
// byte its decoder rejects is a file neither front end can read. Rejecting it
// here -- before the first token, with the line the bad byte sits on -- keeps
// `check`, `cc` and `run` on one answer for one file instead of one of them
// silently compiling garbage.
bool utf8_valid(std::string_view s, uint32_t &bad_line) {
    uint32_t line = 1;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c == '\n') line++;
            i++;
            continue;
        }
        bool ok = false;
        if (c >= 0xC2 && c <= 0xDF) ok = seq_ok(s, i, 2, 0x80);
        else if (c >= 0xE0 && c <= 0xEF) ok = seq_ok(s, i, 3, 0x800);
        else if (c >= 0xF0 && c <= 0xF4) ok = seq_ok(s, i, 4, 0x10000);
        if (!ok) {
            bad_line = line;
            return false;
        }
        i += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    }
    return true;
}

std::vector<Tok> lex(std::string_view src) {
    uint32_t bad_line = 1;
    if (!utf8_valid(src, bad_line)) throw ParseError("source is not valid UTF-8", bad_line);
    std::vector<Tok> out;
    out.reserve(src.size() / 4 + 8);
    size_t i = 0;
    uint32_t line = 1;

    while (i < src.size()) {
        unsigned char c = static_cast<unsigned char>(src[i]);

        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') { i++; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') i++;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            bool closed = false;
            while (j < src.size()) {
                if (src[j] == '\\' && j + 1 < src.size()) {
                    j += 2;
                } else if (src[j] == '"') {
                    closed = true;
                    j++;
                    break;
                } else {
                    j++;
                }
            }
            if (!closed) {   // an unterminated literal is not a string
                i++;
                continue;
            }
            size_t end = j;
            if (src.substr(i, j - i).find('{') != std::string_view::npos) {
                size_t real = str_end(src, i);
                end = (real == std::string_view::npos) ? src.size() : real + 1;
            }
            std::string_view text = src.substr(i, end - i);
            out.push_back({TK::Str, text, line});
            line += static_cast<uint32_t>(std::count(text.begin(), text.end(), '\n'));
            i = end;
            continue;
        }
        if (is_digit(c)) {
            // `_` between two digits is a separator (`1_000`, `1_0.5`), the same
            // rule the Python lexer reads. A `_` that is not between digits ends
            // the number, so `1_` and `1__0` lex as a number and a name.
            size_t start = i;
            auto digits = [&]() {
                while (i < src.size()) {
                    if (is_digit(static_cast<unsigned char>(src[i]))) { i++; continue; }
                    if (src[i] == '_' && i + 1 < src.size() &&
                        is_digit(static_cast<unsigned char>(src[i + 1]))) {
                        i++;
                        continue;
                    }
                    break;
                }
            };
            digits();
            if (i + 1 < src.size() && src[i] == '.' && is_digit(static_cast<unsigned char>(src[i + 1]))) {
                i++;
                digits();
            }
            if (i < src.size() && (src[i] | 0x20) == 'e') {
                size_t k = i + 1;
                if (k < src.size() && (src[k] == '+' || src[k] == '-')) k++;
                if (k < src.size() && is_digit(static_cast<unsigned char>(src[k]))) {
                    size_t save = i;
                    i = k;
                    digits();
                    if (i == k) i = save;   /* nothing consumed: leave `e` alone */
                }
            }
            out.push_back({TK::Num, src.substr(start, i - start), line});
            continue;
        }
        if (is_alpha(c)) {
            size_t start = i;
            while (i < src.size() && is_alnum(static_cast<unsigned char>(src[i]))) i++;
            std::string_view text = src.substr(start, i - start);
            out.push_back({is_keyword(text) ? TK::Kw : TK::Id, text, line});
            continue;
        }

        bool paired = false;
        if (i + 1 < src.size()) {
            std::string_view two = src.substr(i, 2);
            for (std::string_view p : PAIRS) {
                if (p == two) {
                    out.push_back({TK::Op, two, line});
                    i += 2;
                    paired = true;
                    break;
                }
            }
        }
        if (paired) continue;
        if (std::string_view("+-*/%<>=!&{}()[],:;?.").find(src[i]) != std::string_view::npos) {
            out.push_back({TK::Op, src.substr(i, 1), line});
        }
        i++;
    }

    // The EOF token names the line of the last real token, not the line after
    // the trailing newline: `lang/engine/diag.py` reports "unexpected end of
    // file" against `c.t[-1].line`, and a one-line difference here shows up as
    // a one-line disagreement between the two front ends.
    out.push_back({TK::Eof, {}, out.empty() ? 1u : out.back().line});
    return out;
}
