#include "pkg_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pkg_io {

namespace {

constexpr std::array<uint32_t, 64> kShaK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcU, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

struct Sha256 {
    uint32_t s[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    uint8_t buf[64]{};
    uint64_t total = 0;

    void block(const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
        uint32_t e = s[4], f = s[5], g = s[6], h = s[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + kShaK[i] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        s[0] += a; s[1] += b; s[2] += c; s[3] += d;
        s[4] += e; s[5] += f; s[6] += g; s[7] += h;
    }

    void update(const uint8_t *data, size_t len) {
        size_t fill = static_cast<size_t>(total & 63u);
        total += len;
        if (fill && fill + len >= 64) {
            const size_t take = 64 - fill;
            std::memcpy(buf + fill, data, take);
            block(buf);
            data += take;
            len -= take;
            fill = 0;
        }
        while (len >= 64) {
            block(data);
            data += 64;
            len -= 64;
        }
        if (len) std::memcpy(buf + fill, data, len);
    }

    std::string hex() {
        const uint64_t bits = total * 8u;
        size_t fill = static_cast<size_t>(total & 63u);
        buf[fill++] = 0x80u;
        if (fill > 56) {
            std::memset(buf + fill, 0, 64 - fill);
            block(buf);
            fill = 0;
        }
        std::memset(buf + fill, 0, 56 - fill);
        for (int i = 0; i < 8; ++i) buf[56 + i] = static_cast<uint8_t>(bits >> ((7 - i) * 8));
        block(buf);
        static const char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                out[i * 8 + j] = kHex[(s[i] >> ((7 - j) * 4)) & 0xFu];
            }
        }
        return out;
    }
};

uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t adler32_calc(const uint8_t *data, size_t len) {
    uint32_t s1 = 1u, s2 = 0u;
    size_t i = 0;
    while (i < len) {
        const size_t chunk = std::min<size_t>(len - i, 5552u);
        for (size_t k = 0; k < chunk; ++k) {
            s1 += data[i + k];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        i += chunk;
    }
    return (s2 << 16) | s1;
}

struct BitReader {
    const uint8_t *src = nullptr;
    size_t len = 0;
    size_t pos = 0;
    uint64_t bitbuf = 0;
    int bitcnt = 0;

    bool ensure(int n) {
        while (bitcnt < n) {
            if (pos >= len) return false;
            bitbuf |= uint64_t(src[pos++]) << bitcnt;
            bitcnt += 8;
        }
        return true;
    }

    uint32_t peek(int n) const {
        return static_cast<uint32_t>(bitbuf & ((uint64_t(1) << n) - 1u));
    }

    void drop(int n) {
        bitbuf >>= n;
        bitcnt -= n;
    }

    bool read_bits(int n, uint32_t &out) {
        if (n == 0) {
            out = 0;
            return true;
        }
        if (!ensure(n)) return false;
        out = peek(n);
        drop(n);
        return true;
    }

    void align_byte() {
        const int rem = bitcnt & 7;
        if (rem) drop(rem);
    }

    size_t byte_pos() const {
        return pos - static_cast<size_t>(bitcnt / 8);
    }
};

struct HuffTree {
    static constexpr int kMaxBits = 15;
    std::array<uint16_t, kMaxBits + 1> counts{};
    std::vector<uint16_t> symbols;
    int max_len = 0;

    bool build(const uint8_t *lengths, size_t num_syms) {
        counts.fill(0);
        max_len = 0;
        size_t total_syms = 0;
        for (size_t i = 0; i < num_syms; ++i) {
            const uint8_t l = lengths[i];
            if (l > kMaxBits) return false;
            if (l > 0) {
                counts[l]++;
                total_syms++;
                if (l > max_len) max_len = l;
            }
        }
        if (total_syms == 0) {
            symbols.clear();
            return true;
        }
        int left = 1;
        for (int b = 1; b <= kMaxBits; ++b) {
            left = (left << 1) - int(counts[b]);
            if (left < 0) return false;
        }
        std::array<uint16_t, kMaxBits + 1> offs{};
        uint16_t acc = 0;
        for (int b = 1; b <= kMaxBits; ++b) {
            offs[b] = acc;
            acc = static_cast<uint16_t>(acc + counts[b]);
        }
        symbols.assign(total_syms, 0);
        for (size_t i = 0; i < num_syms; ++i) {
            const uint8_t l = lengths[i];
            if (l > 0) symbols[offs[l]++] = static_cast<uint16_t>(i);
        }
        return true;
    }

    bool decode(BitReader &br, uint16_t &sym) const {
        if (symbols.empty()) return false;
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= max_len; ++len) {
            uint32_t bit = 0;
            if (!br.read_bits(1, bit)) return false;
            code |= int(bit);
            const int cnt = counts[len];
            if (code - cnt < first) {
                sym = symbols[static_cast<size_t>(index + (code - first))];
                return true;
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return false;
    }
};

}  // namespace

std::string sha256_hex(std::string_view data) {
    Sha256 st;
    if (!data.empty()) st.update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    return st.hex();
}

bool sha256_file(const std::string &path, std::string &hex_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Sha256 st;
    char buf[65536];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize got = in.gcount();
        if (got > 0) st.update(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(got));
    }
    if (in.bad()) return false;
    hex_out = st.hex();
    return true;
}

bool inflate_raw(const uint8_t *src, size_t len, size_t &consumed,
                 std::string &out, size_t max_out, std::string &err) {
    static constexpr uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static constexpr uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr uint8_t kClOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    BitReader br{src, len, 0, 0, 0};
    bool final_block = false;
    while (!final_block) {
        uint32_t bfinal = 0, btype = 0;
        if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) {
            err = "corrupt deflate stream: truncated block header";
            return false;
        }
        final_block = (bfinal != 0);
        if (btype == 0) {
            br.align_byte();
            const size_t bpos = br.byte_pos();
            if (bpos + 4 > len) {
                err = "corrupt deflate stream: truncated stored block";
                return false;
            }
            const uint16_t blen = uint16_t(src[bpos]) | (uint16_t(src[bpos + 1]) << 8);
            const uint16_t nlen = uint16_t(src[bpos + 2]) | (uint16_t(src[bpos + 3]) << 8);
            if ((blen ^ 0xFFFFu) != nlen || bpos + 4 + blen > len) {
                err = "corrupt deflate stream: invalid stored block length";
                return false;
            }
            if (out.size() + blen > max_out) {
                err = "decompressed data exceeds limit";
                return false;
            }
            out.append(reinterpret_cast<const char *>(src + bpos + 4), blen);
            br.pos = bpos + 4 + blen;
            br.bitbuf = 0;
            br.bitcnt = 0;
            continue;
        }
        if (btype != 1 && btype != 2) {
            err = "corrupt deflate stream: reserved block type";
            return false;
        }
        HuffTree lit_tree, dist_tree;
        if (btype == 1) {
            uint8_t ll[288], dl[32];
            for (int i = 0; i <= 143; ++i) ll[i] = 8;
            for (int i = 144; i <= 255; ++i) ll[i] = 9;
            for (int i = 256; i <= 279; ++i) ll[i] = 7;
            for (int i = 280; i <= 287; ++i) ll[i] = 8;
            for (int i = 0; i < 32; ++i) dl[i] = 5;
            lit_tree.build(ll, 288);
            dist_tree.build(dl, 32);
        } else {
            uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!br.read_bits(5, hlit) || !br.read_bits(5, hdist) || !br.read_bits(4, hclen)) {
                err = "corrupt deflate stream: truncated dynamic header";
                return false;
            }
            const size_t nlit = hlit + 257u;
            const size_t ndist = hdist + 1u;
            const size_t nclen = hclen + 4u;
            if (nlit > 286 || ndist > 30) {
                err = "corrupt deflate stream: invalid code count";
                return false;
            }
            uint8_t cl_lens[19]{};
            for (size_t i = 0; i < nclen; ++i) {
                uint32_t v = 0;
                if (!br.read_bits(3, v)) {
                    err = "corrupt deflate stream: truncated code lengths";
                    return false;
                }
                cl_lens[kClOrder[i]] = static_cast<uint8_t>(v);
            }
            HuffTree cl_tree;
            if (!cl_tree.build(cl_lens, 19)) {
                err = "corrupt deflate stream: invalid code-length tree";
                return false;
            }
            std::vector<uint8_t> lens(nlit + ndist, 0);
            size_t idx = 0;
            while (idx < nlit + ndist) {
                uint16_t sym = 0;
                if (!cl_tree.decode(br, sym)) {
                    err = "corrupt deflate stream: invalid code-length symbol";
                    return false;
                }
                if (sym < 16) {
                    lens[idx++] = static_cast<uint8_t>(sym);
                } else {
                    uint32_t rep = 0;
                    uint8_t fill = 0;
                    if (sym == 16) {
                        if (idx == 0 || !br.read_bits(2, rep)) {
                            err = "corrupt deflate stream: bad repeat 16";
                            return false;
                        }
                        rep += 3;
                        fill = lens[idx - 1];
                    } else if (sym == 17) {
                        if (!br.read_bits(3, rep)) {
                            err = "corrupt deflate stream: bad repeat 17";
                            return false;
                        }
                        rep += 3;
                    } else if (sym == 18) {
                        if (!br.read_bits(7, rep)) {
                            err = "corrupt deflate stream: bad repeat 18";
                            return false;
                        }
                        rep += 11;
                    } else {
                        err = "corrupt deflate stream: unknown code-length symbol";
                        return false;
                    }
                    if (idx + rep > lens.size()) {
                        err = "corrupt deflate stream: code-length overflow";
                        return false;
                    }
                    for (uint32_t k = 0; k < rep; ++k) lens[idx++] = fill;
                }
            }
            if (lens[256] == 0 ||
                !lit_tree.build(lens.data(), nlit) ||
                !dist_tree.build(lens.data() + nlit, ndist)) {
                err = "corrupt deflate stream: invalid Huffman table";
                return false;
            }
        }
        while (true) {
            uint16_t sym = 0;
            if (!lit_tree.decode(br, sym)) {
                err = "corrupt deflate stream: invalid literal/length code";
                return false;
            }
            if (sym < 256) {
                if (out.size() + 1 > max_out) {
                    err = "decompressed data exceeds limit";
                    return false;
                }
                out.push_back(static_cast<char>(sym));
            } else if (sym == 256) {
                break;
            } else {
                const uint16_t lidx = sym - 257;
                if (lidx >= 29) {
                    err = "corrupt deflate stream: invalid length symbol";
                    return false;
                }
                uint32_t lextra = 0;
                if (!br.read_bits(kLenExtra[lidx], lextra)) {
                    err = "corrupt deflate stream: truncated length extra";
                    return false;
                }
                const size_t match_len = size_t(kLenBase[lidx]) + lextra;
                uint16_t dsym = 0;
                if (!dist_tree.decode(br, dsym) || dsym >= 30) {
                    err = "corrupt deflate stream: invalid distance symbol";
                    return false;
                }
                uint32_t dextra = 0;
                if (!br.read_bits(kDistExtra[dsym], dextra)) {
                    err = "corrupt deflate stream: truncated distance extra";
                    return false;
                }
                const size_t dist = size_t(kDistBase[dsym]) + dextra;
                if (dist == 0 || dist > out.size()) {
                    err = "corrupt deflate stream: distance beyond window";
                    return false;
                }
                if (out.size() + match_len > max_out) {
                    err = "decompressed data exceeds limit";
                    return false;
                }
                const size_t start = out.size() - dist;
                out.resize(out.size() + match_len);
                for (size_t k = 0; k < match_len; ++k) {
                    out[start + dist + k] = out[start + k];
                }
            }
        }
    }
    br.align_byte();
    consumed = br.byte_pos();
    return true;
}

