#include "lsp_wire.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__) && defined(__has_include)
#if __has_include(<xlocale.h>)
#include <xlocale.h>
#endif
#elif defined(__APPLE__)
#include <xlocale.h>
#endif
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#define BRAT_LSP_AVX2_COMPILED 1
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define BRAT_LSP_SSE2 1
#define BRAT_LSP_RUNTIME_AVX2 1
#elif defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define BRAT_LSP_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define BRAT_LSP_NEON 1
#endif

namespace lsp_wire {

namespace {

#if defined(BRAT_LSP_RUNTIME_AVX2)
inline bool cpu_has_avx2() {
    static const bool supported = __builtin_cpu_supports("avx2");
    return supported;
}

__attribute__((target("avx2")))
size_t avx2_scan_json_str_safe(const char *p, size_t n) {
    size_t i = 0;
    const __m256i vquote = _mm256_set1_epi8('"');
    const __m256i vbslash = _mm256_set1_epi8('\\');
    const __m256i vctrl_max = _mm256_set1_epi8(0x1f);
    while (i + 32 <= n) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
        const __m256i eq_q = _mm256_cmpeq_epi8(v, vquote);
        const __m256i eq_b = _mm256_cmpeq_epi8(v, vbslash);
        const __m256i is_ctrl = _mm256_cmpeq_epi8(_mm256_max_epu8(v, vctrl_max), vctrl_max);
        const uint32_t mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(eq_q, eq_b), is_ctrl)));
        if (mask != 0) return i + static_cast<size_t>(__builtin_ctz(mask));
        i += 32;
    }
    while (i < n) {
        const auto ch = static_cast<unsigned char>(p[i]);
        if (ch < 0x20u || ch == '"' || ch == '\\') return i;
        ++i;
    }
    return n;
}

__attribute__((target("avx2")))
size_t avx2_build_lines(const char *p, size_t n, std::vector<uint32_t> &starts,
                        std::vector<uint8_t> &non_ascii, uint8_t &cur_hi) {
    size_t i = 0;
    const __m256i vnl = _mm256_set1_epi8('\n');
    while (i + 32 <= n) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
        uint32_t nl_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vnl)));
        const uint32_t hi_mask = static_cast<uint32_t>(_mm256_movemask_epi8(v));
        if (nl_mask == 0) {
            cur_hi |= static_cast<uint8_t>(hi_mask != 0);
        } else {
            uint32_t prev_bit = 0;
            while (nl_mask != 0) {
                const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(nl_mask));
                const uint32_t seg_mask = (bit == 0) ? 0u : (((1u << bit) - 1u) & ~((1u << prev_bit) - 1u));
                if ((hi_mask & seg_mask) != 0) cur_hi = 1;
                non_ascii.push_back(cur_hi);
                cur_hi = 0;
                starts.push_back(static_cast<uint32_t>(i + bit + 1));
                prev_bit = bit + 1;
                nl_mask &= (nl_mask - 1u);
            }
            if (prev_bit < 32u) {
                const uint32_t tail_mask = ~((1u << prev_bit) - 1u);
                if ((hi_mask & tail_mask) != 0) cur_hi = 1;
            }
        }
        i += 32;
    }
    return i;
}
#endif

inline uint32_t decode_utf8_cp(std::string_view s, size_t &i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    if (c < 0xc2 || c > 0xf4) { ++i; return 0xfffd; }
    const size_t n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (i + n > s.size()) { i = s.size(); return 0xfffd; }
    uint32_t cp = c & (n == 2 ? 0x1fu : n == 3 ? 0x0fu : 0x07u);
    for (size_t k = 1; k < n; ++k) {
        const auto cont = static_cast<unsigned char>(s[i + k]);
        if ((cont & 0xc0u) != 0x80u) { ++i; return 0xfffd; }
        cp = (cp << 6) | (cont & 0x3fu);
    }
    const uint32_t min_cp = (n == 2) ? 0x80u : (n == 3) ? 0x800u : 0x10000u;
    if (cp < min_cp || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        ++i;
        return 0xfffd;
    }
    i += n;
    return cp;
}

// Finds first index in `p[0..n)` where byte is `"`, `\`, or unsigned `< 0x20`.
// Returns `n` if all bytes are safe unescaped JSON string characters (including valid UTF-8 >= 0x80).
inline size_t simd_scan_json_str_safe(const char *p, size_t n) {
#if defined(BRAT_LSP_RUNTIME_AVX2)
    if (n >= 32 && cpu_has_avx2()) return avx2_scan_json_str_safe(p, n);
#endif
    size_t i = 0;
#if defined(BRAT_LSP_AVX2_COMPILED)
    const __m256i vquote = _mm256_set1_epi8('"');
    const __m256i vbslash = _mm256_set1_epi8('\\');
    const __m256i vctrl_max = _mm256_set1_epi8(0x1f);
    while (i + 32 <= n) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
        const __m256i eq_q = _mm256_cmpeq_epi8(v, vquote);
        const __m256i eq_b = _mm256_cmpeq_epi8(v, vbslash);
        const __m256i is_ctrl = _mm256_cmpeq_epi8(_mm256_max_epu8(v, vctrl_max), vctrl_max);
        const uint32_t mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(eq_q, eq_b), is_ctrl)));
        if (mask != 0) return i + static_cast<size_t>(__builtin_ctz(mask));
        i += 32;
    }
