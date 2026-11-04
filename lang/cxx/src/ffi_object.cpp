#include "ffi_object.h"
#include "pkg_io.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace native_ffi {

namespace {

inline uint16_t rd16(const uint8_t *p, bool le) {
    return le ? static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8))
              : static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

inline uint32_t rd32(const uint8_t *p, bool le) {
    return le ? (uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
              : ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
}

inline uint64_t rd64(const uint8_t *p, bool le) {
    return le ? (uint64_t(rd32(p, true)) | (uint64_t(rd32(p + 4, true)) << 32))
              : ((uint64_t(rd32(p, false)) << 32) | uint64_t(rd32(p + 4, false)));
}

inline void wr32(uint8_t *p, uint32_t v, bool le) {
    if (le) {
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
    } else {
        p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
    }
}

inline void wr64(uint8_t *p, uint64_t v, bool le) {
    if (le) {
        wr32(p, uint32_t(v), true); wr32(p + 4, uint32_t(v >> 32), true);
    } else {
        wr32(p, uint32_t(v >> 32), false); wr32(p + 4, uint32_t(v), false);
    }
}

std::string cstr_at(std::string_view buf, size_t off) {
    if (off >= buf.size()) return {};
    const size_t nul = buf.find('\0', off);
    const size_t end = (nul == std::string_view::npos) ? buf.size() : nul;
    return std::string(buf.substr(off, end - off));
}

int sym_rank(const ObjectSym &s) {
    return (s.defined ? 4 : 0) + (s.exported ? 2 : 0) +
           ((s.kind == "func" || s.kind == "object") ? 1 : 0);
}

void merge_sym(std::map<std::string, ObjectSym> &tab,
               const std::string &raw,
               bool defined,
               bool exported,
               const std::string &kind,
               uint64_t size) {
    if (raw.empty()) return;
    const size_t at = raw.find('@');
    const std::string base = (at == std::string::npos) ? raw : raw.substr(0, at);
    if (base.empty()) return;
    ObjectSym ent{base, raw, defined, exported, kind, size};
    auto it = tab.find(base);
    if (it == tab.end()) {
        tab.emplace(base, std::move(ent));
        return;
    }
    const int pr = sym_rank(it->second);
    const int nr = sym_rank(ent);
    if (nr > pr || (nr == pr && ent.size > it->second.size)) {
        it->second = std::move(ent);
    }
}

struct DwarfSections {
    bool le = true;
    std::string debug_info;
    std::string debug_abbrev;
    std::string debug_str;
    std::string debug_line_str;
    std::string debug_str_offsets;
};

bool read_uleb(const uint8_t *p, size_t len, size_t &pos, uint64_t &out) {
    uint64_t v = 0;
    unsigned shift = 0;
    while (pos < len) {
        const uint8_t b = p[pos++];
        if (shift < 64) v |= uint64_t(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            out = v;
            return true;
        }
        shift += 7;
        if (shift > 70) return false;
    }
    return false;
}

bool read_sleb(const uint8_t *p, size_t len, size_t &pos, int64_t &out) {
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b = 0;
    while (pos < len) {
        b = p[pos++];
        if (shift < 64) v |= uint64_t(b & 0x7Fu) << shift;
        shift += 7;
        if ((b & 0x80u) == 0) {
            if (shift < 64 && (b & 0x40u)) v |= (~uint64_t(0)) << shift;
            out = static_cast<int64_t>(v);
            return true;
        }
        if (shift > 70) return false;
    }
    return false;
}

struct AbbrevAttr {
    uint64_t name = 0;
    uint64_t form = 0;
    int64_t implicit_const = 0;
};

struct AbbrevDecl {
    uint64_t tag = 0;
    bool has_children = false;
    std::vector<AbbrevAttr> attrs;
};

using AbbrevTable = std::unordered_map<uint64_t, AbbrevDecl>;

bool parse_abbrev_table(std::string_view abbrev_sec, size_t offset, AbbrevTable &out) {
    out.clear();
    if (offset >= abbrev_sec.size()) return false;
    const auto *p = reinterpret_cast<const uint8_t *>(abbrev_sec.data());
    const size_t n = abbrev_sec.size();
    size_t pos = offset;
    while (pos < n) {
        uint64_t code = 0;
        if (!read_uleb(p, n, pos, code)) return false;
        if (code == 0) break;
        AbbrevDecl decl;
        if (!read_uleb(p, n, pos, decl.tag) || pos >= n) return false;
        decl.has_children = (p[pos++] != 0);
        while (true) {
            uint64_t aname = 0, aform = 0;
            if (!read_uleb(p, n, pos, aname) || !read_uleb(p, n, pos, aform)) return false;
            if (aname == 0 && aform == 0) break;
            int64_t imp = 0;
            if (aform == 0x21u /* DW_FORM_implicit_const */) {
                if (!read_sleb(p, n, pos, imp)) return false;
            }
            decl.attrs.push_back(AbbrevAttr{aname, aform, imp});
        }
        out.emplace(code, std::move(decl));
    }
    return true;
}

struct DieAttrVal {
    bool has_int = false;
    int64_t ival = 0;
    bool has_ref = false;
    uint64_t ref_off = 0;
    bool has_str = false;
    std::string sval;
    bool flag = false;
};

struct RawDie {
    uint64_t off = 0;
    uint64_t tag = 0;
    std::unordered_map<uint64_t, DieAttrVal> attrs;
    std::vector<uint64_t> children;
};

bool parse_dwarf_sections(const DwarfSections &sec,
                          std::map<std::string, DwarfFnSig> &out_fns,
                          std::map<std::string, DwarfRecord> &out_structs,
                          std::string &err) {
    if (sec.debug_info.empty()) return true;
    if (sec.debug_abbrev.empty()) {
        err = "corrupt DWARF: .debug_info present without .debug_abbrev";
        return false;
    }
    const auto *p = reinterpret_cast<const uint8_t *>(sec.debug_info.data());
    const size_t n = sec.debug_info.size();
    const bool le = sec.le;

    size_t cu_start = 0;
    bool parsed_any_cu = false;
    while (cu_start < n) {
        bool rem_zero = true;
        for (size_t z = cu_start; z < n; ++z) {
            if (p[z] != 0) { rem_zero = false; break; }
        }
        if (rem_zero && parsed_any_cu) break;
        if (n - cu_start < 11) {
            err = "corrupt DWARF: truncated compilation unit header";
            return false;
        }
        size_t pos = cu_start;
        uint64_t unit_len = rd32(p + pos, le);
        pos += 4;
        bool is_dw64 = false;
        if (unit_len == 0xFFFFFFFFu) {
            if (n - pos < 8) {
                err = "corrupt DWARF: truncated 64-bit compilation unit length";
                return false;
            }
            unit_len = rd64(p + pos, le);
            pos += 8;
            is_dw64 = true;
        } else if (unit_len == 0 || unit_len >= 0xFFFFFFF0u) {
            err = "corrupt DWARF: invalid compilation unit length";
            return false;
        }
        if (unit_len > n - pos) {
            err = "corrupt DWARF: compilation unit exceeds .debug_info bounds";
            return false;
        }
        const size_t cu_end = pos + static_cast<size_t>(unit_len);
        const size_t off_sz = is_dw64 ? 8u : 4u;

        if (cu_end - pos < 2) {
            err = "corrupt DWARF: missing compilation unit version";
            return false;
        }
        const uint16_t version = rd16(p + pos, le);
        pos += 2;
        if (version < 2 || version > 5) {
            err = "unsupported DWARF version " + std::to_string(version);
            return false;
        }

        uint64_t abbrev_off = 0;
        uint8_t addr_size = 8;
        if (version == 5) {
            if (cu_end - pos < 2 + off_sz) {
                err = "corrupt DWARF v5 compilation unit header";
                return false;
            }
            const uint8_t unit_type = p[pos++];
            addr_size = p[pos++];
            abbrev_off = is_dw64 ? rd64(p + pos, le) : rd32(p + pos, le);
            pos += off_sz;
            if (unit_type == 0x04u || unit_type == 0x05u) {
                if (cu_end - pos < 8) { err = "corrupt DWARF v5 split header"; return false; }
                pos += 8;
            } else if (unit_type == 0x02u || unit_type == 0x06u) {
                if (cu_end - pos < 8 + off_sz) { err = "corrupt DWARF v5 type header"; return false; }
                pos += 8 + off_sz;
            }
        } else {
            if (cu_end - pos < off_sz + 1) {
                err = "corrupt DWARF compilation unit header";
                return false;
            }
            abbrev_off = is_dw64 ? rd64(p + pos, le) : rd32(p + pos, le);
            pos += off_sz;
            addr_size = p[pos++];
        }
        if (addr_size != 4 && addr_size != 8) {
            err = "corrupt DWARF: unsupported address size";
            return false;
        }

        AbbrevTable abbrevs;
        if (abbrev_off > sec.debug_abbrev.size() ||
            !parse_abbrev_table(sec.debug_abbrev, static_cast<size_t>(abbrev_off), abbrevs)) {
            err = "corrupt DWARF: invalid .debug_abbrev table";
            return false;
        }

        uint64_t str_offsets_base = (is_dw64 ? 16u : 8u);
        std::unordered_map<uint64_t, RawDie> dies;
        std::vector<uint64_t> die_order;
        std::vector<uint64_t> stack;

        auto read_strx = [&](uint64_t idx) -> std::string {
            const size_t dso_sz = sec.debug_str_offsets.size();
            if (str_offsets_base > dso_sz || off_sz > dso_sz - str_offsets_base) return {};
            const size_t avail = dso_sz - static_cast<size_t>(str_offsets_base);
            if (idx > (avail - off_sz) / off_sz) return {};
            const size_t byte_off = static_cast<size_t>(str_offsets_base) + static_cast<size_t>(idx) * off_sz;
            const auto *sp = reinterpret_cast<const uint8_t *>(sec.debug_str_offsets.data()) + byte_off;
            const uint64_t soff = is_dw64 ? rd64(sp, le) : rd32(sp, le);
            if (soff > sec.debug_str.size()) return {};
            return cstr_at(sec.debug_str, static_cast<size_t>(soff));
        };

        auto read_form = [&](uint64_t form, int64_t imp_const, DieAttrVal &val) -> bool {
            while (form == 0x16u /* DW_FORM_indirect */) {
                if (!read_uleb(p, cu_end, pos, form)) return false;
            }
            switch (form) {
                case 0x01u: {  // DW_FORM_addr
                    if (cu_end - pos < addr_size) return false;
                    val.has_int = true;
                    val.ival = (addr_size == 8) ? static_cast<int64_t>(rd64(p + pos, le))
                                                : static_cast<int64_t>(rd32(p + pos, le));
                    pos += addr_size;
                    return true;
                }
                case 0x03u: {  // DW_FORM_block2
                    if (cu_end - pos < 2) return false;
                    const uint16_t bl = rd16(p + pos, le);
                    pos += 2;
                    if (cu_end - pos < bl) return false;
                    if (bl >= 2 && p[pos] == 0x23u /* DW_OP_plus_uconst */) {
                        size_t bp = pos + 1;
                        uint64_t u = 0;
                        if (read_uleb(p, pos + bl, bp, u)) { val.has_int = true; val.ival = static_cast<int64_t>(u); }
                    }
                    pos += bl;
                    return true;
                }
                case 0x04u: {  // DW_FORM_block4
                    if (cu_end - pos < 4) return false;
                    const uint32_t bl = rd32(p + pos, le);
                    pos += 4;
                    if (bl > cu_end - pos) return false;
                    pos += bl;
                    return true;
                }
                case 0x05u: {  // DW_FORM_data2
                    if (cu_end - pos < 2) return false;
                    val.has_int = true;
                    val.ival = rd16(p + pos, le);
                    pos += 2;
                    return true;
                }
                case 0x06u: {  // DW_FORM_data4
                    if (cu_end - pos < 4) return false;
                    val.has_int = true;
                    val.ival = rd32(p + pos, le);
                    pos += 4;
                    return true;
                }
                case 0x07u: {  // DW_FORM_data8
                    if (cu_end - pos < 8) return false;
                    val.has_int = true;
                    val.ival = static_cast<int64_t>(rd64(p + pos, le));
                    pos += 8;
                    return true;
                }
                case 0x08u: {  // DW_FORM_string
                    const size_t start = pos;
                    while (pos < cu_end && p[pos] != 0) ++pos;
                    if (pos >= cu_end) return false;
                    val.has_str = true;
                    val.sval.assign(reinterpret_cast<const char *>(p + start), pos - start);
                    ++pos;
                    return true;
                }
                case 0x09u:    // DW_FORM_block
                case 0x18u: {  // DW_FORM_exprloc
                    uint64_t bl = 0;
                    if (!read_uleb(p, cu_end, pos, bl) || bl > cu_end - pos) return false;
                    if (bl >= 2 && p[pos] == 0x23u /* DW_OP_plus_uconst */) {
                        size_t bp = pos + 1;
                        uint64_t u = 0;
                        if (read_uleb(p, pos + static_cast<size_t>(bl), bp, u)) {
                            val.has_int = true;
                            val.ival = static_cast<int64_t>(u);
                        }
                    } else if (bl >= 1 && p[pos] >= 0x30u && p[pos] <= 0x4Fu /* DW_OP_lit0..31 */) {
                        val.has_int = true;
                        val.ival = int64_t(p[pos] - 0x30u);
                    }
                    pos += static_cast<size_t>(bl);
                    return true;
                }
                case 0x0Au: {  // DW_FORM_block1
                    if (pos >= cu_end) return false;
                    const uint8_t bl = p[pos++];
                    if (cu_end - pos < bl) return false;
                    if (bl >= 2 && p[pos] == 0x23u) {
                        size_t bp = pos + 1;
                        uint64_t u = 0;
                        if (read_uleb(p, pos + bl, bp, u)) { val.has_int = true; val.ival = static_cast<int64_t>(u); }
                    }
                    pos += bl;
                    return true;
                }
                case 0x0Bu: {  // DW_FORM_data1
                    if (pos >= cu_end) return false;
                    val.has_int = true;
                    val.ival = p[pos++];
                    return true;
                }
                case 0x0Cu: {  // DW_FORM_flag
                    if (pos >= cu_end) return false;
                    val.flag = (p[pos++] != 0);
                    return true;
                }
                case 0x0Du: {  // DW_FORM_sdata
                    int64_t sv = 0;
                    if (!read_sleb(p, cu_end, pos, sv)) return false;
                    val.has_int = true;
                    val.ival = sv;
                    return true;
                }
                case 0x0Eu: {  // DW_FORM_strp
                    if (cu_end - pos < off_sz) return false;
                    const uint64_t soff = is_dw64 ? rd64(p + pos, le) : rd32(p + pos, le);
                    pos += off_sz;
                    val.has_str = true;
                    val.sval = (soff <= sec.debug_str.size()) ? cstr_at(sec.debug_str, static_cast<size_t>(soff)) : std::string();
                    return true;
                }
                case 0x0Fu: {  // DW_FORM_udata
                    uint64_t uv = 0;
                    if (!read_uleb(p, cu_end, pos, uv)) return false;
                    val.has_int = true;
                    val.ival = static_cast<int64_t>(uv);
                    return true;
                }
                case 0x10u: {  // DW_FORM_ref_addr
                    const size_t rsz = (version == 2) ? addr_size : off_sz;
                    if (cu_end - pos < rsz) return false;
                    val.has_ref = true;
                    val.ref_off = (rsz == 8) ? rd64(p + pos, le) : rd32(p + pos, le);
                    pos += rsz;
                    return true;
                }
                case 0x11u: {  // DW_FORM_ref1
                    if (pos >= cu_end) return false;
                    val.has_ref = true;
                    val.ref_off = cu_start + p[pos++];
                    return true;
                }
                case 0x12u: {  // DW_FORM_ref2
                    if (cu_end - pos < 2) return false;
                    val.has_ref = true;
                    val.ref_off = cu_start + rd16(p + pos, le);
                    pos += 2;
                    return true;
                }
                case 0x13u: {  // DW_FORM_ref4
                    if (cu_end - pos < 4) return false;
                    val.has_ref = true;
                    val.ref_off = cu_start + rd32(p + pos, le);
                    pos += 4;
                    return true;
                }
                case 0x14u: {  // DW_FORM_ref8
                    if (cu_end - pos < 8) return false;
                    val.has_ref = true;
                    val.ref_off = cu_start + rd64(p + pos, le);
                    pos += 8;
                    return true;
                }
                case 0x15u: {  // DW_FORM_ref_udata
                    uint64_t u = 0;
                    if (!read_uleb(p, cu_end, pos, u)) return false;
                    val.has_ref = true;
                    val.ref_off = cu_start + u;
                    return true;
                }
                case 0x17u: {  // DW_FORM_sec_offset
                    if (cu_end - pos < off_sz) return false;
                    val.has_int = true;
                    val.ival = static_cast<int64_t>(is_dw64 ? rd64(p + pos, le) : rd32(p + pos, le));
                    pos += off_sz;
                    return true;
                }
                case 0x19u: {  // DW_FORM_flag_present
                    val.flag = true;
                    return true;
                }
                case 0x1Au: {  // DW_FORM_strx
                    uint64_t idx = 0;
                    if (!read_uleb(p, cu_end, pos, idx)) return false;
                    val.has_str = true;
                    val.sval = read_strx(idx);
                    return true;
                }
                case 0x1Bu:     // DW_FORM_addrx
                case 0x22u:     // DW_FORM_loclistx
                case 0x23u:     // DW_FORM_rnglistx
                case 0x1F20u:   // DW_FORM_GNU_addr_index
                case 0x1F21u: { // DW_FORM_GNU_str_index
                    uint64_t idx = 0;
                    return read_uleb(p, cu_end, pos, idx);
                }
                case 0x1Cu: {  // DW_FORM_ref_sup4
                    if (cu_end - pos < 4) return false;
                    pos += 4;
                    return true;
                }
                case 0x1Du:     // DW_FORM_strp_sup
                case 0x1F01u:   // DW_FORM_GNU_ref_alt
                case 0x1F02u: { // DW_FORM_GNU_strp_alt
                    if (cu_end - pos < off_sz) return false;
                    pos += off_sz;
                    return true;
                }
                case 0x1Eu: {  // DW_FORM_data16
                    if (cu_end - pos < 16) return false;
                    pos += 16;
                    return true;
                }
                case 0x1Fu: {  // DW_FORM_line_strp
                    if (cu_end - pos < off_sz) return false;
                    const uint64_t soff = is_dw64 ? rd64(p + pos, le) : rd32(p + pos, le);
                    pos += off_sz;
                    val.has_str = true;
                    val.sval = (soff <= sec.debug_line_str.size()) ? cstr_at(sec.debug_line_str, static_cast<size_t>(soff)) : std::string();
                    return true;
                }
                case 0x20u:    // DW_FORM_ref_sig8
                case 0x24u: {  // DW_FORM_ref_sup8
                    if (cu_end - pos < 8) return false;
                    pos += 8;
                    return true;
                }
                case 0x21u: {  // DW_FORM_implicit_const
                    val.has_int = true;
                    val.ival = imp_const;
                    val.flag = (imp_const != 0);
                    return true;
                }
                case 0x25u: {  // DW_FORM_strx1
                    if (pos >= cu_end) return false;
                    val.has_str = true;
                    val.sval = read_strx(p[pos++]);
                    return true;
                }
                case 0x26u: {  // DW_FORM_strx2
                    if (cu_end - pos < 2) return false;
                    val.has_str = true;
                    val.sval = read_strx(rd16(p + pos, le));
                    pos += 2;
                    return true;
                }
                case 0x27u: {  // DW_FORM_strx3
                    if (cu_end - pos < 3) return false;
                    const uint32_t idx = le ? (uint32_t(p[pos]) | (uint32_t(p[pos + 1]) << 8) | (uint32_t(p[pos + 2]) << 16))
                                            : ((uint32_t(p[pos]) << 16) | (uint32_t(p[pos + 1]) << 8) | uint32_t(p[pos + 2]));
                    pos += 3;
                    val.has_str = true;
                    val.sval = read_strx(idx);
                    return true;
                }
                case 0x28u: {  // DW_FORM_strx4
                    if (cu_end - pos < 4) return false;
                    val.has_str = true;
                    val.sval = read_strx(rd32(p + pos, le));
                    pos += 4;
                    return true;
                }
                case 0x29u: { if (cu_end - pos < 1) return false; pos += 1; return true; }  // addrx1
                case 0x2Au: { if (cu_end - pos < 2) return false; pos += 2; return true; }  // addrx2
                case 0x2Bu: { if (cu_end - pos < 3) return false; pos += 3; return true; }  // addrx3
                case 0x2Cu: { if (cu_end - pos < 4) return false; pos += 4; return true; }  // addrx4
                default:
                    return false;
            }
        };

        bool cu_ok = true;
        while (pos < cu_end) {
            const uint64_t die_off = pos;
            uint64_t acode = 0;
            if (!read_uleb(p, cu_end, pos, acode)) { cu_ok = false; break; }
            if (acode == 0) {
                if (!stack.empty()) stack.pop_back();
                continue;
            }
            auto ait = abbrevs.find(acode);
            if (ait == abbrevs.end()) { cu_ok = false; break; }
            RawDie die;
            die.off = die_off;
            die.tag = ait->second.tag;
            for (const AbbrevAttr &aa : ait->second.attrs) {
                DieAttrVal val;
                if (!read_form(aa.form, aa.implicit_const, val)) { cu_ok = false; break; }
                if (aa.name == 0x72u /* DW_AT_str_offsets_base */ && val.has_int && val.ival >= 0) {
                    str_offsets_base = static_cast<uint64_t>(val.ival);
                }
                die.attrs.emplace(aa.name, std::move(val));
            }
            if (!cu_ok) break;
            if (!stack.empty()) dies[stack.back()].children.push_back(die_off);
            die_order.push_back(die_off);
            dies.emplace(die_off, std::move(die));
            if (ait->second.has_children) stack.push_back(die_off);
        }
        if (!cu_ok) {
            err = "corrupt DWARF: malformed DIE stream in .debug_info";
            return false;
        }
        parsed_any_cu = true;

        std::unordered_map<uint64_t, std::string> typedef_struct_names;
        for (uint64_t doff : die_order) {
            const RawDie &d = dies[doff];
            if (d.tag == 0x16u /* DW_TAG_typedef */) {
                auto nit = d.attrs.find(0x03u /* DW_AT_name */);
                auto tit = d.attrs.find(0x49u /* DW_AT_type */);
                if (nit != d.attrs.end() && nit->second.has_str && !nit->second.sval.empty() &&
                    tit != d.attrs.end() && tit->second.has_ref) {
                    auto tdie = dies.find(tit->second.ref_off);
                    if (tdie != dies.end() &&
                        (tdie->second.tag == 0x13u /* structure_type */ ||
                         tdie->second.tag == 0x17u /* union_type */)) {
                        typedef_struct_names.try_emplace(tdie->first, nit->second.sval);
                    }
                }
            }
        }

        auto is_qualifier_tag = [](uint64_t tag) {
            return tag == 0x16u /* typedef */ || tag == 0x26u /* const_type */ ||
                   tag == 0x35u /* volatile_type */ || tag == 0x37u /* restrict_type */ ||
                   tag == 0x47u /* atomic_type */;
        };

        auto peel_type = [&](uint64_t ref_off) -> const RawDie * {
            uint64_t cur = ref_off;
            for (int depth = 0; depth < 32; ++depth) {
                auto it = dies.find(cur);
                if (it == dies.end()) return nullptr;
                if (is_qualifier_tag(it->second.tag)) {
                    auto tit = it->second.attrs.find(0x49u);
                    if (tit == it->second.attrs.end() || !tit->second.has_ref) return nullptr;
                    cur = tit->second.ref_off;
                    continue;
                }
                return &it->second;
            }
            return nullptr;
        };

        auto type_shape = [&](auto &self, const RawDie *owner, bool in_member, int depth) -> DwarfSlot {
            if (!owner) return DwarfSlot{"void", std::nullopt, {}, nullptr};
            auto tit = owner->attrs.find(0x49u /* DW_AT_type */);
            if (tit == owner->attrs.end() || !tit->second.has_ref)
                return DwarfSlot{"void", std::nullopt, {}, nullptr};
            if (depth > 28) return DwarfSlot{"unknown", std::nullopt, {}, nullptr};
            auto dt_it = dies.find(tit->second.ref_off);
            if (dt_it == dies.end()) return DwarfSlot{"unknown", std::nullopt, {}, nullptr};
            const RawDie *td = &dt_it->second;
            const uint64_t ttag = td->tag;
            if (is_qualifier_tag(ttag)) {
                return self(self, td, in_member, depth + 1);
            }
            if (ttag == 0x01u /* DW_TAG_array_type */ && in_member) {
                const DwarfSlot elem = self(self, td, true, depth + 1);
                bool has_dim = false;
                bool dim_bad = false;
                uint64_t cnt_u = 1;
                for (uint64_t ch_off : td->children) {
                    const RawDie &ch = dies[ch_off];
                    if (ch.tag == 0x21u /* DW_TAG_subrange_type */) {
                        uint64_t sub_u = 0;
                        bool got_sub = false;
                        auto cit = ch.attrs.find(0x37u /* DW_AT_count */);
                        if (cit != ch.attrs.end() && cit->second.has_int) {
                            if (cit->second.ival < 0) { dim_bad = true; break; }
                            sub_u = static_cast<uint64_t>(cit->second.ival);
                            got_sub = true;
                        } else {
                            auto uit = ch.attrs.find(0x2Fu /* DW_AT_upper_bound */);
                            if (uit != ch.attrs.end() && uit->second.has_int) {
                                if (uit->second.ival < -1 || uit->second.ival == INT64_MAX) {
                                    dim_bad = true;
                                    break;
                                }
                                sub_u = static_cast<uint64_t>(uit->second.ival + 1);
                                got_sub = true;
                            }
                        }
                        if (!got_sub) { dim_bad = true; break; }
                        has_dim = true;
                        if (__builtin_mul_overflow(cnt_u, sub_u, &cnt_u) ||
                            cnt_u > static_cast<uint64_t>(INT64_MAX)) {
                            dim_bad = true;
                            break;
                        }
                    }
                }
                std::optional<int64_t> tot;
                if (elem.byte_width && *elem.byte_width >= 0 && has_dim && !dim_bad) {
                    uint64_t bw_u = static_cast<uint64_t>(*elem.byte_width);
                    uint64_t tot_u = 0;
                    if (!__builtin_mul_overflow(bw_u, cnt_u, &tot_u) &&
                        tot_u <= static_cast<uint64_t>(INT64_MAX)) {
                        tot = static_cast<int64_t>(tot_u);
                    }
                }
                return DwarfSlot{"array", tot, {}, nullptr};
            }
            if (ttag == 0x0Fu /* DW_TAG_pointer_type */) {
                auto ptit = td->attrs.find(0x49u);
                if (ptit != td->attrs.end() && ptit->second.has_ref) {
                    const RawDie *ptgt = peel_type(ptit->second.ref_off);
                    if (ptgt && ptgt->tag == 0x15u /* DW_TAG_subroutine_type */) {
                        auto sig = std::make_shared<DwarfFnSig>();
                        sig->ret = std::make_shared<DwarfSlot>(self(self, ptgt, false, depth + 1));
                        for (uint64_t ch_off : ptgt->children) {
                            const RawDie &ch = dies[ch_off];
                            if (ch.tag == 0x05u /* DW_TAG_formal_parameter */) {
                                sig->params.push_back(self(self, &ch, false, depth + 1));
                            } else if (ch.tag == 0x18u /* DW_TAG_unspecified_parameters */) {
                                sig->variadic = true;
                            }
                        }
                        return DwarfSlot{"fn", int64_t(addr_size), {}, sig};
                    }
                }
                return DwarfSlot{"pointer", int64_t(addr_size), {}, nullptr};
            }
            if (ttag == 0x10u /* reference_type */ || ttag == 0x42u /* rvalue_reference_type */ ||
                ttag == 0x15u /* subroutine_type */ || ttag == 0x01u /* array_type */) {
                return DwarfSlot{"pointer", int64_t(addr_size), {}, nullptr};
            }
            if (ttag == 0x24u /* DW_TAG_base_type */) {
                std::optional<int64_t> bsz;
                auto sit = td->attrs.find(0x0Bu /* DW_AT_byte_size */);
                if (sit != td->attrs.end() && sit->second.has_int) bsz = sit->second.ival;
                bool is_flt = false;
                auto eit = td->attrs.find(0x3Eu /* DW_AT_encoding */);
                if (eit != td->attrs.end() && eit->second.has_int) {
                    is_flt = (eit->second.ival == 4 /* DW_ATE_float */ ||
                              eit->second.ival == 3 /* DW_ATE_complex_float */);
                }
                std::string nm;
                auto nit = td->attrs.find(0x03u);
                if (nit != td->attrs.end() && nit->second.has_str) nm = nit->second.sval;
                return DwarfSlot{is_flt ? "float" : "scalar", bsz, nm, nullptr};
            }
            if (ttag == 0x04u /* DW_TAG_enumeration_type */) {
                std::optional<int64_t> bsz;
                auto sit = td->attrs.find(0x0Bu);
                if (sit != td->attrs.end() && sit->second.has_int) bsz = sit->second.ival;
                if (!bsz && td->attrs.count(0x49u)) {
                    const DwarfSlot sub = self(self, td, false, depth + 1);
                    bsz = sub.byte_width;
                }
                std::string nm;
                auto nit = td->attrs.find(0x03u);
                if (nit != td->attrs.end() && nit->second.has_str) nm = nit->second.sval;
                return DwarfSlot{"scalar", bsz, nm, nullptr};
            }
            if (ttag == 0x13u /* structure_type */ || ttag == 0x17u /* union_type */) {
                std::optional<int64_t> bsz;
                auto sit = td->attrs.find(0x0Bu);
                if (sit != td->attrs.end() && sit->second.has_int) bsz = sit->second.ival;
                std::string snm;
                auto nit = td->attrs.find(0x03u);
                if (nit != td->attrs.end() && nit->second.has_str) snm = nit->second.sval;
                if (snm.empty()) {
                    auto tnit = typedef_struct_names.find(td->off);
                    if (tnit != typedef_struct_names.end()) snm = tnit->second;
                }
                return DwarfSlot{(ttag == 0x17u) ? "union" : "struct", bsz, snm, nullptr};
            }
            return DwarfSlot{"unknown", std::nullopt, {}, nullptr};
        };

        for (uint64_t doff : die_order) {
            const RawDie &d = dies[doff];
            auto decl_it = d.attrs.find(0x3Cu /* DW_AT_declaration */);
            if (decl_it != d.attrs.end() && (decl_it->second.flag || (decl_it->second.has_int && decl_it->second.ival != 0)))
                continue;

            if (d.tag == 0x13u || d.tag == 0x17u) {
                auto sit = d.attrs.find(0x0Bu /* DW_AT_byte_size */);
                if (sit == d.attrs.end() || !sit->second.has_int) continue;
                const bool is_un = (d.tag == 0x17u);
                std::vector<std::string> names;
                auto nit = d.attrs.find(0x03u);
                if (nit != d.attrs.end() && nit->second.has_str && !nit->second.sval.empty())
                    names.push_back(nit->second.sval);
                auto tnit = typedef_struct_names.find(d.off);
                if (tnit != typedef_struct_names.end() && !tnit->second.empty())
                    names.push_back(tnit->second);
                if (names.empty()) continue;

                DwarfRecord rec;
                rec.size = sit->second.ival;
                rec.is_union = is_un;
                int anon_k = 0;
                for (uint64_t ch_off : d.children) {
                    const RawDie &ch = dies[ch_off];
                    if (ch.tag != 0x0Du /* DW_TAG_member */) continue;
                    std::string mnm;
                    auto mnit = ch.attrs.find(0x03u);
                    if (mnit != ch.attrs.end() && mnit->second.has_str) mnm = mnit->second.sval;
                    if (mnm.empty()) {
                        mnm = "anon" + std::to_string(anon_k++);
                        auto mtit = ch.attrs.find(0x49u);
                        if (mtit != ch.attrs.end() && mtit->second.has_ref) {
                            for (const auto &pn : names)
                                typedef_struct_names.try_emplace(mtit->second.ref_off, pn + "__" + mnm);
                        }
                    }
                    int64_t moff = 0;
                    if (!is_un) {
                        auto loit = ch.attrs.find(0x38u /* DW_AT_data_member_location */);
                        if (loit != ch.attrs.end() && loit->second.has_int) moff = loit->second.ival;
                    }
                    DwarfSlot mslot = type_shape(type_shape, &ch, true, 0);
                    rec.members.push_back(DwarfMember{mnm, moff, std::move(mslot)});
                }
                for (const std::string &snm : names) {
                    out_structs[snm] = rec;
                }
            } else if (d.tag == 0x2Eu /* DW_TAG_subprogram */) {
                std::string fnm;
                for (uint64_t key : {uint64_t(0x6Eu) /* linkage_name */,
                                     uint64_t(0x2007u) /* MIPS_linkage_name */,
                                     uint64_t(0x03u) /* name */}) {
                    auto it = d.attrs.find(key);
                    if (it != d.attrs.end() && it->second.has_str && !it->second.sval.empty()) {
                        fnm = it->second.sval;
                        break;
                    }
                }
                if (fnm.empty()) continue;
                DwarfFnSig sig;
                sig.ret = std::make_shared<DwarfSlot>(type_shape(type_shape, &d, false, 0));
                for (uint64_t ch_off : d.children) {
                    const RawDie &ch = dies[ch_off];
                    if (ch.tag == 0x05u /* formal_parameter */) {
                        sig.params.push_back(type_shape(type_shape, &ch, false, 0));
                    } else if (ch.tag == 0x18u /* unspecified_parameters */) {
                        sig.variadic = true;
                    }
                }
                out_fns[fnm] = std::move(sig);
            }
        }
        cu_start = cu_end;
    }
    return true;
}

bool decompress_zstd_bytes(const uint8_t *src, size_t src_sz, uint64_t usize, std::string &out) {
#if defined(_WIN32)
    (void)src; (void)src_sz; (void)usize; (void)out;
    return false;
#else
    using ZstdDecompFn = size_t (*)(void *, size_t, const void *, size_t);
    using ZstdIsErrFn = unsigned (*)(size_t);
    static int state = 0;
    static ZstdDecompFn fn_decomp = nullptr;
    static ZstdIsErrFn fn_iserr = nullptr;
    if (state == 0) {
        const char *libs[] = {"libzstd.so.1", "libzstd.so", "libzstd.1.dylib", "libzstd.dylib", nullptr};
        void *h = nullptr;
        for (int i = 0; libs[i] && !h; ++i) h = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            fn_decomp = reinterpret_cast<ZstdDecompFn>(dlsym(h, "ZSTD_decompress"));
            fn_iserr = reinterpret_cast<ZstdIsErrFn>(dlsym(h, "ZSTD_isError"));
        }
        state = (fn_decomp && fn_iserr) ? 1 : -1;
    }
    if (state < 0) return false;
    out.resize(static_cast<size_t>(usize));
    if (usize == 0) return true;
    const size_t got = fn_decomp(out.data(), out.size(), src, src_sz);
    if (fn_iserr(got) || got != static_cast<size_t>(usize)) {
        out.clear();
        return false;
    }
    return true;
#endif
}