bool inflate_zlib(const uint8_t *src, size_t len,
                  std::string &out, size_t max_out, std::string &err) {
    if (len < 6) {
        err = "truncated zlib stream";
        return false;
    }
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u || ((cmf >> 4) > 7u) || (((uint16_t(cmf) << 8) | flg) % 31u != 0u) || (flg & 0x20u)) {
        err = "invalid zlib header";
        return false;
    }
    out.clear();
    size_t consumed = 0;
    if (!inflate_raw(src + 2, len - 6, consumed, out, max_out, err)) return false;
    if (2 + consumed + 4 > len) {
        err = "truncated zlib trailer";
        return false;
    }
    const uint8_t *tr = src + 2 + consumed;
    const uint32_t want = (uint32_t(tr[0]) << 24) | (uint32_t(tr[1]) << 16) |
                          (uint32_t(tr[2]) << 8) | uint32_t(tr[3]);
    const uint32_t got = adler32_calc(reinterpret_cast<const uint8_t *>(out.data()), out.size());
    if (want != got) {
        err = "zlib adler32 mismatch";
        return false;
    }
    return true;
}

bool gunzip(std::string_view gz, std::string &out, size_t max_out, std::string &err) {
    out.clear();
    const auto *bytes = reinterpret_cast<const uint8_t *>(gz.data());
    const size_t n = gz.size();
    if (n < 18) {
        err = "corrupt tar.gz archive: truncated gzip header";
        return false;
    }
    size_t pos = 0;
    while (pos < n) {
        if (pos + 18 > n || bytes[pos] != 0x1Fu || bytes[pos + 1] != 0x8Bu || bytes[pos + 2] != 8u) {
            err = "corrupt tar.gz archive: bad gzip magic";
            return false;
        }
        const uint8_t flg = bytes[pos + 3];
        if (flg & 0xE0u) {
            err = "corrupt tar.gz archive: reserved gzip flags";
            return false;
        }
        pos += 10;
        if (flg & 0x04u) {
            if (pos + 2 > n) { err = "corrupt tar.gz archive: truncated FEXTRA"; return false; }
            const uint16_t xlen = uint16_t(bytes[pos]) | (uint16_t(bytes[pos + 1]) << 8);
            pos += 2;
            if (pos + xlen > n) { err = "corrupt tar.gz archive: truncated FEXTRA data"; return false; }
            pos += xlen;
        }
        if (flg & 0x08u) {
            while (pos < n && bytes[pos] != 0) ++pos;
            if (pos >= n) { err = "corrupt tar.gz archive: unterminated FNAME"; return false; }
            ++pos;
        }
        if (flg & 0x10u) {
            while (pos < n && bytes[pos] != 0) ++pos;
            if (pos >= n) { err = "corrupt tar.gz archive: unterminated FCOMMENT"; return false; }
            ++pos;
        }
        if (flg & 0x02u) {
            if (pos + 2 > n) { err = "corrupt tar.gz archive: truncated FHCRC"; return false; }
            pos += 2;
        }
        if (pos + 8 > n) {
            err = "corrupt tar.gz archive: missing gzip trailer";
            return false;
        }
        const size_t start_out = out.size();
        size_t consumed = 0;
        std::string inf_err;
        if (!inflate_raw(bytes + pos, n - pos - 8, consumed, out, max_out, inf_err)) {
            err = (inf_err == "decompressed data exceeds limit")
                      ? "uncompressed archive exceeds 16 MiB"
                      : ("corrupt tar.gz archive: " + inf_err);
            return false;
        }
        pos += consumed;
        if (pos + 8 > n) {
            err = "corrupt tar.gz archive: truncated gzip trailer";
            return false;
        }
        const uint32_t want_crc = uint32_t(bytes[pos]) | (uint32_t(bytes[pos + 1]) << 8) |
                                  (uint32_t(bytes[pos + 2]) << 16) | (uint32_t(bytes[pos + 3]) << 24);
        const uint32_t want_isz = uint32_t(bytes[pos + 4]) | (uint32_t(bytes[pos + 5]) << 8) |
                                  (uint32_t(bytes[pos + 6]) << 16) | (uint32_t(bytes[pos + 7]) << 24);
        pos += 8;
        const size_t member_len = out.size() - start_out;
        const uint32_t got_crc = crc32_calc(reinterpret_cast<const uint8_t *>(out.data() + start_out), member_len);
        if (got_crc != want_crc || uint32_t(member_len) != want_isz) {
            err = "corrupt tar.gz archive: gzip CRC/size mismatch";
            return false;
        }
        while (pos < n && bytes[pos] == 0) ++pos;
    }
    return true;
}