#elif defined(BRAT_LSP_SSE2)
    const __m128i vquote = _mm_set1_epi8('"');
    const __m128i vbslash = _mm_set1_epi8('\\');
    const __m128i vctrl_max = _mm_set1_epi8(0x1f);
    while (i + 16 <= n) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + i));
        const __m128i eq_q = _mm_cmpeq_epi8(v, vquote);
        const __m128i eq_b = _mm_cmpeq_epi8(v, vbslash);
        const __m128i is_ctrl = _mm_cmpeq_epi8(_mm_max_epu8(v, vctrl_max), vctrl_max);
        const uint32_t mask = static_cast<uint32_t>(
            _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(eq_q, eq_b), is_ctrl)));
        if (mask != 0) return i + static_cast<size_t>(__builtin_ctz(mask));
        i += 16;
    }
#elif defined(BRAT_LSP_NEON)
    const uint8x16_t vquote = vdupq_n_u8('"');
    const uint8x16_t vbslash = vdupq_n_u8('\\');
    const uint8x16_t vctrl = vdupq_n_u8(0x20);
    while (i + 16 <= n) {
        const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(p + i));
        const uint8x16_t bad = vorrq_u8(vorrq_u8(vceqq_u8(v, vquote), vceqq_u8(v, vbslash)),
                                        vcltq_u8(v, vctrl));
        const uint64x2_t pair = vreinterpretq_u64_u8(bad);
        if ((vgetq_lane_u64(pair, 0) | vgetq_lane_u64(pair, 1)) != 0) {
            for (size_t k = 0; k < 16; ++k) {
                const auto ch = static_cast<unsigned char>(p[i + k]);
                if (ch < 0x20u || ch == '"' || ch == '\\') return i + k;
            }
        }
        i += 16;
    }
#else
    // 64-bit SWAR fast scan for pure-ASCII unescaped runs
    while (i + 8 <= n) {
        uint64_t w = 0;
        std::memcpy(&w, p + i, 8);
        // Any high bit or <0x20 or '"' (0x22) or '\\' (0x5c):
        const uint64_t fq = w ^ 0x2222222222222222ULL;
        const uint64_t fb = w ^ 0x5c5c5c5c5c5c5c5cULL;
        const uint64_t has_q = (fq - 0x0101010101010101ULL) & ~fq & 0x8080808080808080ULL;
        const uint64_t has_b = (fb - 0x0101010101010101ULL) & ~fb & 0x8080808080808080ULL;
        const uint64_t has_ctrl_or_hi = ((w - 0x2020202020202020ULL) | w) & 0x8080808080808080ULL;
        if ((has_q | has_b | has_ctrl_or_hi) != 0) {
            for (size_t k = 0; k < 8; ++k) {
                const auto ch = static_cast<unsigned char>(p[i + k]);
                if (ch < 0x20u || ch == '"' || ch == '\\') return i + k;
            }
        }
        i += 8;
    }
#endif
    while (i < n) {
        const auto ch = static_cast<unsigned char>(p[i]);
        if (ch < 0x20u || ch == '"' || ch == '\\') return i;
        ++i;
    }
    return n;
}

inline size_t simd_skip_ws(const char *p, size_t i, size_t n) {
#if defined(BRAT_LSP_SSE2)
    const __m128i vsp = _mm_set1_epi8(' ');
    const __m128i vnl = _mm_set1_epi8('\n');
    const __m128i vcr = _mm_set1_epi8('\r');
    const __m128i vtb = _mm_set1_epi8('\t');
    while (i + 16 <= n) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + i));
        const __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, vsp), _mm_cmpeq_epi8(v, vnl)),
                                        _mm_or_si128(_mm_cmpeq_epi8(v, vcr), _mm_cmpeq_epi8(v, vtb)));
        const uint32_t non_ws = static_cast<uint32_t>(~_mm_movemask_epi8(ws)) & 0xffffu;
        if (non_ws != 0) return i + static_cast<size_t>(__builtin_ctz(non_ws));
        i += 16;
    }
#endif
    while (i < n) {
        const char c = p[i];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
        ++i;
    }
    return i;
}

inline void append_utf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffu) {
        out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffffu) {
        out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
}

}  // namespace