bool extract_elf_sec_bytes(std::string_view file,
                           uint64_t sh_flags,
                           uint64_t sh_off,
                           uint64_t sh_size,
                           bool is64,
                           bool le,
                           std::string_view sname,
                           std::string &out,
                           std::string &err) {
    if (sh_off > file.size() || sh_size > file.size() - sh_off) {
        err = "truncated ELF section " + std::string(sname);
        return false;
    }
    const auto *raw = reinterpret_cast<const uint8_t *>(file.data() + sh_off);
    const size_t sz = static_cast<size_t>(sh_size);
    if (sh_flags & 0x800u /* SHF_COMPRESSED */) {
        const size_t chdr = is64 ? 24u : 12u;
        if (sz < chdr) {
            err = "truncated SHF_COMPRESSED header in " + std::string(sname);
            return false;
        }
        const uint32_t ctype = rd32(raw, le);
        const uint64_t usize = is64 ? rd64(raw + 8, le) : rd32(raw + 4, le);
        if (usize > 64u * 1024u * 1024u) {
            err = "compressed ELF section exceeds 64 MiB: " + std::string(sname);
            return false;
        }
        if (ctype == 1u /* ELFCOMPRESS_ZLIB */) {
            std::string zerr;
            if (!pkg_io::inflate_zlib(raw + chdr, sz - chdr, out, static_cast<size_t>(usize) + 16u, zerr)) {
                err = "corrupt zlib DWARF section " + std::string(sname) + ": " + zerr;
                return false;
            }
            return true;
        }
        if (ctype == 2u /* ELFCOMPRESS_ZSTD */) {
            if (!decompress_zstd_bytes(raw + chdr, sz - chdr, usize, out)) {
                err = "cannot decompress zstd DWARF section " + std::string(sname) +
                      " (install libzstd or compile with -gz=zlib)";
                return false;
            }
            return true;
        }
        err = "unsupported ELF compression type " + std::to_string(ctype) + " in " + std::string(sname);
        return false;
    }
    if (sname.rfind(".zdebug_", 0) == 0) {
        if (sz < 12 || std::memcmp(raw, "ZLIB", 4) != 0) {
            err = "malformed .zdebug header in " + std::string(sname);
            return false;
        }
        const uint64_t usize = rd64(raw + 4, false);
        if (usize > 64u * 1024u * 1024u) {
            err = "compressed .zdebug section exceeds 64 MiB: " + std::string(sname);
            return false;
        }
        std::string zerr;
        if (!pkg_io::inflate_zlib(raw + 12, sz - 12, out, static_cast<size_t>(usize) + 16u, zerr)) {
            err = "corrupt .zdebug section " + std::string(sname) + ": " + zerr;
            return false;
        }
        return true;
    }
    out.assign(reinterpret_cast<const char *>(raw), sz);
    return true;
}