namespace {

bool parse_tar_uint(const uint8_t *field, size_t len, uint64_t &val) {
    if (len > 0 && (field[0] & 0x80u)) {
        if (field[0] != 0x80u) return false;
        uint64_t acc = 0;
        for (size_t i = 1; i < len; ++i) {
            if (acc > (UINT64_MAX >> 8)) return false;
            acc = (acc << 8) | field[i];
        }
        val = acc;
        return true;
    }
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
    uint64_t acc = 0;
    for (; i < len; ++i) {
        const uint8_t c = field[i];
        if (c == ' ' || c == '\0') break;
        if (c < '0' || c > '7') return false;
        if (acc > (UINT64_MAX >> 3)) return false;
        acc = (acc << 3) | uint64_t(c - '0');
    }
    val = acc;
    return true;
}

std::string tar_cstr(const uint8_t *p, size_t max_len) {
    size_t n = 0;
    while (n < max_len && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char *>(p), n);
}

bool parse_pax_records(std::string_view data,
                       std::string &pax_path,
                       bool &has_linkpath,
                       int64_t &pax_size) {
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t sp = data.find(' ', pos);
        if (sp == std::string_view::npos) return false;
        size_t rec_len = 0;
        for (size_t i = pos; i < sp; ++i) {
            if (data[i] < '0' || data[i] > '9') return false;
            rec_len = rec_len * 10 + size_t(data[i] - '0');
            if (rec_len > data.size() - pos) return false;
        }
        if (rec_len <= (sp - pos + 1) || pos + rec_len > data.size() || data[pos + rec_len - 1] != '\n')
            return false;
        const std::string_view kv = data.substr(sp + 1, rec_len - (sp - pos + 1) - 1);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view k = kv.substr(0, eq);
            const std::string_view v = kv.substr(eq + 1);
            if (k == "path") pax_path = std::string(v);
            else if (k == "linkpath" && !v.empty()) has_linkpath = true;
            else if (k == "size") {
                uint64_t sz = 0;
                for (char ch : v) {
                    if (ch < '0' || ch > '9') return false;
                    sz = sz * 10 + uint64_t(ch - '0');
                    if (sz > kMaxTarStreamBytes) return false;
                }
                pax_size = static_cast<int64_t>(sz);
            }
        }
        pos += rec_len;
    }
    return true;
}

}  // namespace

bool extract_targz_raw(std::string_view gz_bytes,
                       std::vector<TarMember> &members,
                       std::string &err,
                       size_t max_payload_bytes,
                       size_t max_entries) {
    members.clear();
    std::string tar;
    if (!gunzip(gz_bytes, tar, kMaxTarStreamBytes, err)) return false;

    const auto *buf = reinterpret_cast<const uint8_t *>(tar.data());
    const size_t n = tar.size();
    size_t pos = 0;
    size_t entry_count = 0;
    size_t total_payload = 0;
    std::set<std::string> seen_paths;

    std::string global_pax_path;
    std::string next_longname;
    std::string next_pax_path;
    bool next_has_link = false;
    int64_t next_pax_size = -1;

    while (pos + 512 <= n) {
        const uint8_t *hdr = buf + pos;
        bool all_zero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (hdr[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) {
            pos += 512;
            continue;
        }
        uint64_t want_sum = 0;
        if (!parse_tar_uint(hdr + 148, 8, want_sum)) {
            err = "corrupt tar.gz archive: invalid tar header checksum";
            return false;
        }
        uint32_t unsigned_sum = 0;
        int32_t signed_sum = 0;
        for (size_t i = 0; i < 512; ++i) {
            const uint8_t b = (i >= 148 && i < 156) ? uint8_t(' ') : hdr[i];
            unsigned_sum += b;
            signed_sum += static_cast<int8_t>(b);
        }
        if (want_sum != unsigned_sum && int64_t(want_sum) != int64_t(signed_sum)) {
            err = "corrupt tar.gz archive: tar header checksum mismatch";
            return false;
        }
        uint64_t raw_size = 0;
        if (!parse_tar_uint(hdr + 124, 12, raw_size)) {
            err = "corrupt tar.gz archive: invalid member size";
            return false;
        }
        if (next_pax_size >= 0) raw_size = static_cast<uint64_t>(next_pax_size);
        const char typeflag = hdr[156] ? static_cast<char>(hdr[156]) : '0';

        pos += 512;
        if (raw_size > n - pos) {
            err = "corrupt tar.gz archive: truncated member payload";
            return false;
        }
        const std::string_view payload(tar.data() + pos, static_cast<size_t>(raw_size));
        const size_t padded = (static_cast<size_t>(raw_size) + 511u) & ~size_t(511u);
        pos += std::min(padded, n - pos);

        if (typeflag == 'L') {
            size_t ln = payload.size();
            while (ln > 0 && payload[ln - 1] == '\0') --ln;
            next_longname.assign(payload.data(), ln);
            continue;
        }
        if (typeflag == 'K') {
            err = "archive may not contain symbolic or hard links";
            return false;
        }
        if (typeflag == 'x' || typeflag == 'g') {
            std::string ppath;
            bool plink = false;
            int64_t psz = -1;
            if (!parse_pax_records(payload, ppath, plink, psz)) {
                err = "corrupt tar.gz archive: malformed PAX header";
                return false;
            }
            if (plink) {
                err = "archive may not contain symbolic or hard links";
                return false;
            }
            if (typeflag == 'g') global_pax_path = ppath;
            else {
                next_pax_path = ppath;
                next_has_link = plink;
                next_pax_size = psz;
            }
            continue;
        }

        std::string name;
        if (!next_pax_path.empty()) name = next_pax_path;
        else if (!next_longname.empty()) name = next_longname;
        else if (!global_pax_path.empty()) name = global_pax_path;
        else {
            const std::string base = tar_cstr(hdr, 100);
            const bool is_ustar = std::memcmp(hdr + 257, "ustar", 5) == 0;
            const std::string pref = is_ustar ? tar_cstr(hdr + 345, 155) : std::string();
            name = pref.empty() ? base : (pref + "/" + base);
        }
        const bool had_link = next_has_link;
        next_longname.clear();
        next_pax_path.clear();
        next_has_link = false;
        next_pax_size = -1;

        ++entry_count;
        if (entry_count > max_entries) {
            err = "archive has too many entries (>512)";
            return false;
        }
        if (had_link || typeflag == '1' || typeflag == '2') {
            err = "archive may not contain symbolic or hard links (" + name + ")";
            return false;
        }
        if (typeflag == '5' || (!name.empty() && name.back() == '/' && raw_size == 0)) {
            continue;
        }
        if (typeflag != '0' && typeflag != '7') {
            err = "archive contains unsupported entry type (" + name + ")";
            return false;
        }
        if (name.find('\0') != std::string::npos) {
            err = "unsafe path in archive: " + name;
            return false;
        }
        std::string norm = name;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        while (norm.rfind("./", 0) == 0) norm.erase(0, 2);
        if (norm.empty() || norm.front() == '/' ||
            (norm.size() >= 2 && std::isalpha(static_cast<unsigned char>(norm[0])) && norm[1] == ':')) {
            err = "path traversal in archive: " + name;
            return false;
        }
        std::vector<std::string> segs;
        size_t s = 0;
        while (s <= norm.size()) {
            const size_t slash = norm.find('/', s);
            const std::string seg = (slash == std::string::npos) ? norm.substr(s) : norm.substr(s, slash - s);
            if (seg.empty() || seg == "." || seg == "..") {
                err = "path traversal in archive: " + name;
                return false;
            }
            segs.push_back(seg);
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
        std::string canon;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i) canon.push_back('/');
            canon += segs[i];
        }
        if (!seen_paths.insert(canon).second) {
            err = "duplicate file in archive: " + canon;
            return false;
        }
        if (raw_size > max_payload_bytes || total_payload + raw_size > max_payload_bytes) {
            err = "uncompressed archive exceeds 16 MiB";
            return false;
        }
        total_payload += static_cast<size_t>(raw_size);
        members.push_back(TarMember{name, std::move(segs), std::string(payload)});
    }
    if (pos != n) {
        err = "corrupt tar.gz archive: trailing partial block";
        return false;
    }
    return true;
}

namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_nibble(s[i + 1]);
            const int lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string target;
};

bool parse_http_url(std::string_view url, ParsedUrl &out, std::string &err) {
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos) {
        err = "invalid URL " + std::string(url);
        return false;
    }
    out.scheme = std::string(url.substr(0, sep));
    for (char &c : out.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (out.scheme != "http" && out.scheme != "https") {
        err = "unsupported URL scheme in " + std::string(url);
        return false;
    }
    std::string_view rest = url.substr(sep + 3);
    const size_t path_pos = rest.find_first_of("/?#");
    std::string_view authority = (path_pos == std::string_view::npos) ? rest : rest.substr(0, path_pos);
    std::string_view path_part = (path_pos == std::string_view::npos) ? std::string_view("/") : rest.substr(path_pos);
    const size_t hash_pos = path_part.find('#');
    if (hash_pos != std::string_view::npos) path_part = path_part.substr(0, hash_pos);
    out.target = path_part.empty() ? "/" : std::string(path_part);
    if (out.target.front() != '/') out.target.insert(out.target.begin(), '/');

    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        err = "invalid URL authority in " + std::string(url);
        return false;
    }
    out.port = (out.scheme == "https") ? 443 : 80;
    if (authority.front() == '[') {
        const size_t rb = authority.find(']');
        if (rb == std::string_view::npos) {
            err = "unclosed IPv6 literal in " + std::string(url);
            return false;
        }
        out.host = std::string(authority.substr(1, rb - 1));
        if (rb + 1 < authority.size()) {
            if (authority[rb + 1] != ':') {
                err = "invalid port in " + std::string(url);
                return false;
            }
            const std::string_view ps = authority.substr(rb + 2);
            if (ps.empty()) { err = "empty port in " + std::string(url); return false; }
            uint32_t p = 0;
            for (char c : ps) {
                if (c < '0' || c > '9') { err = "invalid port in " + std::string(url); return false; }
                p = p * 10 + uint32_t(c - '0');
                if (p > 65535) { err = "port out of range in " + std::string(url); return false; }
            }
            if (p == 0) { err = "port out of range in " + std::string(url); return false; }
            out.port = static_cast<uint16_t>(p);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            out.host = std::string(authority.substr(0, colon));
            const std::string_view ps = authority.substr(colon + 1);
            if (ps.empty()) { err = "empty port in " + std::string(url); return false; }
            uint32_t p = 0;
            for (char c : ps) {
                if (c < '0' || c > '9') { err = "invalid port in " + std::string(url); return false; }
                p = p * 10 + uint32_t(c - '0');
                if (p > 65535) { err = "port out of range in " + std::string(url); return false; }
            }
            if (p == 0) { err = "port out of range in " + std::string(url); return false; }
            out.port = static_cast<uint16_t>(p);
        } else {
            out.host = std::string(authority);
        }
    }
    if (out.host.empty()) {
        err = "missing host in " + std::string(url);
        return false;
    }
    for (char c : out.host) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7f) {
            err = "unsafe host in " + std::string(url);
            return false;
        }
    }
    for (char c : out.target) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7f) {
            err = "unsafe path in " + std::string(url);
            return false;
        }
    }
    return true;
}

std::string resolve_redirect_url(const ParsedUrl &base, std::string_view loc) {
    if (loc.find("://") != std::string_view::npos) return std::string(loc);
    const bool def_port = (base.scheme == "http" && base.port == 80) ||
                          (base.scheme == "https" && base.port == 443);
    const std::string host_fmt = (base.host.find(':') != std::string::npos)
                                     ? ("[" + base.host + "]")
                                     : base.host;
    const std::string origin = base.scheme + "://" + host_fmt +
                               (def_port ? "" : (":" + std::to_string(base.port)));
    if (loc.rfind("//", 0) == 0) return base.scheme + ":" + std::string(loc);
    if (!loc.empty() && loc.front() == '/') return origin + std::string(loc);
    std::string dir = base.target;
    const size_t q = dir.find('?');
    if (q != std::string::npos) dir.resize(q);
    const size_t sl = dir.rfind('/');
    dir = (sl == std::string::npos) ? "/" : dir.substr(0, sl + 1);
    return origin + dir + std::string(loc);
}

void classify_ipv4_host_order(uint32_t ip, bool &non_public, bool &always_blocked) {
    if ((ip >> 24) == 0u || (ip >> 16) == 0xA9FEu || ip == 0xFFFFFFFFu) {
        always_blocked = true;
        non_public = true;
        return;
    }
    if ((ip >> 24) == 127u || (ip >> 24) == 10u ||
        (ip & 0xFFF00000u) == 0xAC100000u ||
        (ip >> 16) == 0xC0A8u ||
        (ip & 0xFFC00000u) == 0x64400000u ||
        (ip >> 28) >= 14u) {
        non_public = true;
    }
}

void classify_sockaddr(const struct sockaddr *sa, bool &non_public, bool &always_blocked) {
    non_public = false;
    always_blocked = false;
    if (!sa) return;
    if (sa->sa_family == AF_INET) {
        const auto *s4 = reinterpret_cast<const struct sockaddr_in *>(sa);
        classify_ipv4_host_order(ntohl(s4->sin_addr.s_addr), non_public, always_blocked);
    } else if (sa->sa_family == AF_INET6) {
        const auto *s6 = reinterpret_cast<const struct sockaddr_in6 *>(sa);
        const uint8_t *b = s6->sin6_addr.s6_addr;
        bool z0_9 = true;
        for (int i = 0; i < 10; ++i) if (b[i] != 0) z0_9 = false;
        if (z0_9 && b[10] == 0xff && b[11] == 0xff) {
            uint32_t v4 = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) |
                          (uint32_t(b[14]) << 8) | uint32_t(b[15]);
            classify_ipv4_host_order(v4, non_public, always_blocked);
            return;
        }
        bool z0_14 = z0_9 && b[10] == 0 && b[11] == 0 && b[12] == 0 && b[13] == 0 && b[14] == 0;
        if (z0_14 && b[15] == 0) {
            always_blocked = true;
            non_public = true;
            return;
        }
        if ((z0_14 && b[15] == 1) || (b[0] & 0xfe) == 0xfc ||
            (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || b[0] == 0xff) {
            non_public = true;
        }
    }
}

#if defined(_WIN32)