// LineIndex implementation (SIMD newline + non-ASCII bitmask scan).
void LineIndex::build(std::string_view text) {
    starts.clear();
    non_ascii.clear();
    const size_t approx_lines = text.size() / 32 + 4;
    if (starts.capacity() < approx_lines) {
        starts.reserve(approx_lines);
        non_ascii.reserve(approx_lines);
    }
    starts.push_back(0);
    uint8_t cur_hi = 0;
    const char *p = text.data();
    const size_t n = text.size();
    size_t i = 0;

#if defined(BRAT_LSP_RUNTIME_AVX2)
    if (n >= 32 && cpu_has_avx2()) {
        i = avx2_build_lines(p, n, starts, non_ascii, cur_hi);
    } else
#endif
    {
#if defined(BRAT_LSP_AVX2_COMPILED)
        const __m256i vnl = _mm256_set1_epi8('\n');
        while (i + 32 <= n) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
            uint32_t nl_mask = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vnl)));
            const uint32_t hi_mask = static_cast<uint32_t>(_mm256_movemask_epi8(v));
            if (nl_mask == 0) {
                cur_hi |= static_cast<uint8_t>(hi_mask != 0);
            } else {
                uint32_t prev_bit = 0;
                while (nl_mask != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(nl_mask));
                    const uint32_t seg_mask = (bit == 0) ? 0u : (((1u << bit) - 1u) & ~((1u << prev_bit) - 1u));
                    if ((hi_mask & seg_mask) != 0) cur_hi = 1;
                    non_ascii.push_back(cur_hi);
                    cur_hi = 0;
                    starts.push_back(static_cast<uint32_t>(i + bit + 1));
                    prev_bit = bit + 1;
                    nl_mask &= (nl_mask - 1u);
                }
                if (prev_bit < 32u) {
                    const uint32_t tail_mask = ~((1u << prev_bit) - 1u);
                    if ((hi_mask & tail_mask) != 0) cur_hi = 1;
                }
            }
            i += 32;
        }
#elif defined(BRAT_LSP_SSE2)
        const __m128i vnl = _mm_set1_epi8('\n');
        while (i + 16 <= n) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + i));
            uint32_t nl_mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, vnl)));
            const uint32_t hi_mask = static_cast<uint32_t>(_mm_movemask_epi8(v));
            if (nl_mask == 0) {
                cur_hi |= static_cast<uint8_t>(hi_mask != 0);
            } else {
                uint32_t prev_bit = 0;
                while (nl_mask != 0) {
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(nl_mask));
                    const uint32_t seg_mask = (bit == 0) ? 0u : (((1u << bit) - 1u) & ~((1u << prev_bit) - 1u));
                    if ((hi_mask & seg_mask) != 0) cur_hi = 1;
                    non_ascii.push_back(cur_hi);
                    cur_hi = 0;
                    starts.push_back(static_cast<uint32_t>(i + bit + 1));
                    prev_bit = bit + 1;
                    nl_mask &= (nl_mask - 1u);
                }
                if (prev_bit < 16u) {
                    const uint32_t tail_mask = (0xffffu) & ~((1u << prev_bit) - 1u);
                    if ((hi_mask & tail_mask) != 0) cur_hi = 1;
                }
            }
            i += 16;
        }
#endif
    }
    while (i < n) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (c == '\n') {
            non_ascii.push_back(cur_hi);
            cur_hi = 0;
            starts.push_back(static_cast<uint32_t>(i + 1));
        } else if (c >= 0x80u) {
            cur_hi = 1;
        }
        ++i;
    }
    non_ascii.push_back(cur_hi);
}

size_t LineIndex::line_start_offset(std::string_view text, int line) const {
    if (line <= 0 || starts.empty()) return 0;
    if (static_cast<size_t>(line) >= starts.size()) return text.size();
    return starts[static_cast<size_t>(line)];
}

std::string_view LineIndex::line_view(std::string_view text, int line) const {
    if (line < 0 || static_cast<size_t>(line) >= starts.size()) return {};
    const size_t s = starts[static_cast<size_t>(line)];
    size_t e = (static_cast<size_t>(line + 1) < starts.size())
                   ? (starts[static_cast<size_t>(line + 1)] - 1)
                   : text.size();
    if (e > s && text[e - 1] == '\r') --e;
    return text.substr(s, e - s);
}

int LineIndex::line_of_offset(size_t byte_offset) const {
    if (starts.empty()) return 0;
    auto it = std::upper_bound(starts.begin(), starts.end(), static_cast<uint32_t>(byte_offset));
    if (it == starts.begin()) return 0;
    return static_cast<int>((it - starts.begin()) - 1);
}

int LineIndex::utf16_len(std::string_view s, bool known_ascii) {
    if (known_ascii) return static_cast<int>(s.size());
    int n = 0;
    for (size_t i = 0; i < s.size();) n += decode_utf8_cp(s, i) > 0xffffu ? 2 : 1;
    return n;
}

int LineIndex::line_utf16_len(std::string_view text, int line) const {
    if (line < 0 || static_cast<size_t>(line) >= starts.size()) return 0;
    const std::string_view sv = line_view(text, line);
    if (non_ascii[static_cast<size_t>(line)] == 0) return static_cast<int>(sv.size());
    return utf16_len(sv, false);
}