bool parse_elf_bytes(std::string_view data, ObjectInfo &out, std::string &err) {
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f""ELF", 4) != 0) {
        err = "not an ELF object";
        return false;
    }
    const auto *p = reinterpret_cast<const uint8_t *>(data.data());
    const uint8_t ei_class = p[4];
    const uint8_t ei_data = p[5];
    if ((ei_class != 1 && ei_class != 2) || (ei_data != 1 && ei_data != 2)) {
        err = "unsupported ELF class/endianness";
        return false;
    }
    const bool is64 = (ei_class == 2);
    const bool le = (ei_data == 1);
    if (is64 && data.size() < 64) {
        err = "truncated ELF64 header";
        return false;
    }
    out.format = "elf";
    out.ptr_width = is64 ? 8 : 4;
    const uint16_t e_machine = rd16(p + 18, le);
    switch (e_machine) {
        case 3: out.arch = "x86"; break;
        case 62: out.arch = "x86_64"; break;
        case 40: out.arch = "arm"; break;
        case 183: out.arch = "aarch64"; break;
        case 243: out.arch = "riscv"; break;
        case 8: out.arch = "mips"; break;
        default: out.arch = "elf-" + std::to_string(e_machine); break;
    }

    const uint64_t e_shoff = is64 ? rd64(p + 40, le) : rd32(p + 32, le);
    const uint16_t e_shentsize = rd16(p + (is64 ? 58 : 46), le);
    const uint16_t e_shnum = rd16(p + (is64 ? 60 : 48), le);
    const uint16_t e_shstrndx = rd16(p + (is64 ? 62 : 50), le);

    struct ElfSec {
        uint32_t name_off = 0;
        uint32_t type = 0;
        uint64_t flags = 0;
        uint64_t off = 0;
        uint64_t size = 0;
        uint32_t link = 0;
        uint32_t info = 0;
        uint64_t entsize = 0;
    };
    std::vector<ElfSec> shdrs;
    if (e_shoff && e_shnum && e_shentsize >= (is64 ? 64u : 40u) &&
        e_shoff <= data.size() && e_shentsize <= data.size() - e_shoff) {
        const uint64_t max_sh = (data.size() - static_cast<size_t>(e_shoff)) / e_shentsize;
        const uint64_t sh_cnt = std::min<uint64_t>(e_shnum, max_sh);
        for (uint64_t i = 0; i < sh_cnt; ++i) {
            const size_t off = static_cast<size_t>(e_shoff) + static_cast<size_t>(i * e_shentsize);
            const uint8_t *sp = p + off;
            ElfSec s;
            s.name_off = rd32(sp, le);
            s.type = rd32(sp + 4, le);
            if (is64) {
                s.flags = rd64(sp + 8, le);
                s.off = rd64(sp + 24, le);
                s.size = rd64(sp + 32, le);
                s.link = rd32(sp + 40, le);
                s.info = rd32(sp + 44, le);
                s.entsize = rd64(sp + 56, le);
            } else {
                s.flags = rd32(sp + 8, le);
                s.off = rd32(sp + 16, le);
                s.size = rd32(sp + 20, le);
                s.link = rd32(sp + 24, le);
                s.info = rd32(sp + 28, le);
                s.entsize = rd32(sp + 36, le);
            }
            shdrs.push_back(s);
        }
    }

    std::string_view shstrtab;
    if (e_shstrndx < shdrs.size()) {
        const ElfSec &ss = shdrs[e_shstrndx];
        if (ss.off <= data.size() && ss.size <= data.size() - ss.off)
            shstrtab = data.substr(static_cast<size_t>(ss.off), static_cast<size_t>(ss.size));
    }

    auto read_sym_val = [&](uint32_t symtab_idx, uint32_t sym_idx) -> uint64_t {
        if (symtab_idx >= shdrs.size()) return 0;
        const ElfSec &st = shdrs[symtab_idx];
        const uint64_t esz = st.entsize ? st.entsize : (is64 ? 24u : 16u);
        if (esz < (is64 ? 24u : 16u) || st.off > data.size() || esz > data.size() - st.off) return 0;
        const uint64_t max_idx = (data.size() - static_cast<size_t>(st.off) - static_cast<size_t>(esz)) / esz;
        if (sym_idx > max_idx) return 0;
        const size_t pos = static_cast<size_t>(st.off) + static_cast<size_t>(uint64_t(sym_idx) * esz);
        return is64 ? rd64(p + pos + 8, le) : rd32(p + pos + 4, le);
    };

    for (const ElfSec &s : shdrs) {
        if ((s.type != 2u && s.type != 11u) || s.link >= shdrs.size()) continue;
        const ElfSec &str_sec = shdrs[s.link];
        if (str_sec.off > data.size() || str_sec.size > data.size() - str_sec.off) continue;
        if (s.off > data.size() || s.size > data.size() - s.off) continue;
        const std::string_view strtab = data.substr(static_cast<size_t>(str_sec.off), static_cast<size_t>(str_sec.size));
        const uint64_t entsz = s.entsize ? s.entsize : (is64 ? 24u : 16u);
        if (entsz < (is64 ? 24u : 16u)) continue;
        const uint64_t n_ent = s.size / entsz;
        for (uint64_t ei = 0; ei < n_ent; ++ei) {
            const size_t pos = static_cast<size_t>(s.off) + static_cast<size_t>(ei * entsz);
            const uint8_t *ep = p + pos;
            uint32_t st_name = 0;
            uint8_t st_info = 0;
            uint16_t st_shndx = 0;
            uint64_t st_size = 0;
            if (is64) {
                st_name = rd32(ep, le);
                st_info = ep[4];
                st_shndx = rd16(ep + 6, le);
                st_size = rd64(ep + 16, le);
            } else {
                st_name = rd32(ep, le);
                st_size = rd32(ep + 8, le);
                st_info = ep[12];
                st_shndx = rd16(ep + 14, le);
            }
            const std::string nm = cstr_at(strtab, st_name);
            if (nm.empty()) continue;
            out.raw_names.insert(nm);
            const uint8_t bind = st_info >> 4;
            const uint8_t stt = st_info & 0x0Fu;
            if (stt == 3u || stt == 4u) continue;
            const bool defined = (st_shndx != 0);
            const bool exported = (bind == 1u || bind == 2u || bind == 10u);
            const std::string kind = (stt == 2u || stt == 10u) ? "func"
                                     : (stt == 1u || stt == 5u || stt == 6u) ? "object" : "notype";
            merge_sym(out.symbols, nm, defined, exported, kind, st_size);
        }
    }

    DwarfSections dw;
    dw.le = le;
    int info_idx = -1, str_offs_idx = -1;
    for (size_t i = 0; i < shdrs.size(); ++i) {
        const ElfSec &s = shdrs[i];
        if (s.type == 0 || s.type == 8 /* SHT_NOBITS */) continue;
        const std::string sname = cstr_at(shstrtab, s.name_off);
        if (sname == ".debug_info" || sname == ".zdebug_info") {
            if (!extract_elf_sec_bytes(data, s.flags, s.off, s.size, is64, le, sname, dw.debug_info, err))
                return false;
            info_idx = static_cast<int>(i);
        } else if (sname == ".debug_abbrev" || sname == ".zdebug_abbrev") {
            if (!extract_elf_sec_bytes(data, s.flags, s.off, s.size, is64, le, sname, dw.debug_abbrev, err))
                return false;
        } else if (sname == ".debug_str" || sname == ".zdebug_str") {
            if (!extract_elf_sec_bytes(data, s.flags, s.off, s.size, is64, le, sname, dw.debug_str, err))
                return false;
        } else if (sname == ".debug_line_str" || sname == ".zdebug_line_str") {
            if (!extract_elf_sec_bytes(data, s.flags, s.off, s.size, is64, le, sname, dw.debug_line_str, err))
                return false;
        } else if (sname == ".debug_str_offsets" || sname == ".zdebug_str_offsets") {
            if (!extract_elf_sec_bytes(data, s.flags, s.off, s.size, is64, le, sname, dw.debug_str_offsets, err))
                return false;
            str_offs_idx = static_cast<int>(i);
        }
    }

    auto apply_rela_to = [&](int target_shndx, std::string &sec_buf) {
        if (target_shndx < 0 || sec_buf.empty()) return;
        for (const ElfSec &rs : shdrs) {
            if ((rs.type != 4u /* SHT_RELA */ && rs.type != 9u /* SHT_REL */) ||
                int(rs.info) != target_shndx)
                continue;
            if (rs.off > data.size() || rs.size > data.size() - rs.off) continue;
            const bool is_rela = (rs.type == 4u);
            const uint64_t rsz = rs.entsize ? rs.entsize
                                            : (is64 ? (is_rela ? 24u : 16u) : (is_rela ? 12u : 8u));
            if (rsz < (is64 ? (is_rela ? 24u : 16u) : (is_rela ? 12u : 8u))) continue;
            const uint64_t n_rel = rs.size / rsz;
            for (uint64_t ri = 0; ri < n_rel; ++ri) {
                const size_t pos = static_cast<size_t>(rs.off) + static_cast<size_t>(ri * rsz);
                const uint8_t *rp = p + pos;
                uint64_t r_off = 0, r_sym = 0, r_type = 0;
                int64_t r_add = 0;
                if (is64) {
                    r_off = rd64(rp, le);
                    const uint64_t r_info = rd64(rp + 8, le);
                    r_sym = r_info >> 32;
                    r_type = r_info & 0xFFFFFFFFu;
                    if (is_rela) r_add = static_cast<int64_t>(rd64(rp + 16, le));
                } else {
                    r_off = rd32(rp, le);
                    const uint32_t r_info = rd32(rp + 4, le);
                    r_sym = r_info >> 8;
                    r_type = r_info & 0xFFu;
                    if (is_rela) r_add = static_cast<int32_t>(rd32(rp + 8, le));
                }
                if (r_type == 0) continue;
                const uint64_t sym_v = read_sym_val(rs.link, static_cast<uint32_t>(r_sym));
                const bool wide64 = is64 &&
                    ((e_machine == 62 && r_type == 1u /* R_X86_64_64 */) ||
                     (e_machine == 183 && r_type == 257u /* R_AARCH64_ABS64 */) ||
                     (e_machine == 243 && r_type == 2u /* R_RISCV_64 */));
                if (wide64) {
                    if (r_off <= sec_buf.size() && 8u <= sec_buf.size() - static_cast<size_t>(r_off)) {
                        auto *dst = reinterpret_cast<uint8_t *>(sec_buf.data() + static_cast<size_t>(r_off));
                        const uint64_t base = is_rela ? 0u : rd64(dst, le);
                        wr64(dst, base + sym_v + static_cast<uint64_t>(r_add), le);
                    }
                } else if (r_off <= sec_buf.size() && 4u <= sec_buf.size() - static_cast<size_t>(r_off)) {
                    auto *dst = reinterpret_cast<uint8_t *>(sec_buf.data() + static_cast<size_t>(r_off));
                    const uint32_t base = is_rela ? 0u : rd32(dst, le);
                    wr32(dst, base + static_cast<uint32_t>(sym_v + static_cast<uint64_t>(r_add)), le);
                }
            }
        }
    };

    apply_rela_to(info_idx, dw.debug_info);
    apply_rela_to(str_offs_idx, dw.debug_str_offsets);
    if (!parse_dwarf_sections(dw, out.dwarf_fns, out.dwarf_structs, err)) return false;
    return true;
}