bool fetch_http_single(const ParsedUrl &u,
                       bool block_internal,
                       bool *connected_internal_out,
                       int &status_out,
                       std::string &location_out,
                       std::string &body_out,
                       size_t max_bytes,
                       std::string &err) {
    status_out = 0;
    location_out.clear();
    body_out.clear();

    static std::once_flag wsa_once;
    static bool wsa_ok = false;
    std::call_once(wsa_once, [] {
        WSADATA wd {};
        wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    });
    if (!wsa_ok) {
        err = "Winsock initialization failed";
        return false;
    }

    const char *allow_env = std::getenv("BRAT_PKG_ALLOW_INTERNAL");
    const bool allow_internal = allow_env && std::strcmp(allow_env, "1") == 0;

    struct addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = nullptr;
    const std::string port_s = std::to_string(u.port);
    if (getaddrinfo(u.host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        err = "DNS lookup failed for " + u.host;
        return false;
    }
    bool any_internal = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        bool non_pub = false, always_blk = false;
        classify_sockaddr(ai->ai_addr, non_pub, always_blk);
        if (non_pub) any_internal = true;
        if (!allow_internal) {
            if (always_blk) {
                freeaddrinfo(res);
                err = "refusing connection to link-local/unspecified address for " + u.host;
                return false;
            }
            if (block_internal && non_pub) {
                freeaddrinfo(res);
                err = "refusing redirect to private/loopback/link-local address for " + u.host;
                return false;
            }
        }
    }
    freeaddrinfo(res);

    struct WinHttpConnInfo {
        DWORD cbSize;
        SOCKADDR_STORAGE LocalAddress;
        SOCKADDR_STORAGE RemoteAddress;
    };
    struct WinHttpApi {
        using HINTERNET = void *;
        HINTERNET(WINAPI *Open)(const wchar_t *, DWORD, const wchar_t *, const wchar_t *, DWORD) = nullptr;
        HINTERNET(WINAPI *Connect)(HINTERNET, const wchar_t *, WORD, DWORD) = nullptr;
        HINTERNET(WINAPI *OpenRequest)(HINTERNET, const wchar_t *, const wchar_t *, const wchar_t *,
                                       const wchar_t *, const wchar_t **, DWORD) = nullptr;
        BOOL(WINAPI *SetOption)(HINTERNET, DWORD, void *, DWORD) = nullptr;
        BOOL(WINAPI *QueryOption)(HINTERNET, DWORD, void *, DWORD *) = nullptr;
        BOOL(WINAPI *SetTimeouts)(HINTERNET, int, int, int, int) = nullptr;
        BOOL(WINAPI *SendRequest)(HINTERNET, const wchar_t *, DWORD, void *, DWORD, DWORD, DWORD_PTR) = nullptr;
        BOOL(WINAPI *ReceiveResponse)(HINTERNET, void *) = nullptr;
        BOOL(WINAPI *QueryHeaders)(HINTERNET, DWORD, const wchar_t *, void *, DWORD *, DWORD *) = nullptr;
        BOOL(WINAPI *ReadData)(HINTERNET, void *, DWORD, DWORD *) = nullptr;
        BOOL(WINAPI *CloseHandle)(HINTERNET) = nullptr;
        bool ready = false;
    };
    static WinHttpApi wapi;
    static std::once_flag wonce;
    std::call_once(wonce, [] {
        HMODULE mod = LoadLibraryExA("winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mod) return;
        auto sym = [mod](const char *name) -> void * {
            return reinterpret_cast<void *>(GetProcAddress(mod, name));
        };
        wapi.Open = reinterpret_cast<decltype(wapi.Open)>(sym("WinHttpOpen"));
        wapi.Connect = reinterpret_cast<decltype(wapi.Connect)>(sym("WinHttpConnect"));
        wapi.OpenRequest = reinterpret_cast<decltype(wapi.OpenRequest)>(sym("WinHttpOpenRequest"));
        wapi.SetOption = reinterpret_cast<decltype(wapi.SetOption)>(sym("WinHttpSetOption"));
        wapi.QueryOption = reinterpret_cast<decltype(wapi.QueryOption)>(sym("WinHttpQueryOption"));
        wapi.SetTimeouts = reinterpret_cast<decltype(wapi.SetTimeouts)>(sym("WinHttpSetTimeouts"));
        wapi.SendRequest = reinterpret_cast<decltype(wapi.SendRequest)>(sym("WinHttpSendRequest"));
        wapi.ReceiveResponse = reinterpret_cast<decltype(wapi.ReceiveResponse)>(sym("WinHttpReceiveResponse"));
        wapi.QueryHeaders = reinterpret_cast<decltype(wapi.QueryHeaders)>(sym("WinHttpQueryHeaders"));
        wapi.ReadData = reinterpret_cast<decltype(wapi.ReadData)>(sym("WinHttpReadData"));
        wapi.CloseHandle = reinterpret_cast<decltype(wapi.CloseHandle)>(sym("WinHttpCloseHandle"));
        wapi.ready = wapi.Open && wapi.Connect && wapi.OpenRequest && wapi.SetOption &&
                     wapi.QueryOption && wapi.SetTimeouts && wapi.SendRequest &&
                     wapi.ReceiveResponse && wapi.QueryHeaders && wapi.ReadData && wapi.CloseHandle;
    });
    if (!wapi.ready) {
        err = "winhttp.dll unavailable";
        return false;
    }
    auto to_wide = [](const std::string &s) {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    void *hsess = wapi.Open(L"brat-pkg/0.7", 0, nullptr, nullptr, 0);
    if (!hsess) { err = "WinHttpOpen failed"; return false; }
    wapi.SetTimeouts(hsess, 15000, 15000, 30000, 30000);
    const std::wstring whost = to_wide(u.host);
    const std::wstring wtarget = to_wide(u.target);
    void *hconn = wapi.Connect(hsess, whost.c_str(), u.port, 0);
    if (!hconn) { wapi.CloseHandle(hsess); err = "WinHttpConnect failed"; return false; }
    const DWORD req_flags = (u.scheme == "https") ? 0x00800000u /* WINHTTP_FLAG_SECURE */ : 0u;
    void *hreq = wapi.OpenRequest(hconn, L"GET", wtarget.c_str(), nullptr, nullptr, nullptr, req_flags);
    if (!hreq) {
        wapi.CloseHandle(hconn); wapi.CloseHandle(hsess);
        err = "WinHttpOpenRequest failed";
        return false;
    }
    auto close_all = [&]() {
        wapi.CloseHandle(hreq);
        wapi.CloseHandle(hconn);
        wapi.CloseHandle(hsess);
    };
    // WINHTTP_OPTION_DISABLE_FEATURE = 63, WINHTTP_DISABLE_REDIRECTS = 2
    DWORD dis_redir = 0x00000002u;
    if (!wapi.SetOption(hreq, 63u, &dis_redir, sizeof(dis_redir))) {
        close_all();
        err = "WinHttpSetOption(DISABLE_REDIRECTS) failed";
        return false;
    }
    if (!wapi.SendRequest(hreq, nullptr, 0, nullptr, 0, 0, 0) || !wapi.ReceiveResponse(hreq, nullptr)) {
        close_all();
        err = "WinHTTP request failed";
        return false;
    }
    // Re-check actual connected peer address via WINHTTP_OPTION_CONNECTION_INFO (73) against DNS rebinding.
    WinHttpConnInfo ci {};
    ci.cbSize = sizeof(ci);
    DWORD ci_len = sizeof(ci);
    if (wapi.QueryOption(hreq, 73u, &ci, &ci_len)) {
        bool peer_non_pub = false, peer_always_blk = false;
        classify_sockaddr(reinterpret_cast<const struct sockaddr *>(&ci.RemoteAddress),
                          peer_non_pub, peer_always_blk);
        if (peer_non_pub) any_internal = true;
        if (!allow_internal && (peer_always_blk || (block_internal && peer_non_pub))) {
            close_all();
            err = "refusing connection to non-public peer address for " + u.host;
            return false;
        }
    }
    if (connected_internal_out) *connected_internal_out = any_internal;

    DWORD status_code = 0, sc_sz = sizeof(status_code);
    if (!wapi.QueryHeaders(hreq, 19u | 0x20000000u, nullptr, &status_code, &sc_sz, nullptr)) {
        close_all();
        err = "WinHttpQueryHeaders(status) failed";
        return false;
    }
    status_out = static_cast<int>(status_code);
    if (status_out == 301 || status_out == 302 || status_out == 303 ||
        status_out == 307 || status_out == 308) {
        DWORD loc_bytes = 0;
        // WINHTTP_QUERY_LOCATION = 33
        wapi.QueryHeaders(hreq, 33u, nullptr, nullptr, &loc_bytes, nullptr);
        if (loc_bytes > 0 && loc_bytes < 16384u) {
            std::wstring wloc(loc_bytes / sizeof(wchar_t) + 1, L'\0');
            if (wapi.QueryHeaders(hreq, 33u, nullptr, wloc.data(), &loc_bytes, nullptr)) {
                const int un = WideCharToMultiByte(CP_UTF8, 0, wloc.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (un > 1) {
                    location_out.resize(static_cast<size_t>(un - 1));
                    WideCharToMultiByte(CP_UTF8, 0, wloc.c_str(), -1, location_out.data(), un, nullptr, nullptr);
                }
            }
        }
        close_all();
        return true;
    }
    char wbuf[16384];
    while (true) {
        DWORD got = 0;
        if (!wapi.ReadData(hreq, wbuf, sizeof(wbuf), &got)) {
            close_all();
            err = "WinHttpReadData failed";
            return false;
        }
        if (got == 0) break;
        if (body_out.size() + got > max_bytes) {
            close_all();
            err = "package exceeds 16 MiB";
            return false;
        }
        body_out.append(wbuf, got);
    }
    close_all();
    return true;
}

#else

struct OpenSslApi {
    bool ready = false;
    const void *(*TLS_client_method)(void) = nullptr;
    void *(*SSL_CTX_new)(const void *) = nullptr;
    void (*SSL_CTX_free)(void *) = nullptr;
    int (*SSL_CTX_set_default_verify_paths)(void *) = nullptr;
    int (*SSL_CTX_load_verify_locations)(void *, const char *, const char *) = nullptr;
    void (*SSL_CTX_set_verify)(void *, int, void *) = nullptr;
    void *(*SSL_new)(void *) = nullptr;
    void (*SSL_free)(void *) = nullptr;
    int (*SSL_set_fd)(void *, int) = nullptr;
    long (*SSL_ctrl)(void *, int, long, void *) = nullptr;
    int (*SSL_set1_host)(void *, const char *) = nullptr;
    void *(*SSL_get0_param)(void *) = nullptr;
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void *, const char *) = nullptr;
    int (*SSL_connect)(void *) = nullptr;
    long (*SSL_get_verify_result)(const void *) = nullptr;
    int (*SSL_write)(void *, const void *, int) = nullptr;
    int (*SSL_read)(void *, void *, int) = nullptr;
    int (*SSL_shutdown)(void *) = nullptr;
};

const OpenSslApi &load_openssl() {
    static OpenSslApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        static const char *const kLibs[] = {
            "libssl.so.3", "libssl.so.1.1", "libssl.so",
            "libssl.3.dylib", "libssl.1.1.dylib", "libssl.dylib",
        };
        static const char *const kCryLibs[] = {
            "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so",
            "libcrypto.3.dylib", "libcrypto.1.1.dylib", "libcrypto.dylib",
        };
        void *h = nullptr;
        for (const char *nm : kLibs) {
            h = dlopen(nm, RTLD_LAZY | RTLD_LOCAL);
            if (h) break;
        }
        if (!h) return;
        void *hc = nullptr;
        for (const char *nm : kCryLibs) {
            hc = dlopen(nm, RTLD_LAZY | RTLD_LOCAL);
            if (hc) break;
        }
        auto sym = [h](const char *s) { return dlsym(h, s); };
        api.TLS_client_method = reinterpret_cast<decltype(api.TLS_client_method)>(
            sym("TLS_client_method") ? sym("TLS_client_method") : sym("SSLv23_client_method"));
        api.SSL_CTX_new = reinterpret_cast<decltype(api.SSL_CTX_new)>(sym("SSL_CTX_new"));
        api.SSL_CTX_free = reinterpret_cast<decltype(api.SSL_CTX_free)>(sym("SSL_CTX_free"));
        api.SSL_CTX_set_default_verify_paths =
            reinterpret_cast<decltype(api.SSL_CTX_set_default_verify_paths)>(sym("SSL_CTX_set_default_verify_paths"));
        api.SSL_CTX_load_verify_locations =
            reinterpret_cast<decltype(api.SSL_CTX_load_verify_locations)>(sym("SSL_CTX_load_verify_locations"));
        api.SSL_CTX_set_verify = reinterpret_cast<decltype(api.SSL_CTX_set_verify)>(sym("SSL_CTX_set_verify"));
        api.SSL_new = reinterpret_cast<decltype(api.SSL_new)>(sym("SSL_new"));
        api.SSL_free = reinterpret_cast<decltype(api.SSL_free)>(sym("SSL_free"));
        api.SSL_set_fd = reinterpret_cast<decltype(api.SSL_set_fd)>(sym("SSL_set_fd"));
        api.SSL_ctrl = reinterpret_cast<decltype(api.SSL_ctrl)>(sym("SSL_ctrl"));
        api.SSL_set1_host = reinterpret_cast<decltype(api.SSL_set1_host)>(sym("SSL_set1_host"));
        api.SSL_get0_param = reinterpret_cast<decltype(api.SSL_get0_param)>(sym("SSL_get0_param"));
        void *ip_sym = sym("X509_VERIFY_PARAM_set1_ip_asc");
        if (!ip_sym && hc) ip_sym = dlsym(hc, "X509_VERIFY_PARAM_set1_ip_asc");
        api.X509_VERIFY_PARAM_set1_ip_asc =
            reinterpret_cast<decltype(api.X509_VERIFY_PARAM_set1_ip_asc)>(ip_sym);
        api.SSL_connect = reinterpret_cast<decltype(api.SSL_connect)>(sym("SSL_connect"));
        api.SSL_get_verify_result = reinterpret_cast<decltype(api.SSL_get_verify_result)>(sym("SSL_get_verify_result"));
        api.SSL_write = reinterpret_cast<decltype(api.SSL_write)>(sym("SSL_write"));
        api.SSL_read = reinterpret_cast<decltype(api.SSL_read)>(sym("SSL_read"));
        api.SSL_shutdown = reinterpret_cast<decltype(api.SSL_shutdown)>(sym("SSL_shutdown"));
        api.ready = api.TLS_client_method && api.SSL_CTX_new && api.SSL_CTX_free &&
                    api.SSL_CTX_set_default_verify_paths && api.SSL_CTX_set_verify &&
                    api.SSL_new && api.SSL_free && api.SSL_set_fd && api.SSL_ctrl &&
                    api.SSL_set1_host && api.SSL_connect && api.SSL_get_verify_result &&
                    api.SSL_write && api.SSL_read;
    });
    return api;
}