bool LineIndex::position_to_offset(std::string_view text, int line, int character,
                                   size_t &out_offset) const {
    if (line < 0 || character < 0 || static_cast<size_t>(line) >= starts.size()) return false;
    const size_t start = starts[static_cast<size_t>(line)];
    const std::string_view sv = line_view(text, line);
    if (non_ascii[static_cast<size_t>(line)] == 0) {
        if (static_cast<size_t>(character) > sv.size()) return false;
        out_offset = start + static_cast<size_t>(character);
        return true;
    }
    size_t at = 0;
    int units = 0;
    while (at < sv.size()) {
        if (units == character) {
            out_offset = start + at;
            return true;
        }
        const size_t prev = at;
        const int width = decode_utf8_cp(sv, at) > 0xffffu ? 2 : 1;
        if (at > sv.size() || units + width > character || at <= prev) return false;
        units += width;
    }
    if (units != character) return false;
    out_offset = start + sv.size();
    return true;
}

size_t LineIndex::clamp_col_in_line(std::string_view text, int line, int character) const {
    if (line < 0 || static_cast<size_t>(line) >= starts.size() || character <= 0) return 0;
    const std::string_view sv = line_view(text, line);
    if (non_ascii[static_cast<size_t>(line)] == 0) {
        return std::min<size_t>(sv.size(), static_cast<size_t>(character));
    }
    size_t i = 0;
    int units = character;
    while (i < sv.size() && units > 0) {
        const size_t old = i;
        units -= decode_utf8_cp(sv, i) > 0xffffu ? 2 : 1;
        if (units < 0) return old;
    }
    return i;
}

int LineIndex::byte_to_utf16_col(std::string_view text, int line, size_t byte_in_line) const {
    if (line < 0 || static_cast<size_t>(line) >= starts.size()) return 0;
    const std::string_view sv = line_view(text, line);
    const size_t clamped = std::min(byte_in_line, sv.size());
    if (non_ascii[static_cast<size_t>(line)] == 0) return static_cast<int>(clamped);
    return utf16_len(sv.substr(0, clamped), false);
}

// Zero-copy arena JSON-RPC tape parser.
namespace {

constexpr int kMaxJsonDepth = 200;

bool valid_json_utf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        // Most protocol strings are ASCII. Skip whole words without alignment
        // assumptions or reading beyond the string view.
        while (text.size() - i >= sizeof(uint64_t)) {
            uint64_t word;
            std::memcpy(&word, text.data() + i, sizeof(word));
            if (word & UINT64_C(0x8080808080808080)) break;
            i += sizeof(word);
        }
        if (i == text.size()) break;
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80u) continue;
        size_t tail;
        uint32_t cp, minimum;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            tail = 1; cp = lead & 0x1fu; minimum = 0x80u;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            tail = 2; cp = lead & 0x0fu; minimum = 0x800u;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            tail = 3; cp = lead & 0x07u; minimum = 0x10000u;
        } else {
            return false;
        }
        if (text.size() - i < tail) return false;
        for (size_t k = 0; k < tail; ++k) {
            const auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0u) != 0x80u) return false;
            cp = (cp << 6) | (c & 0x3fu);
        }
        if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
            return false;
    }
    return true;
}

class TapeParser {
public:
    TapeParser(std::string_view src, std::vector<TapeNode> &nodes, std::string &arena)
        : src_(src), p_(src.data()), n_(src.size()), nodes_(nodes), arena_(arena) {}