bool parse_macho_bytes(std::string_view data, ObjectInfo &out, std::string &err) {
    if (data.size() < 28) {
        err = "truncated Mach-O header";
        return false;
    }
    const auto *p = reinterpret_cast<const uint8_t *>(data.data());
    const uint32_t magic = rd32(p, true);
    bool le = true, is64 = false;
    if (magic == 0xFEEDFACEu || magic == 0xFEEDFACFu) {
        le = true;
        is64 = (magic == 0xFEEDFACFu);
    } else if (magic == 0xCEFAEDFEu || magic == 0xCFFAEDFEu) {
        le = false;
        is64 = (magic == 0xCFFAEDFEu);
    } else {
        err = "not a Mach-O object";
        return false;
    }
    if (is64 && data.size() < 32) {
        err = "truncated Mach-O 64 header";
        return false;
    }
    out.format = "macho";
    out.ptr_width = is64 ? 8 : 4;
    const uint32_t cputype = rd32(p + 4, le);
    switch (cputype) {
        case 7u: out.arch = "x86"; break;
        case 0x01000007u: out.arch = "x86_64"; break;
        case 12u: out.arch = "arm"; break;
        case 0x0100000Cu: out.arch = "aarch64"; break;
        default: out.arch = "macho-" + std::to_string(int32_t(cputype)); break;
    }
    const uint32_t ncmds = rd32(p + 16, le);
    size_t off = is64 ? 32u : 28u;
    std::vector<std::string> sec_kinds{""};
    struct SymCmd { uint32_t symoff, nsyms, stroff, strsize; };
    std::vector<SymCmd> sym_cmds;
    DwarfSections dw;
    dw.le = le;

    for (uint32_t ci = 0; ci < ncmds; ++ci) {
        if (off + 8 > data.size()) break;
        const uint32_t cmd = rd32(p + off, le);
        const uint32_t cmdsize = rd32(p + off + 4, le);
        if (cmdsize < 8 || cmdsize > data.size() - off) break;
        if (cmd == 0x1u || cmd == 0x19u) {
            const bool seg64 = (cmd == 0x19u);
            const size_t hdr_sz = seg64 ? 72u : 56u;
            const size_t sec_sz = seg64 ? 80u : 68u;
            if (cmdsize >= hdr_sz) {
                const uint32_t nsects = rd32(p + off + (seg64 ? 64u : 48u), le);
                size_t soff = off + hdr_sz;
                for (uint32_t si = 0; si < nsects; ++si) {
                    if (soff + sec_sz > off + cmdsize) break;
                    std::string sectname(reinterpret_cast<const char *>(p + soff),
                                         strnlen(reinterpret_cast<const char *>(p + soff), 16));
                    std::string segname(reinterpret_cast<const char *>(p + soff + 16),
                                        strnlen(reinterpret_cast<const char *>(p + soff + 16), 16));
                    sec_kinds.push_back(segname == "__TEXT" ? "func" : "object");
                    const uint64_t s_size = seg64 ? rd64(p + soff + 40, le) : rd32(p + soff + 36, le);
                    const uint32_t s_foff = rd32(p + soff + (seg64 ? 48u : 40u), le);
                    if (segname == "__DWARF" && s_foff <= data.size() && s_size <= data.size() - s_foff) {
                        const std::string_view sv = data.substr(s_foff, static_cast<size_t>(s_size));
                        if (sectname == "__debug_info") dw.debug_info.assign(sv.data(), sv.size());
                        else if (sectname == "__debug_abbrev") dw.debug_abbrev.assign(sv.data(), sv.size());
                        else if (sectname == "__debug_str") dw.debug_str.assign(sv.data(), sv.size());
                        else if (sectname == "__debug_line_str") dw.debug_line_str.assign(sv.data(), sv.size());
                        else if (sectname == "__debug_str_offs") dw.debug_str_offsets.assign(sv.data(), sv.size());
                    }
                    soff += sec_sz;
                }
            }
        } else if (cmd == 0x2u && cmdsize >= 24) {
            sym_cmds.push_back(SymCmd{
                rd32(p + off + 8, le), rd32(p + off + 12, le),
                rd32(p + off + 16, le), rd32(p + off + 20, le),
            });
        }
        off += cmdsize;
    }

    const size_t entsz = is64 ? 16u : 12u;
    for (const SymCmd &sc : sym_cmds) {
        if (sc.stroff > data.size() || sc.strsize > data.size() - sc.stroff) continue;
        const std::string_view strtab = data.substr(sc.stroff, sc.strsize);
        for (uint32_t i = 0; i < sc.nsyms; ++i) {
            const uint64_t pos = uint64_t(sc.symoff) + uint64_t(i) * entsz;
            if (pos + entsz > data.size()) break;
            const uint32_t n_strx = rd32(p + pos, le);
            const uint8_t n_type = p[pos + 4];
            const uint8_t n_sect = p[pos + 5];
            if (n_type & 0xE0u) continue;
            const std::string raw_nm = cstr_at(strtab, n_strx);
            if (raw_nm.empty()) continue;
            out.raw_names.insert(raw_nm);
            const std::string nm = (raw_nm.front() == '_') ? raw_nm.substr(1) : raw_nm;
            const uint8_t ntype = n_type & 0x0Eu;
            const bool defined = (ntype == 0x0Eu || ntype == 0x02u);
            const bool exported = (n_type & 0x01u) != 0;
            const std::string kind = (defined && n_sect > 0 && n_sect < sec_kinds.size())
                                         ? sec_kinds[n_sect]
                                         : "notype";
            merge_sym(out.symbols, nm, defined, exported, kind.empty() ? "notype" : kind, 0);
        }
    }
    if (!parse_dwarf_sections(dw, out.dwarf_fns, out.dwarf_structs, err)) return false;
    return true;
}

