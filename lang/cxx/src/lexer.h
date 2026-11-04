// One pass, no allocation, every token a slice of the source.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class TK : uint8_t { Id, Kw, Num, Str, Op, Eof };

struct Tok {
    TK kind;
    std::string_view text;
    uint32_t line;
};

bool is_keyword(std::string_view s);
std::vector<Tok> lex(std::string_view src, uint32_t start_line = 1);

// Whole source is valid UTF-8; `bad_line` receives the line of the first offending
// byte when it is not.
bool utf8_valid(std::string_view s, uint32_t &bad_line, uint32_t start_line = 1);