    bool run(uint32_t &out_root, std::string_view &out_err) {
        nodes_.clear();
        arena_.clear();
        nodes_.push_back(TapeNode{});  // sentinel 0 = invalid/null
        out_root = read_value();
        if (out_root == 0) {
            out_err = err_;
            return false;
        }
        at_ = simd_skip_ws(p_, at_, n_);
        if (at_ != n_) {
            out_err = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    std::string_view src_;
    const char *p_;
    size_t n_;
    size_t at_ = 0;
    int depth_ = 0;
    std::vector<TapeNode> &nodes_;
    std::string &arena_;
    std::string_view err_ = "invalid JSON";

    uint32_t alloc_node(JKind k, uint32_t raw_off) {
        const uint32_t idx = static_cast<uint32_t>(nodes_.size());
        TapeNode nd;
        nd.kind = k;
        nd.raw_off = raw_off;
        nodes_.push_back(nd);
        return idx;
    }

    bool parse_hex4(uint32_t &out) {
        if (n_ - at_ < 4) return false;
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p_[at_++];
            val <<= 4;
            if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        out = val;
        return true;
    }

    uint32_t read_string() {
        const uint32_t start_tok = static_cast<uint32_t>(at_);
        ++at_;  // consume opening '"'
        const size_t str_start = at_;
        const size_t safe_len = simd_scan_json_str_safe(p_ + at_, n_ - at_);
        if (!valid_json_utf8(src_.substr(at_, safe_len))) {
            err_ = "invalid UTF-8 in JSON string";
            return 0;
        }
        at_ += safe_len;
        if (at_ < n_ && p_[at_] == '"') {
            // Fast path: unescaped string slice referencing `src_` directly.
            const uint32_t idx = alloc_node(JKind::StrRaw, static_cast<uint32_t>(str_start));
            nodes_[idx].raw_len = static_cast<uint32_t>(safe_len);
            ++at_;  // closing '"'
            return idx;
        }
        if (at_ >= n_ || static_cast<unsigned char>(p_[at_]) < 0x20u) return 0;

        // Slow-ish path: string has `\` escapes -> SIMD-chunk copy into `arena_`.
        const uint32_t arena_off = static_cast<uint32_t>(arena_.size());
        if (safe_len > 0) arena_.append(p_ + str_start, safe_len);

        while (at_ < n_) {
            const auto ch = static_cast<unsigned char>(p_[at_]);
            if (ch == '"') {
                ++at_;
                const uint32_t idx = alloc_node(JKind::StrArena, start_tok);
                nodes_[idx].first_child = arena_off;
                nodes_[idx].raw_len = static_cast<uint32_t>(arena_.size() - arena_off);
                nodes_[idx].count = static_cast<uint32_t>(at_ - start_tok);
                return idx;
            }
            if (ch < 0x20u) return 0;
            if (ch != '\\') {
                const size_t run = simd_scan_json_str_safe(p_ + at_, n_ - at_);
                if (!valid_json_utf8(src_.substr(at_, run))) {
                    err_ = "invalid UTF-8 in JSON string";
                    return 0;
                }
                arena_.append(p_ + at_, run);
                at_ += run;
                continue;
            }
            ++at_;  // skip '\\'
            if (at_ >= n_) return 0;
            const char esc = p_[at_++];
            switch (esc) {
            case '"': arena_.push_back('"'); break;
            case '\\': arena_.push_back('\\'); break;
            case '/': arena_.push_back('/'); break;
            case 'b': arena_.push_back('\b'); break;
            case 'f': arena_.push_back('\f'); break;
            case 'n': arena_.push_back('\n'); break;
            case 'r': arena_.push_back('\r'); break;
            case 't': arena_.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(cp)) return 0;
                if (cp >= 0xd800u && cp <= 0xdbffu) {
                    if (n_ - at_ < 2 || p_[at_] != '\\' || p_[at_ + 1] != 'u') return 0;
                    at_ += 2;
                    uint32_t lo = 0;
                    if (!parse_hex4(lo) || lo < 0xdc00u || lo > 0xdfffu) return 0;
                    cp = 0x10000u + (((cp - 0xd800u) << 10) | (lo - 0xdc00u));
                } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                    return 0;
                }
                append_utf8(arena_, cp);
                break;
            }
            default:
                return 0;
            }
        }
        return 0;
    }

    uint32_t read_number() {
        const size_t start = at_;
        if (p_[at_] == '-') ++at_;
        if (at_ >= n_) return 0;
        if (p_[at_] == '0') {
            ++at_;
        } else if (p_[at_] >= '1' && p_[at_] <= '9') {
            while (at_ < n_ && p_[at_] >= '0' && p_[at_] <= '9') ++at_;
        } else {
            return 0;
        }
        bool is_float = false;
        if (at_ < n_ && p_[at_] == '.') {
            is_float = true;
            ++at_;
            if (at_ >= n_ || p_[at_] < '0' || p_[at_] > '9') return 0;
            while (at_ < n_ && p_[at_] >= '0' && p_[at_] <= '9') ++at_;
        }
        if (at_ < n_ && (p_[at_] == 'e' || p_[at_] == 'E')) {
            is_float = true;
            ++at_;
            if (at_ < n_ && (p_[at_] == '+' || p_[at_] == '-')) ++at_;
            if (at_ >= n_ || p_[at_] < '0' || p_[at_] > '9') return 0;
            while (at_ < n_ && p_[at_] >= '0' && p_[at_] <= '9') ++at_;
        }
        const size_t len = at_ - start;
        if (!is_float) {
            int64_t iv = 0;
            auto r = std::from_chars(p_ + start, p_ + at_, iv);
            if (r.ec == std::errc{} && r.ptr == p_ + at_) {
                const uint32_t idx = alloc_node(JKind::Int, static_cast<uint32_t>(start));
                nodes_[idx].raw_len = static_cast<uint32_t>(len);
                nodes_[idx].num.i64 = iv;
                return idx;
            }
        }
        // Fallback / float parse
        char stack_buf[64];
        std::string heap_buf;
        const char *cstr = nullptr;
        if (len < sizeof(stack_buf)) {
            std::memcpy(stack_buf, p_ + start, len);
            stack_buf[len] = '\0';
            cstr = stack_buf;
        } else {
            heap_buf.assign(p_ + start, len);
            cstr = heap_buf.c_str();
        }
        if (std::memchr(cstr, ',', len) != nullptr) return 0;
        char *endp = nullptr;
#if defined(_MSC_VER)
        static _locale_t c_loc = _create_locale(LC_NUMERIC, "C");
        double dv = c_loc ? _strtod_l(cstr, &endp, c_loc) : std::strtod(cstr, &endp);
        if (!endp || static_cast<size_t>(endp - cstr) != len) return 0;
#elif defined(_WIN32)
        double dv = 0.0;
        const char *dp = std::localeconv()->decimal_point;
        if (dp && dp[0] && !(dp[0] == '.' && dp[1] == '\0')) {
            std::string loc(cstr, len);
            size_t dot = loc.find('.');
            if (dot != std::string::npos) loc.replace(dot, 1, dp);
            dv = std::strtod(loc.c_str(), &endp);
            if (!endp || static_cast<size_t>(endp - loc.c_str()) != loc.size()) return 0;
        } else {
            dv = std::strtod(cstr, &endp);
            if (!endp || static_cast<size_t>(endp - cstr) != len) return 0;
        }
#else
        static locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        double dv = c_loc ? strtod_l(cstr, &endp, c_loc) : std::strtod(cstr, &endp);
        if (!endp || static_cast<size_t>(endp - cstr) != len) return 0;
#endif
        const uint32_t idx = alloc_node(JKind::Float, static_cast<uint32_t>(start));
        nodes_[idx].raw_len = static_cast<uint32_t>(len);
        nodes_[idx].num.f64 = dv;
        return idx;
    }

    uint32_t read_value() {
        if (nodes_.size() > kMaxJsonNodes) {
            err_ = "JSON token count exceeds limit";
            return 0;
        }
        at_ = simd_skip_ws(p_, at_, n_);
        if (at_ >= n_) return 0;
        const char c = p_[at_];
        if (c == '"') return read_string();
        if (c == '{') {
            if (++depth_ > kMaxJsonDepth) return 0;
            const uint32_t obj_start = static_cast<uint32_t>(at_);
            ++at_;
            const uint32_t obj_idx = alloc_node(JKind::Object, obj_start);
            at_ = simd_skip_ws(p_, at_, n_);
            if (at_ < n_ && p_[at_] == '}') {
                ++at_;
                nodes_[obj_idx].raw_len = static_cast<uint32_t>(at_ - obj_start);
                --depth_;
                return obj_idx;
            }
            uint32_t prev_link = 0;
            uint32_t pairs = 0;
            while (true) {
                if (nodes_.size() > kMaxJsonNodes) {
                    err_ = "JSON token count exceeds limit";
                    return 0;
                }
                at_ = simd_skip_ws(p_, at_, n_);
                if (at_ >= n_ || p_[at_] != '"') return 0;
                const uint32_t k_idx = read_string();
                if (k_idx == 0) return 0;
                at_ = simd_skip_ws(p_, at_, n_);
                if (at_ >= n_ || p_[at_] != ':') return 0;
                ++at_;
                const uint32_t v_idx = read_value();
                if (v_idx == 0) return 0;
                nodes_[k_idx].next_sibling = v_idx;
                if (prev_link == 0) nodes_[obj_idx].first_child = k_idx;
                else nodes_[prev_link].next_sibling = k_idx;
                prev_link = v_idx;
                ++pairs;
                at_ = simd_skip_ws(p_, at_, n_);
                if (at_ < n_ && p_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < n_ && p_[at_] == '}') {
                    ++at_;
                    break;
                }
                return 0;
            }
            nodes_[obj_idx].count = pairs;
            nodes_[obj_idx].raw_len = static_cast<uint32_t>(at_ - obj_start);
            --depth_;
            return obj_idx;
        }
        if (c == '[') {
            if (++depth_ > kMaxJsonDepth) return 0;
            const uint32_t arr_start = static_cast<uint32_t>(at_);
            ++at_;
            const uint32_t arr_idx = alloc_node(JKind::Array, arr_start);
            at_ = simd_skip_ws(p_, at_, n_);
            if (at_ < n_ && p_[at_] == ']') {
                ++at_;
                nodes_[arr_idx].raw_len = static_cast<uint32_t>(at_ - arr_start);
                --depth_;
                return arr_idx;
            }
            uint32_t prev_elem = 0;
            uint32_t count = 0;
            while (true) {
                const uint32_t elem_idx = read_value();
                if (elem_idx == 0) return 0;
                if (prev_elem == 0) nodes_[arr_idx].first_child = elem_idx;
                else nodes_[prev_elem].next_sibling = elem_idx;
                prev_elem = elem_idx;
                ++count;
                at_ = simd_skip_ws(p_, at_, n_);
                if (at_ < n_ && p_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < n_ && p_[at_] == ']') {
                    ++at_;
                    break;
                }
                return 0;
            }
            nodes_[arr_idx].count = count;
            nodes_[arr_idx].raw_len = static_cast<uint32_t>(at_ - arr_start);
            --depth_;
            return arr_idx;
        }
        if (c == 't' && n_ - at_ >= 4 && std::memcmp(p_ + at_, "true", 4) == 0) {
            const uint32_t idx = alloc_node(JKind::Bool, static_cast<uint32_t>(at_));
            nodes_[idx].raw_len = 4;
            nodes_[idx].num.i64 = 1;
            at_ += 4;
            return idx;
        }
        if (c == 'f' && n_ - at_ >= 5 && std::memcmp(p_ + at_, "false", 5) == 0) {
            const uint32_t idx = alloc_node(JKind::Bool, static_cast<uint32_t>(at_));
            nodes_[idx].raw_len = 5;
            nodes_[idx].num.i64 = 0;
            at_ += 5;
            return idx;
        }
        if (c == 'n' && n_ - at_ >= 4 && std::memcmp(p_ + at_, "null", 4) == 0) {
            const uint32_t idx = alloc_node(JKind::Null, static_cast<uint32_t>(at_));
            nodes_[idx].raw_len = 4;
            at_ += 4;
            return idx;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return read_number();
        return 0;
    }
};

}  // namespace