int connect_with_timeout(const std::string &host,
                         uint16_t port,
                         int timeout_ms,
                         std::string &err,
                         bool block_internal,
                         bool *connected_internal_out) {
    struct addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = nullptr;
    const std::string port_s = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        err = "DNS lookup failed for " + host + ": " + (gai ? gai_strerror(gai) : "no addresses");
        return -1;
    }
    const char *allow_env = std::getenv("BRAT_PKG_ALLOW_INTERNAL");
    const bool allow_internal = allow_env && std::strcmp(allow_env, "1") == 0;
    int sock = -1;
    bool blocked_policy = false;
    std::string policy_err;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        bool non_pub = false, always_blk = false;
        classify_sockaddr(ai->ai_addr, non_pub, always_blk);
        if (!allow_internal) {
            if (always_blk) {
                blocked_policy = true;
                policy_err = "refusing connection to link-local/unspecified address for " + host;
                continue;
            }
            if (block_internal && non_pub) {
                blocked_policy = true;
                policy_err = "refusing redirect to private/loopback/link-local address for " + host;
                continue;
            }
        }
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            rc = poll(&pfd, 1, timeout_ms);
            if (rc > 0) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) rc = 0;
                else rc = -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            if (flags >= 0) fcntl(fd, F_SETFL, flags);
            struct timeval tv {};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connected_internal_out) *connected_internal_out = non_pub;
            sock = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);
    if (sock < 0) {
        err = blocked_policy ? policy_err : ("cannot connect to " + host + ":" + port_s);
    }
    return sock;
}