bool parse_pe_coff_bytes(std::string_view data, ObjectInfo &out, std::string &err) {
    const auto *p = reinterpret_cast<const uint8_t *>(data.data());
    size_t coff_off = 0;
    bool is_pe = false;
    if (data.size() >= 64 && p[0] == 'M' && p[1] == 'Z') {
        const uint32_t pe_off = rd32(p + 0x3C, true);
        if (pe_off <= data.size() - 24 && std::memcmp(p + pe_off, "PE\0\0", 4) == 0) {
            coff_off = pe_off + 4;
            is_pe = true;
        } else {
            err = "invalid PE header";
            return false;
        }
    }
    if (data.size() - coff_off < 20) {
        err = "not a COFF/PE object";
        return false;
    }
    const uint16_t machine = rd16(p + coff_off, true);
    const uint16_t nsects = rd16(p + coff_off + 2, true);
    const uint32_t symptr = rd32(p + coff_off + 8, true);
    const uint32_t nsyms = rd32(p + coff_off + 12, true);
    const uint16_t optsize = rd16(p + coff_off + 16, true);
    switch (machine) {
        case 0x014Cu: out.arch = "x86"; out.ptr_width = 4; break;
        case 0x8664u: out.arch = "x86_64"; out.ptr_width = 8; break;
        case 0x01C0u: out.arch = "arm"; out.ptr_width = 4; break;
        case 0xAA64u: out.arch = "aarch64"; out.ptr_width = 8; break;
        default:
            err = "unrecognized binary format";
            return false;
    }
    if (data.size() - coff_off - 20 < optsize) {
        err = "truncated COFF/PE optional header";
        return false;
    }
    out.format = is_pe ? "pe" : "coff";

    std::string_view strtab;
    if (symptr && nsyms && uint64_t(symptr) <= data.size() &&
        uint64_t(nsyms) * 18u <= data.size() - symptr) {
        const size_t str_off = static_cast<size_t>(uint64_t(symptr) + uint64_t(nsyms) * 18u);
        strtab = data.substr(str_off);
    }

    const size_t sec_off = coff_off + 20 + optsize;
    std::vector<bool> sec_exec{false};
    struct PeSec { uint32_t va, vsz, rptr, rsz; };
    std::vector<PeSec> sects;
    DwarfSections dw;
    dw.le = true;

    for (uint16_t i = 0; i < nsects; ++i) {
        if (data.size() - sec_off < uint64_t(i + 1u) * 40u) break;
        const size_t so = sec_off + size_t(i) * 40u;
        std::string sname(reinterpret_cast<const char *>(p + so),
                          strnlen(reinterpret_cast<const char *>(p + so), 8));
        if (!sname.empty() && sname[0] == '/' && !strtab.empty()) {
            size_t off_n = 0;
            bool ok_n = sname.size() > 1;
            for (size_t k = 1; k < sname.size(); ++k) {
                if (sname[k] < '0' || sname[k] > '9') { ok_n = false; break; }
                if (off_n > strtab.size() / 10u) { ok_n = false; break; }
                off_n = off_n * 10 + size_t(sname[k] - '0');
            }
            if (ok_n) sname = cstr_at(strtab, off_n);
        }
        const uint32_t vsize = rd32(p + so + 8, true);
        const uint32_t vaddr = rd32(p + so + 12, true);
        const uint32_t rawsize = rd32(p + so + 16, true);
        const uint32_t rawptr = rd32(p + so + 20, true);
        const uint32_t schars = rd32(p + so + 36, true);
        sec_exec.push_back((schars & 0x20000020u) != 0);
        sects.push_back(PeSec{vaddr, std::max(vsize, rawsize), rawptr, rawsize});
        const uint32_t eff_sz = (vsize > 0 && vsize < rawsize) ? vsize : rawsize;
        if (rawptr <= data.size() && eff_sz <= data.size() - rawptr) {
            const std::string_view sv = data.substr(rawptr, eff_sz);
            if (sname == ".debug_info") dw.debug_info.assign(sv.data(), sv.size());
            else if (sname == ".debug_abbrev") dw.debug_abbrev.assign(sv.data(), sv.size());
            else if (sname == ".debug_str") dw.debug_str.assign(sv.data(), sv.size());
            else if (sname == ".debug_line_str") dw.debug_line_str.assign(sv.data(), sv.size());
            else if (sname == ".debug_str_offsets") dw.debug_str_offsets.assign(sv.data(), sv.size());
        }
    }

    if (symptr && nsyms) {
        uint64_t idx = 0;
        while (idx < nsyms) {
            const uint64_t pos = uint64_t(symptr) + idx * 18u;
            if (pos > data.size() || data.size() - static_cast<size_t>(pos) < 18u) break;
            const uint8_t *sp = p + pos;
            const int16_t scnum = static_cast<int16_t>(rd16(sp + 12, true));
            const uint16_t symtype = rd16(sp + 14, true);
            const uint8_t sclass = sp[16];
            const uint8_t naux = sp[17];
            idx += 1u + uint64_t(naux);
            std::string raw_nm;
            if (rd32(sp, true) == 0) {
                raw_nm = cstr_at(strtab, rd32(sp + 4, true));
            } else {
                raw_nm.assign(reinterpret_cast<const char *>(sp),
                              strnlen(reinterpret_cast<const char *>(sp), 8));
            }
            if (raw_nm.empty()) continue;
            out.raw_names.insert(raw_nm);
            const std::string nm = (out.ptr_width == 4 && raw_nm.front() == '_') ? raw_nm.substr(1) : raw_nm;
            const bool defined = (scnum > 0 || scnum == -1);
            const bool exported = (sclass == 2u);
            const bool is_fn = (symtype == 0x20u) ||
                               (scnum > 0 && size_t(scnum) < sec_exec.size() && sec_exec[size_t(scnum)]);
            const std::string kind = is_fn ? "func" : (defined ? "object" : "notype");
            merge_sym(out.symbols, nm, defined, exported, kind, 0);
        }
    }

    if (is_pe && optsize >= 96) {
        const size_t opt_off = coff_off + 20;
        const uint16_t opt_magic = rd16(p + opt_off, true);
        const size_t dd_off = opt_off + ((opt_magic == 0x20Bu) ? 112u : 96u);
        if (dd_off + 8 <= coff_off + 20 + optsize) {
            const uint32_t exp_rva = rd32(p + dd_off, true);
            const uint32_t exp_sz = rd32(p + dd_off + 4, true);
            auto rva_to_off = [&](uint32_t rva) -> std::optional<size_t> {
                for (const PeSec &s : sects) {
                    const uint64_t span = std::max<uint32_t>(s.vsz, 1u);
                    if (rva >= s.va && uint64_t(rva) < uint64_t(s.va) + span) {
                        const uint64_t res = uint64_t(s.rptr) + (uint64_t(rva) - s.va);
                        if (res < data.size()) return static_cast<size_t>(res);
                        return std::nullopt;
                    }
                }
                return std::nullopt;
            };
            const auto eoff = (exp_rva && exp_sz) ? rva_to_off(exp_rva) : std::nullopt;
            if (eoff && data.size() - *eoff >= 40) {
                const uint32_t num_funcs = rd32(p + *eoff + 20, true);
                const uint32_t num_names = rd32(p + *eoff + 24, true);
                const auto foff = rva_to_off(rd32(p + *eoff + 28, true));
                const auto noff = rva_to_off(rd32(p + *eoff + 32, true));
                const auto ooff = rva_to_off(rd32(p + *eoff + 36, true));
                if (foff && noff && ooff) {
                    const uint32_t cap = std::min<uint32_t>(num_names, 65536u);
                    for (uint32_t i = 0; i < cap; ++i) {
                        if (data.size() - *noff < uint64_t(i + 1u) * 4u ||
                            data.size() - *ooff < uint64_t(i + 1u) * 2u) break;
                        const auto nptr = rva_to_off(rd32(p + *noff + i * 4u, true));
                        const uint16_t ord_i = rd16(p + *ooff + i * 2u, true);
                        if (!nptr) continue;
                        const std::string nm = cstr_at(data, *nptr);
                        if (nm.empty()) continue;
                        out.raw_names.insert(nm);
                        std::string kind = "func";
                        if (ord_i < num_funcs && data.size() - *foff >= (uint64_t(ord_i) + 1u) * 4u) {
                            const uint32_t frva = rd32(p + *foff + uint32_t(ord_i) * 4u, true);
                            for (size_t si = 0; si < sects.size(); ++si) {
                                const uint64_t span = std::max<uint32_t>(sects[si].vsz, 1u);
                                if (frva >= sects[si].va && uint64_t(frva) < uint64_t(sects[si].va) + span) {
                                    kind = sec_exec[si + 1] ? "func" : "object";
                                    break;
                                }
                            }
                        }
                        merge_sym(out.symbols, nm, true, true, kind, 0);
                    }
                }
            }
        }
    }

    if (!parse_dwarf_sections(dw, out.dwarf_fns, out.dwarf_structs, err)) return false;
    return true;
}