bool LspJsonTape::parse(std::string_view src) {
    src_ = {};
    root_ = 0;
    err_ = {};
    if (src.size() > 0x7fffffffu) {
        nodes_.assign(1, TapeNode{});
        err_ = "JSON input exceeds 2 GiB limit";
        return false;
    }
    src_ = src;
    TapeParser tp(src, nodes_, arena_);
    return tp.run(root_, err_);
}

std::string_view LspJsonTape::str_view(uint32_t idx) const {
    if (idx == 0 || idx >= nodes_.size()) return {};
    const TapeNode &nd = nodes_[idx];
    if (nd.kind == JKind::StrRaw) return src_.substr(nd.raw_off, nd.raw_len);
    if (nd.kind == JKind::StrArena) return std::string_view(arena_).substr(nd.first_child, nd.raw_len);
    return {};
}

std::string_view LspJsonTape::raw_json_slice(uint32_t idx) const {
    if (idx == 0 || idx >= nodes_.size()) return "null";
    const TapeNode &nd = nodes_[idx];
    if (nd.kind == JKind::StrRaw) {
        // Include surrounding quotes from `src_`
        if (nd.raw_off > 0 && nd.raw_off + nd.raw_len < src_.size())
            return src_.substr(nd.raw_off - 1, nd.raw_len + 2);
    }
    if (nd.kind == JKind::StrArena) {
        return src_.substr(nd.raw_off, nd.count);
    }
    return src_.substr(nd.raw_off, nd.raw_len);
}

