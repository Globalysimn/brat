#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace {

void utf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// A JSON document is a tree the *sender* decides the shape of: `[[[[…` a few
// thousand deep is a SIGSEGV, not an exception, and a stack overflow cannot be
// caught. Every frame here is about 100 bytes, so 200 levels is far more than any
// real protocol message needs and far less than the smallest stack we run on.
constexpr int kMaxDepth = 200;

class Parser {
public:
    explicit Parser(const std::string &text) : text_(text) {}

    Json parse() {
        Json value = read_value();
        whitespace();
        if (at_ != text_.size()) fail();
        return value;
    }

private:
    const std::string &text_;
    size_t at_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail() const { throw std::runtime_error("invalid JSON"); }

    void whitespace() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' ||
                text_[at_] == '\n')) ++at_;
    }

    bool take(char c) {
        whitespace();
        if (at_ == text_.size() || text_[at_] != c) return false;
        ++at_;
        return true;
    }

    uint32_t hex4() {
        if (text_.size() - at_ < 4) fail();
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[at_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else fail();
        }
        return value;
    }

    std::string read_string() {
        if (!take('"')) fail();
        std::string out;
        while (at_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return out;
            if (c < 0x20) fail();
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (at_ == text_.size()) fail();
            switch (text_[at_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (text_.size() - at_ < 6 || text_[at_] != '\\' ||
                        text_[at_ + 1] != 'u') fail();
                    at_ += 2;
                    const uint32_t low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    fail();
                }
                utf8(out, cp);
                break;
            }
            default: fail();
            }
        }
        fail();
    }

    Json read_value() {
        if (++depth_ > kMaxDepth) throw std::runtime_error("JSON nested too deeply");
        Json value = read_value_inner();
        --depth_;
        return value;
    }

    Json read_value_inner() {
        whitespace();
        if (at_ == text_.size()) fail();
        if (text_[at_] == '"') return Json(read_string());
        if (take('{')) {
            Json::Object out;
            if (take('}')) return Json(std::move(out));
            do {
                std::string key = read_string();
                if (!take(':')) fail();
                out[std::move(key)] = read_value();
            } while (take(','));
            if (!take('}')) fail();
            return Json(std::move(out));
        }
        if (take('[')) {
            Json::Array out;
            if (take(']')) return Json(std::move(out));
            do { out.push_back(read_value()); } while (take(','));
            if (!take(']')) fail();
            return Json(std::move(out));
        }
        if (text_.compare(at_, 4, "true") == 0) { at_ += 4; return Json(true); }
        if (text_.compare(at_, 5, "false") == 0) { at_ += 5; return Json(false); }
        if (text_.compare(at_, 4, "null") == 0) { at_ += 4; return Json(nullptr); }
        const char *start = text_.c_str() + at_;
        char *end = nullptr;
        const double number = std::strtod(start, &end);
        if (end == start) fail();
        at_ += static_cast<size_t>(end - start);
        return Json(number);
    }
};

void escaped(std::string &out, const std::string &text) {
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void write_value(std::string &out, const Json &json) {
    if (std::holds_alternative<std::nullptr_t>(json.data)) {
        out += "null";
    } else if (const auto *v = std::get_if<bool>(&json.data)) {
        out += *v ? "true" : "false";
    } else if (const auto *v = std::get_if<double>(&json.data)) {
        out += std::to_string(static_cast<int64_t>(*v));
    } else if (const auto *v = std::get_if<std::string>(&json.data)) {
        escaped(out, *v);
    } else if (const auto *v = std::get_if<Json::Array>(&json.data)) {
        out.push_back('[');
        bool first = true;
        for (const Json &item : *v) {
            if (!first) out.push_back(',');
            first = false;
            write_value(out, item);
        }
        out.push_back(']');
    } else {
        const auto &object = std::get<Json::Object>(json.data);
        out.push_back('{');
        bool first = true;
        for (const auto &[key, item] : object) {
            if (!first) out.push_back(',');
            first = false;
            escaped(out, key);
            out.push_back(':');
            write_value(out, item);
        }
        out.push_back('}');
    }
}

}  // namespace

Json json_parse(const std::string &text) { return Parser(text).parse(); }

std::string json_write(const Json &value) {
    std::string out;
    write_value(out, value);
    return out;
}
