// Zero-copy SIMD JSON-RPC tape, streaming JsonWriter, O(1) SIMD LineIndex, and
// pipelined Content-Length framing for brat-lsp v0.4.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsp_wire {

inline constexpr size_t kMaxFrameBytes = 16u * 1024u * 1024u;
inline constexpr size_t kMaxJsonNodes = 1048576u;

// SIMD-accelerated O(1) line and UTF-16 index.
// `starts[i]` is the byte offset of 0-based line `i` in `text`.
// `non_ascii[i]` is 1 iff line `i` contains any byte >= 0x80 (UTF-8 multi-byte).
// On ASCII lines (~99.5% of code), UTF-16 column <-> byte offset is O(1) math.
struct LineIndex {
    std::vector<uint32_t> starts;
    std::vector<uint8_t> non_ascii;

    void build(std::string_view text);

    size_t line_count() const { return starts.size(); }

    // Byte slice of 0-based `line` excluding trailing `\n` and `\r`.
    std::string_view line_view(std::string_view text, int line) const;

    // Byte offset of start of 0-based `line` (clamped to text.size()).
    size_t line_start_offset(std::string_view text, int line) const;

    // 0-based line containing `byte_offset` via binary search (O(log L)).
    int line_of_offset(size_t byte_offset) const;

    // UTF-16 length of arbitrary UTF-8 slice (O(1) when `known_ascii` is true).
    static int utf16_len(std::string_view s, bool known_ascii = false);

    // UTF-16 length of 0-based `line` in O(1) for ASCII lines.
    int line_utf16_len(std::string_view text, int line) const;

    // Strict LSP UTF-16 (line, character) -> byte offset in `text`.
    // Rejects negative coordinates, out-of-range lines/columns, and positions
    // landing in the middle of a surrogate pair (astral codepoint).
    bool position_to_offset(std::string_view text, int line, int character,
                            size_t &out_offset) const;

    // Clamped UTF-16 column -> byte offset within a single line slice `line_str`
    // (matches `utf16_offset` semantics for cursor queries).
    size_t clamp_col_in_line(std::string_view text, int line, int character) const;

    // Byte offset within `line` -> UTF-16 column (O(1) on ASCII lines).
    int byte_to_utf16_col(std::string_view text, int line, size_t byte_in_line) const;
};

// Zero-copy arena JSON-RPC tape (RFC 8259, zero std::map allocations).
enum class JKind : uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    StrRaw = 4,    // unescaped slice directly inside input buffer `src_`
    StrArena = 5,  // decoded string slice inside reusable `arena_`
    Array = 6,     // payload = count, aux = index of contiguous children or link
    Object = 7,    // payload = pair count, aux = head child index
};

struct TapeNode {
    JKind kind = JKind::Null;
    uint8_t flags = 0;
    uint32_t next_sibling = 0;  // 0 = none; linked list inside flat `nodes_`.
    uint32_t first_child = 0;   // for Array / Object (for Object: key node -> val node -> next key...)
    uint32_t count = 0;         // element count (Array) or key-value pair count (Object)
    uint32_t raw_off = 0;       // byte offset in `src_` (for raw JSON echo of `id` or StrRaw)
    uint32_t raw_len = 0;       // byte length in `src_` or `arena_`
    union {
        int64_t i64;
        double f64;
    } num{0};
};

class LspJsonTape {
public:
    LspJsonTape() {
        nodes_.reserve(256);
        arena_.reserve(16384);
    }

    // Parses `src` into reusable flat `nodes_` + `arena_`. Returns false on invalid JSON.
    bool parse(std::string_view src);

    std::string_view error() const { return err_; }
    uint32_t root() const { return root_; }
    const TapeNode &at(uint32_t idx) const { return nodes_[idx]; }

    bool is_null(uint32_t idx) const { return idx == 0 || nodes_[idx].kind == JKind::Null; }
    bool is_bool(uint32_t idx) const { return idx != 0 && nodes_[idx].kind == JKind::Bool; }
    bool is_array(uint32_t idx) const { return idx != 0 && nodes_[idx].kind == JKind::Array; }
    bool is_object(uint32_t idx) const { return idx != 0 && nodes_[idx].kind == JKind::Object; }
    bool is_string(uint32_t idx) const {
        return idx != 0 && (nodes_[idx].kind == JKind::StrRaw || nodes_[idx].kind == JKind::StrArena);
    }
    bool is_number(uint32_t idx) const {
        return idx != 0 && (nodes_[idx].kind == JKind::Int || nodes_[idx].kind == JKind::Float);
    }