bool decode_chunked_body(std::string_view raw, std::string &out, size_t max_bytes, std::string &err) {
    out.clear();
    size_t pos = 0;
    while (true) {
        const size_t eol = raw.find("\r\n", pos);
        if (eol == std::string_view::npos) {
            err = "truncated chunked HTTP response";
            return false;
        }
        std::string_view line = raw.substr(pos, eol - pos);
        const size_t semi = line.find(';');
        if (semi != std::string_view::npos) line = line.substr(0, semi);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
        if (line.empty()) {
            err = "malformed chunk size in HTTP response";
            return false;
        }
        size_t csz = 0;
        for (char c : line) {
            const int nib = hex_nibble(c);
            if (nib < 0 || csz > (max_bytes >> 4) + 16) {
                err = "invalid chunk size in HTTP response";
                return false;
            }
            csz = (csz << 4) | size_t(nib);
        }
        pos = eol + 2;
        if (csz == 0) {
            if (raw.find("\r\n", pos) == std::string_view::npos) {
                err = "unterminated final chunk in HTTP response";
                return false;
            }
            return true;
        }
        if (out.size() + csz > max_bytes) {
            err = "package exceeds 16 MiB";
            return false;
        }
        if (pos + csz + 2 > raw.size() || raw.substr(pos + csz, 2) != "\r\n") {
            err = "truncated chunk payload in HTTP response";
            return false;
        }
        out.append(raw.data() + pos, csz);
        pos += csz + 2;
    }
}

bool fetch_http_single(const ParsedUrl &u,
                       bool block_internal,
                       bool *connected_internal_out,
                       int &status_out,
                       std::string &location_out,
                       std::string &body_out,
                       size_t max_bytes,
                       std::string &err) {
    status_out = 0;
    location_out.clear();
    body_out.clear();

    const int fd = connect_with_timeout(u.host, u.port, 30000, err, block_internal, connected_internal_out);
    if (fd < 0) return false;

    void *ssl_ctx = nullptr;
    void *ssl = nullptr;
    const OpenSslApi *ossl = nullptr;
    auto cleanup = [&]() {
        if (ssl && ossl) {
            if (ossl->SSL_shutdown) ossl->SSL_shutdown(ssl);
            ossl->SSL_free(ssl);
        }
        if (ssl_ctx && ossl) ossl->SSL_CTX_free(ssl_ctx);
        close(fd);
    };

    if (u.scheme == "https") {
        ossl = &load_openssl();
        if (!ossl->ready) {
            cleanup();
            err = "TLS unavailable: libssl not found";
            return false;
        }
        ssl_ctx = ossl->SSL_CTX_new(ossl->TLS_client_method());
        if (!ssl_ctx) {
            cleanup();
            err = "TLS initialization failed";
            return false;
        }
        ossl->SSL_CTX_set_default_verify_paths(ssl_ctx);
        const char *ca_file = std::getenv("SSL_CERT_FILE");
        const char *ca_dir = std::getenv("SSL_CERT_DIR");
        if ((ca_file && *ca_file) || (ca_dir && *ca_dir)) {
            if (ossl->SSL_CTX_load_verify_locations)
                ossl->SSL_CTX_load_verify_locations(ssl_ctx, (ca_file && *ca_file) ? ca_file : nullptr,
                                                    (ca_dir && *ca_dir) ? ca_dir : nullptr);
        }
        ossl->SSL_CTX_set_verify(ssl_ctx, 1 /* SSL_VERIFY_PEER */, nullptr);
        ssl = ossl->SSL_new(ssl_ctx);
        if (!ssl) {
            cleanup();
            err = "TLS session allocation failed";
            return false;
        }
        ossl->SSL_set_fd(ssl, fd);
        struct in_addr ip4 {};
        struct in6_addr ip6 {};
        const bool is_ip = (inet_pton(AF_INET, u.host.c_str(), &ip4) == 1) ||
                           (inet_pton(AF_INET6, u.host.c_str(), &ip6) == 1);
        if (!is_ip) {
            // SSL_CTRL_SET_TLSEXT_HOSTNAME = 55, TLSEXT_NAMETYPE_host_name = 0
            ossl->SSL_ctrl(ssl, 55, 0, const_cast<char *>(u.host.c_str()));
        }
        bool host_ok = false;
        if (is_ip) {
            if (ossl->SSL_get0_param && ossl->X509_VERIFY_PARAM_set1_ip_asc) {
                void *vp = ossl->SSL_get0_param(ssl);
                if (vp && ossl->X509_VERIFY_PARAM_set1_ip_asc(vp, u.host.c_str()) == 1)
                    host_ok = true;
            }
        } else {
            if (ossl->SSL_set1_host(ssl, u.host.c_str()) == 1) host_ok = true;
        }
        if (!host_ok) {
            cleanup();
            err = "TLS peer identity setup failed for " + u.host;
            return false;
        }
        if (ossl->SSL_connect(ssl) != 1 || ossl->SSL_get_verify_result(ssl) != 0) {
            cleanup();
            err = "TLS handshake or certificate verification failed for " + u.host;
            return false;
        }
    }

    const bool def_port = (u.scheme == "http" && u.port == 80) ||
                          (u.scheme == "https" && u.port == 443);
    const std::string host_hdr = (u.host.find(':') != std::string::npos ? ("[" + u.host + "]") : u.host) +
                                 (def_port ? "" : (":" + std::to_string(u.port)));
    const std::string req = "GET " + u.target + " HTTP/1.1\r\n"
                            "Host: " + host_hdr + "\r\n"
                            "User-Agent: brat-pkg/0.7\r\n"
                            "Accept: */*\r\n"
                            "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t w = ssl ? ossl->SSL_write(ssl, req.data() + sent, static_cast<int>(req.size() - sent))
                              : send(fd, req.data() + sent, req.size() - sent, 0);
        if (w <= 0) {
            cleanup();
            err = "HTTP write failed to " + u.host;
            return false;
        }
        sent += static_cast<size_t>(w);
    }

    std::string raw;
    char buf[16384];
    size_t hdr_end = std::string::npos;
    bool has_cl = false;
    size_t content_len = 0;
    bool chunked = false;

    while (true) {
        const ssize_t r = ssl ? ossl->SSL_read(ssl, buf, sizeof(buf))
                              : recv(fd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EINTR && !ssl) continue;
            cleanup();
            err = "HTTP read failed from " + u.host;
            return false;
        }
        if (r == 0) break;
        raw.append(buf, static_cast<size_t>(r));
        if (hdr_end == std::string::npos) {
            const size_t pos = raw.find("\r\n\r\n");
            if (pos == std::string::npos) {
                if (raw.size() > 65536) {
                    cleanup();
                    err = "HTTP response headers exceed 64 KiB";
                    return false;
                }
                continue;
            }
            hdr_end = pos + 4;
            const std::string_view hdr_block(raw.data(), pos);
            const size_t first_eol = hdr_block.find("\r\n");
            const std::string_view sline = (first_eol == std::string_view::npos)
                                               ? hdr_block
                                               : hdr_block.substr(0, first_eol);
            const size_t sp1 = sline.find(' ');
            if (sline.rfind("HTTP/1.", 0) != 0 || sp1 == std::string_view::npos || sp1 + 4 > sline.size()) {
                cleanup();
                err = "malformed HTTP status line";
                return false;
            }
            for (size_t k = 0; k < 3; ++k) {
                const char c = sline[sp1 + 1 + k];
                if (c < '0' || c > '9') {
                    cleanup();
                    err = "malformed HTTP status code";
                    return false;
                }
                status_out = status_out * 10 + (c - '0');
            }
            size_t hpos = (first_eol == std::string_view::npos) ? hdr_block.size() : (first_eol + 2);
            while (hpos < hdr_block.size()) {
                const size_t nl = hdr_block.find("\r\n", hpos);
                const std::string_view hl = (nl == std::string_view::npos)
                                                ? hdr_block.substr(hpos)
                                                : hdr_block.substr(hpos, nl - hpos);
                hpos = (nl == std::string_view::npos) ? hdr_block.size() : (nl + 2);
                const size_t col = hl.find(':');
                if (col == std::string_view::npos) continue;
                std::string k(hl.substr(0, col));
                for (char &ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                std::string_view v = hl.substr(col + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
                if (k == "location") {
                    location_out = std::string(v);
                } else if (k == "content-length") {
                    size_t parsed = 0;
                    if (v.empty()) { cleanup(); err = "empty Content-Length"; return false; }
                    for (char ch : v) {
                        if (ch < '0' || ch > '9' || parsed > max_bytes) {
                            cleanup();
                            err = (ch >= '0' && ch <= '9') ? "package exceeds 16 MiB" : "invalid Content-Length";
                            return false;
                        }
                        parsed = parsed * 10 + size_t(ch - '0');
                    }
                    if (parsed > max_bytes) {
                        cleanup();
                        err = "package exceeds 16 MiB";
                        return false;
                    }
                    if (has_cl && parsed != content_len) {
                        cleanup();
                        err = "conflicting Content-Length headers";
                        return false;
                    }
                    has_cl = true;
                    content_len = parsed;
                } else if (k == "transfer-encoding") {
                    std::string lv(v);
                    for (char &ch : lv) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (lv.find("chunked") != std::string::npos) chunked = true;
                }
            }
            if (has_cl && chunked) {
                cleanup();
                err = "HTTP response sets both Content-Length and Transfer-Encoding";
                return false;
            }
        }
        if (hdr_end != std::string::npos) {
            if (status_out == 204 || status_out == 304 || (status_out >= 100 && status_out < 200)) break;
            const size_t body_so_far = raw.size() - hdr_end;
            if (!chunked && body_so_far > max_bytes) {
                cleanup();
                err = "package exceeds 16 MiB";
                return false;
            }
            if (chunked && body_so_far > max_bytes + 262144u) {
                cleanup();
                err = "package exceeds 16 MiB";
                return false;
            }
            if (has_cl && body_so_far >= content_len) break;
        }
    }
    cleanup();
    if (hdr_end == std::string::npos) {
        err = "incomplete HTTP response headers from " + u.host;
        return false;
    }
    if (status_out == 204 || status_out == 304 || (status_out >= 100 && status_out < 200)) {
        body_out.clear();
        return true;
    }
    const std::string_view raw_body(raw.data() + hdr_end, raw.size() - hdr_end);
    if (chunked) {
        return decode_chunked_body(raw_body, body_out, max_bytes, err);
    }
    if (has_cl) {
        if (raw_body.size() < content_len) {
            err = "truncated HTTP response body";
            return false;
        }
        body_out.assign(raw_body.data(), content_len);
        return true;
    }
    if (raw_body.size() > max_bytes) {
        err = "package exceeds 16 MiB";
        return false;
    }
    body_out.assign(raw_body.data(), raw_body.size());
    return true;
}