bool LspJsonTape::exact_int32(uint32_t idx, int &out) const {
    if (idx == 0 || idx >= nodes_.size()) return false;
    const TapeNode &nd = nodes_[idx];
    if (nd.kind == JKind::Int) {
        if (nd.num.i64 < std::numeric_limits<int>::min() ||
            nd.num.i64 > std::numeric_limits<int>::max())
            return false;
        out = static_cast<int>(nd.num.i64);
        return true;
    }
    if (nd.kind == JKind::Float) {
        const double d = nd.num.f64;
        if (!std::isfinite(d) || std::trunc(d) != d ||
            d < std::numeric_limits<int>::min() ||
            d > std::numeric_limits<int>::max())
            return false;
        out = static_cast<int>(d);
        return true;
    }
    return false;
}

int LspJsonTape::int_or(uint32_t idx, int fallback) const {
    int v = fallback;
    return exact_int32(idx, v) ? v : fallback;
}

uint32_t LspJsonTape::get(uint32_t obj_idx, std::string_view key) const {
    if (!is_object(obj_idx)) return 0;
    uint32_t cur = nodes_[obj_idx].first_child;
    uint32_t found = 0;
    while (cur != 0) {
        const uint32_t val_idx = nodes_[cur].next_sibling;
        if (val_idx == 0) break;
        if (str_view(cur) == key) found = val_idx;  // last duplicate wins, matching std::map overwrite
        cur = nodes_[val_idx].next_sibling;
    }
    return found;
}

// JsonWriter implementation (SIMD string escaping + std::to_chars).
void JsonWriter::int64(int64_t v) {
    comma_if_needed();
    char tmp[32];
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
    (void)ec;
    buf_.append(tmp, static_cast<size_t>(ptr - tmp));
    need_comma_ = true;
}

void JsonWriter::uint64(uint64_t v) {
    comma_if_needed();
    char tmp[32];
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
    (void)ec;
    buf_.append(tmp, static_cast<size_t>(ptr - tmp));
    need_comma_ = true;
}

void JsonWriter::lsp_range(int l1, int c1, int l2, int c2) {
    begin_obj();
    key_lit("start");
    begin_obj();
    key_lit("line"); int64(l1);
    key_lit("character"); int64(c1);
    end_obj();
    key_lit("end");
    begin_obj();
    key_lit("line"); int64(l2);
    key_lit("character"); int64(c2);
    end_obj();
    end_obj();
}

void JsonWriter::write_escaped_string(std::string &out, std::string_view s) {
    out.push_back('"');
    const char *p = s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const size_t run = simd_scan_json_str_safe(p + i, n - i);
        if (run > 0) {
            out.append(p + i, run);
            i += run;
            if (i >= n) break;
        }
        const auto ch = static_cast<unsigned char>(p[i++]);
        switch (ch) {
        case '"': out.append("\\\"", 2); break;
        case '\\': out.append("\\\\", 2); break;
        case '\b': out.append("\\b", 2); break;
        case '\f': out.append("\\f", 2); break;
        case '\n': out.append("\\n", 2); break;
        case '\r': out.append("\\r", 2); break;
        case '\t': out.append("\\t", 2); break;
        default: {
            static constexpr char hex[] = "0123456789abcdef";
            char esc[6] = {'\\', 'u', '0', '0', hex[(ch >> 4) & 0xf], hex[ch & 0xf]};
            out.append(esc, 6);
            break;
        }
        }
    }
    out.push_back('"');
}

