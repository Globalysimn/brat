#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace native_ffi {

struct ObjectSym {
    std::string name;
    std::string raw;
    bool defined = false;
    bool exported = false;
    std::string kind;  // "func", "object", "notype"
    uint64_t size = 0;
};

struct DwarfSlot;

struct DwarfFnSig {
    std::shared_ptr<DwarfSlot> ret;
    std::vector<DwarfSlot> params;
    bool variadic = false;
};

struct DwarfSlot {
    std::string category = "unknown";  // "void", "scalar", "float", "struct", "array", "fn", "unknown"
    std::optional<int64_t> byte_width;
    std::string name;
    std::shared_ptr<DwarfFnSig> fn;
};

struct DwarfMember {
    std::string name;
    int64_t offset = 0;
    DwarfSlot slot;
};

struct DwarfRecord {
    int64_t size = 0;
    bool is_union = false;
    std::vector<DwarfMember> members;
};

struct ObjectInfo {
    std::string format;
    int ptr_width = 8;
    std::string arch;
    std::map<std::string, ObjectSym> symbols;
    std::set<std::string> raw_names;
    std::map<std::string, DwarfFnSig> dwarf_fns;
    std::map<std::string, DwarfRecord> dwarf_structs;
};

bool inspect_object_file(const std::string &path, ObjectInfo &out, std::string &err);

}  // namespace native_ffi