#endif  // !_WIN32

}  // namespace

bool fetch_url(const std::string &url,
               std::string &out_bytes,
               std::string &err,
               bool allow_file,
               size_t max_bytes,
               bool block_internal_root,
               bool *connected_internal_out) {
    out_bytes.clear();
    if (connected_internal_out) *connected_internal_out = false;
    std::string scheme_low = url.substr(0, std::min<size_t>(url.size(), 8));
    for (char &c : scheme_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (scheme_low.rfind("file:", 0) == 0) {
        if (!allow_file) {
            err = "fetch " + url + ": remote source may not reference file:// URL";
            return false;
        }
        if (connected_internal_out) *connected_internal_out = true;
        std::string rest = url.substr(5);
        if (rest.rfind("///", 0) == 0) {
#if defined(_WIN32)
            if (rest.size() >= 5 && std::isalpha(static_cast<unsigned char>(rest[3])) && rest[4] == ':')
                rest.erase(0, 3);
            else
                rest.erase(0, 2);
#else
            rest.erase(0, 2);
#endif
        } else if (rest.rfind("//localhost/", 0) == 0) {
            rest.erase(0, 11);
        } else if (rest.rfind("//", 0) == 0) {
            err = "fetch " + url + ": unsupported file:// host";
            return false;
        }
        const std::string local_path = percent_decode(rest);
        if (local_path.find('\0') != std::string::npos) {
            err = "fetch " + url + ": unsafe file path";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(local_path, ec) || ec) {
            err = "fetch " + url + ": cannot open file";
            return false;
        }
        const uintmax_t sz = std::filesystem::file_size(local_path, ec);
        if (ec) {
            err = "fetch " + url + ": cannot stat file";
            return false;
        }
        if (sz > max_bytes) {
            err = "fetch " + url + ": package exceeds 16 MiB";
            return false;
        }
        std::ifstream in(local_path, std::ios::binary);
        if (!in) {
            err = "fetch " + url + ": cannot read file";
            return false;
        }
        out_bytes.resize(static_cast<size_t>(sz));
        if (sz > 0) {
            in.read(out_bytes.data(), static_cast<std::streamsize>(sz));
            if (in.gcount() != static_cast<std::streamsize>(sz)) {
                err = "fetch " + url + ": short read";
                return false;
            }
        }
        return true;
    }

    std::string cur_url = url;
    bool started_https = false;
    std::string init_host;
    bool init_internal = false;
    auto low_ascii = [](std::string_view sv) {
        std::string r(sv);
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return r;
    };
    for (int hop = 0; hop <= 5; ++hop) {
        ParsedUrl pu;
        std::string perr;
        if (!parse_http_url(cur_url, pu, perr)) {
            err = "fetch " + url + ": " + perr;
            return false;
        }
        if (hop == 0) {
            started_https = (pu.scheme == "https");
            init_host = low_ascii(pu.host);
        } else if (started_https && pu.scheme != "https") {
            err = "fetch " + url + ": refusing HTTPS downgrade redirect to " + cur_url;
            return false;
        } else if (pu.scheme == "https") {
            started_https = true;
        }
        const bool block_internal = (hop == 0)
                                        ? block_internal_root
                                        : (block_internal_root || low_ascii(pu.host) != init_host || !init_internal);
        int status = 0;
        std::string loc, body, herr;
        bool conn_internal = false;
        if (!fetch_http_single(pu, block_internal, &conn_internal, status, loc, body, max_bytes, herr)) {
            err = "fetch " + url + ": " + herr;
            return false;
        }
        if (hop == 0) init_internal = conn_internal;
        if (connected_internal_out) *connected_internal_out = conn_internal;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (loc.empty()) {
                err = "fetch " + url + ": HTTP " + std::to_string(status) + " without Location header";
                return false;
            }
            for (unsigned char c : loc) {
                if (c <= 0x20 || c >= 0x7f) {
                    err = "fetch " + url + ": unsafe character in redirect Location";
                    return false;
                }
            }
            if (hop == 5) {
                err = "fetch " + url + ": too many HTTP redirects";
                return false;
            }
            cur_url = resolve_redirect_url(pu, loc);
            continue;
        }
        if (status < 200 || status >= 300) {
            err = "fetch " + url + ": HTTP " + std::to_string(status);
            return false;
        }
    out_bytes = std::move(body);
        return true;
    }
    err = "fetch " + url + ": too many HTTP redirects";
    return false;
}

bool write_ar_bundle(const std::string &out_path,
                     const std::vector<std::string> &obj_paths,
                     std::string &err) {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "cannot create archive bundle " + out_path;
        return false;
    }
    out.write("!<arch>\n", 8);
    for (size_t i = 0; i < obj_paths.size(); ++i) {
        std::ifstream in(obj_paths[i], std::ios::binary);
        if (!in) {
            err = "cannot read object " + obj_paths[i];
            return false;
        }
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        char hdr[61];
        std::string mname = "m" + std::to_string(i) + ".o/";
        if (mname.size() > 16) mname.resize(16);
        std::snprintf(hdr, sizeof(hdr), "%-16s%-12s%-6s%-6s%-8s%-10llu`\n",
                      mname.c_str(), "0", "0", "0", "100644",
                      static_cast<unsigned long long>(data.size()));
        out.write(hdr, 60);
        if (!data.empty()) out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (data.size() & 1u) out.put('\n');
    }
    if (!out.good()) {
        err = "failed writing archive bundle " + out_path;
        return false;
    }
    return true;
}

}  // namespace pkg_io