// Pipelined frame reader and single-write frame sender.
bool LspFrameReader::fill_more() {
    if (eof_) return false;
    compact_if_needed();
    char chunk[65536];
#if defined(_WIN32)
    const int got = _read(_fileno(stdin), chunk, sizeof(chunk));
    if (got <= 0) {
        eof_ = true;
        return false;
    }
    rx_.append(chunk, static_cast<size_t>(got));
    return true;
#else
    for (;;) {
        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got > 0) {
            rx_.append(chunk, static_cast<size_t>(got));
            return true;
        }
        if (got == 0) {
            eof_ = true;
            return false;
        }
        if (errno == EINTR) continue;
        eof_ = true;
        return false;
    }
#endif
}

void LspFrameReader::compact_if_needed() {
    if (pos_ == 0) return;
    if (pos_ == rx_.size()) {
        rx_.clear();
        pos_ = 0;
    } else if (pos_ >= 65536 || pos_ * 2 >= rx_.size()) {
        rx_.erase(0, pos_);
        pos_ = 0;
    }
}

FrameStatus LspFrameReader::next_frame(std::string_view &out_body, std::string &out_err) {
    compact_if_needed();
    size_t content_len = 0;
    bool have_len = false;
    size_t header_count = 0;

    for (;;) {
        const size_t nl = rx_.find('\n', pos_);
        if (nl == std::string::npos) {
            if (rx_.size() - pos_ > 8192) {
                out_err = "header line exceeds maximum length";
                return FrameStatus::HeaderError;
            }
            if (!fill_more()) {
                if (pos_ == rx_.size() && !have_len) return FrameStatus::Eof;
                out_err = "missing Content-Length header";
                return FrameStatus::HeaderError;
            }
            continue;
        }
        if (nl - pos_ > 8192) {
            out_err = "header line exceeds maximum length";
            return FrameStatus::HeaderError;
        }
        size_t line_end = nl;
        if (line_end > pos_ && rx_[line_end - 1] == '\r') --line_end;
        const std::string_view line(rx_.data() + pos_, line_end - pos_);
        pos_ = nl + 1;

        if (line.empty()) break;  // end of headers
        if (++header_count > 64) {
            out_err = "too many message headers";
            return FrameStatus::HeaderError;
        }

        const size_t colon = line.find(':');
        if (colon == 0 || colon == std::string_view::npos) {
            out_err = "malformed message header";
            return FrameStatus::HeaderError;
        }
        const std::string_view raw_name = line.substr(0, colon);
        for (unsigned char ch : raw_name) {
            if (!std::isalnum(ch) && ch != '-' && ch != '_') {
                out_err = "malformed message header";
                return FrameStatus::HeaderError;
            }
        }
        bool is_cl = (raw_name.size() == 14);
        if (is_cl) {
            static constexpr char want[] = "content-length";
            for (size_t i = 0; i < 14; ++i) {
                char c = raw_name[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != want[i]) { is_cl = false; break; }
            }
        }
        if (!is_cl) continue;
        if (have_len) {
            out_err = "duplicate Content-Length header";
            return FrameStatus::HeaderError;
        }
        std::string_view val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
        if (val.empty()) {
            out_err = "invalid Content-Length header";
            return FrameStatus::HeaderError;
        }
        const auto parsed = std::from_chars(val.data(), val.data() + val.size(), content_len);
        if (parsed.ec != std::errc{} || parsed.ptr != val.data() + val.size() ||
            content_len == 0 || content_len > kMaxFrameBytes) {
            out_err = "invalid Content-Length header";
            return FrameStatus::HeaderError;
        }
        have_len = true;
    }

    if (!have_len) {
        out_err = "missing Content-Length header";
        return FrameStatus::HeaderError;
    }

    while (rx_.size() - pos_ < content_len) {
        if (!fill_more()) {
            out_err = "truncated JSON-RPC body";
            return FrameStatus::HeaderError;
        }
    }

    out_body = std::string_view(rx_.data() + pos_, content_len);
    pos_ += content_len;
    return FrameStatus::Ok;
}

void send_frame(std::string_view json_body) {
    char hdr[64];
    char *p = hdr;
    std::memcpy(p, "Content-Length: ", 16);
    p += 16;
    auto [endp, ec] = std::to_chars(p, hdr + sizeof(hdr) - 4, json_body.size());
    (void)ec;
    std::memcpy(endp, "\r\n\r\n", 4);
    const size_t hdr_len = static_cast<size_t>((endp + 4) - hdr);
    std::fwrite(hdr, 1, hdr_len, stdout);
    if (!json_body.empty()) std::fwrite(json_body.data(), 1, json_body.size(), stdout);
    std::fflush(stdout);
}

}  // namespace lsp_wire