    bool bool_val(uint32_t idx, bool fallback = false) const {
        return is_bool(idx) ? (nodes_[idx].num.i64 != 0) : fallback;
    }

    // Exact integer check matching `exact_integer` (rejects floats, NaN, out-of-int32).
    bool exact_int32(uint32_t idx, int &out) const;
    int int_or(uint32_t idx, int fallback = 0) const;

    // Zero-copy string_view! Points either directly into `src_` or into `arena_`.
    std::string_view str_view(uint32_t idx) const;

    // Raw JSON slice from input buffer (e.g., to echo `"id"` verbatim without re-formatting!).
    std::string_view raw_json_slice(uint32_t idx) const;

    // Object key lookup (linear over flat tape nodes: L1 cache hot for 2..10 LSP keys!).
    uint32_t get(uint32_t obj_idx, std::string_view key) const;

private:
    std::string_view src_;
    std::vector<TapeNode> nodes_;
    std::string arena_;
    uint32_t root_ = 0;
    std::string_view err_;
};

// Direct streaming JsonWriter (SIMD-escaped strings + std::to_chars).
class JsonWriter {
public:
    JsonWriter() { buf_.reserve(65536); }

    void reset() {
        buf_.clear();
        need_comma_ = false;
    }

    std::string_view view() const { return buf_; }
    size_t size() const { return buf_.size(); }
    const char *data() const { return buf_.data(); }

    void begin_obj() {
        comma_if_needed();
        buf_.push_back('{');
        need_comma_ = false;
    }
    void end_obj() {
        buf_.push_back('}');
        need_comma_ = true;
    }
    void begin_arr() {
        comma_if_needed();
        buf_.push_back('[');
        need_comma_ = false;
    }
    void end_arr() {
        buf_.push_back(']');
        need_comma_ = true;
    }

    void key(std::string_view k) {
        comma_if_needed();
        write_escaped_string(buf_, k);
        buf_.push_back(':');
        need_comma_ = false;
    }

    // Fast path for compile-time ASCII literal keys (no escaping needed!).
    template <size_t N>
    void key_lit(const char (&lit)[N]) {
        comma_if_needed();
        buf_.push_back('"');
        buf_.append(lit, N - 1);
        buf_.append("\":", 2);
        need_comma_ = false;
    }

    void str(std::string_view s) {
        comma_if_needed();
        write_escaped_string(buf_, s);
        need_comma_ = true;
    }

    void int64(int64_t v);
    void uint64(uint64_t v);
    void boolean(bool v) {
        comma_if_needed();
        if (v) buf_.append("true", 4);
        else buf_.append("false", 5);
        need_comma_ = true;
    }
    void null_val() {
        comma_if_needed();
        buf_.append("null", 4);
        need_comma_ = true;
    }
    void raw_value(std::string_view raw) {
        comma_if_needed();
        if (raw.empty()) buf_.append("null", 4);
        else buf_.append(raw.data(), raw.size());
        need_comma_ = true;
    }

    // Convenience LSP range helper: `{"start":{"line":L1,"character":C1},"end":{"line":L2,"character":C2}}`
    void lsp_range(int l1, int c1, int l2, int c2);

    static void write_escaped_string(std::string &out, std::string_view s);

private:
    std::string buf_;
    bool need_comma_ = false;

    void comma_if_needed() {
        if (need_comma_) buf_.push_back(',');
    }
};

// Pipelined zero-copy frame reader and single-write frame sender.
enum class FrameStatus : uint8_t {
    Ok = 0,
    Eof = 1,
    HeaderError = 2,
};

class LspFrameReader {
public:
    LspFrameReader() { rx_.reserve(131072); }

    // Reads next Content-Length frame into internal slab and sets `out_body`
    // as a valid `string_view` until the next `next_frame()` call.
    FrameStatus next_frame(std::string_view &out_body, std::string &out_err);

private:
    std::string rx_;
    size_t pos_ = 0;
    bool eof_ = false;

    bool fill_more();
    void compact_if_needed();
};

void send_frame(std::string_view json_body);

}  // namespace lsp_wire