bool parse_single_object(std::string_view data, ObjectInfo &out, std::string &err) {
    if (data.size() >= 4 && std::memcmp(data.data(), "\x7f""ELF", 4) == 0)
        return parse_elf_bytes(data, out, err);
    if (data.size() >= 4) {
        const uint32_t m = rd32(reinterpret_cast<const uint8_t *>(data.data()), true);
        if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCEFAEDFEu || m == 0xCFFAEDFEu)
            return parse_macho_bytes(data, out, err);
    }
    return parse_pe_coff_bytes(data, out, err);
}

}  // namespace

bool inspect_object_file(const std::string &path, ObjectInfo &out, std::string &err) {
    out = ObjectInfo{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        err = "cannot read " + path;
        return false;
    }
    if (data.size() >= 8 && std::memcmp(data.data(), "!<arch>\n", 8) == 0) {
        size_t pos = 8;
        bool any_member = false;
        while (pos < data.size()) {
            if (pos + 1 == data.size() && data[pos] == '\n') break;
            if (data.size() - pos < 60) {
                err = "corrupt archive " + path + ": truncated member header";
                return false;
            }
            const std::string_view hdr(data.data() + pos, 60);
            if (hdr.substr(58, 2) != "`\n") {
                err = "corrupt archive " + path + ": invalid member magic";
                return false;
            }
            std::string name_field(hdr.substr(0, 16));
            while (!name_field.empty() && name_field.back() == ' ') name_field.pop_back();
            std::string sz_s(hdr.substr(48, 10));
            while (!sz_s.empty() && sz_s.back() == ' ') sz_s.pop_back();
            size_t sz = 0;
            if (sz_s.empty()) {
                err = "corrupt archive " + path + ": missing member size";
                return false;
            }
            bool ok_sz = true;
            for (char c : sz_s) {
                if (c < '0' || c > '9') { ok_sz = false; break; }
                if (sz > data.size() / 10u) { ok_sz = false; break; }
                sz = sz * 10 + size_t(c - '0');
                if (sz > data.size()) { ok_sz = false; break; }
            }
            if (!ok_sz) {
                err = "corrupt archive " + path + ": invalid member size";
                return false;
            }
            const size_t body_start = pos + 60;
            if (sz > data.size() - body_start) {
                err = "corrupt archive " + path + ": truncated member payload";
                return false;
            }
            std::string_view member(data.data() + body_start, sz);
            pos = body_start + sz;
            if ((sz & 1u) && pos < data.size()) ++pos;
            if (name_field.rfind("#1/", 0) == 0) {
                size_t nlen = 0;
                bool ok_nl = name_field.size() > 3;
                for (size_t k = 3; k < name_field.size(); ++k) {
                    if (name_field[k] < '0' || name_field[k] > '9') { ok_nl = false; break; }
                    if (nlen > member.size() / 10u) { ok_nl = false; break; }
                    nlen = nlen * 10 + size_t(name_field[k] - '0');
                }
                if (!ok_nl || nlen > member.size()) {
                    err = "corrupt archive " + path + ": invalid BSD extended member name";
                    return false;
                }
                const size_t z = member.substr(0, nlen).find('\0');
                name_field.assign(member.substr(0, z == std::string_view::npos ? nlen : z));
                member.remove_prefix(nlen);
            }
            if (name_field == "/" || name_field == "//" || name_field == "/SYM64/" ||
                name_field.rfind("__.SYMDEF", 0) == 0 || member.empty())
                continue;
            ObjectInfo sub;
            std::string sub_err;
            if (!parse_single_object(member, sub, sub_err)) {
                err = "corrupt member in archive " + path + ": " + sub_err;
                return false;
            }
            if (!any_member) {
                out.format = "ar";
                out.ptr_width = sub.ptr_width;
                out.arch = sub.arch;
                any_member = true;
            } else if (sub.ptr_width != out.ptr_width || sub.arch != out.arch) {
                err = "archive " + path + " mixes incompatible object architectures (" +
                      out.arch + " vs " + sub.arch + ")";
                return false;
            }
            out.raw_names.insert(sub.raw_names.begin(), sub.raw_names.end());
            for (const auto &[_, sym] : sub.symbols) {
                merge_sym(out.symbols, sym.raw, sym.defined, sym.exported, sym.kind, sym.size);
            }
            for (auto &[fnm, fsig] : sub.dwarf_fns) out.dwarf_fns[fnm] = std::move(fsig);
            for (auto &[snm, srec] : sub.dwarf_structs) out.dwarf_structs[snm] = std::move(srec);
        }
        if (!any_member) {
            err = "archive " + path + " contains no recognizable object members";
            return false;
        }
        return true;
    }
    return parse_single_object(data, out, err);
}

}  // namespace native_ffi
