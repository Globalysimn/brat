#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pkg_io {

inline constexpr size_t kMaxPackageBytes = 16u * 1024u * 1024u;
inline constexpr size_t kMaxTarStreamBytes = 64u * 1024u * 1024u;
inline constexpr size_t kMaxGraphArchiveBytes = 64u * 1024u * 1024u;
inline constexpr size_t kMaxGraphExtractedBytes = 128u * 1024u * 1024u;
inline constexpr size_t kMaxGraphFiles = 4096u;
inline constexpr size_t kMaxIndexGraphPackages = 256u;

std::string sha256_hex(std::string_view data);
bool sha256_file(const std::string &path, std::string &hex_out);

bool inflate_raw(const uint8_t *src, size_t len, size_t &consumed,
                 std::string &out, size_t max_out, std::string &err);
bool inflate_zlib(const uint8_t *src, size_t len,
                  std::string &out, size_t max_out, std::string &err);
bool gunzip(std::string_view gz, std::string &out, size_t max_out, std::string &err);

struct TarMember {
    std::string raw_name;
    std::vector<std::string> segs;
    std::string data;
};

bool extract_targz_raw(std::string_view gz_bytes,
                       std::vector<TarMember> &members,
                       std::string &err,
                       size_t max_payload_bytes = kMaxPackageBytes,
                       size_t max_entries = 512);

bool fetch_url(const std::string &url,
               std::string &out_bytes,
               std::string &err,
               bool allow_file = true,
               size_t max_bytes = kMaxPackageBytes,
               bool block_internal_root = false,
               bool *connected_internal_out = nullptr);

bool write_ar_bundle(const std::string &out_path,
                     const std::vector<std::string> &obj_paths,
                     std::string &err);

}  // namespace pkg_io
