// A store-only (uncompressed) zip writer for the log zip (spec section 11), and CRC-32. No deflate code is needed for a
// file the user only sends on. Names are stored with '/' (memory tester-zip-forward-slashes) and the UTF-8 flag. Entries
// and the archive stay under 4 GB (the log set is the newest few logs and dumps). Pure and header-only - tested by
// src_beta/src/tests/launcher_logs_test.cpp, which lists and extracts the result with Windows' own tar.exe.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dlvr::launcher {

inline uint32_t Crc32(const std::string& data)
{
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) { c = table[(c ^ b) & 0xFF] ^ (c >> 8); }
    return c ^ 0xFFFFFFFFu;
}

namespace detail {

inline void U16le(std::string* o, uint32_t v) { o->push_back(static_cast<char>(v & 0xFF)); o->push_back(static_cast<char>((v >> 8) & 0xFF)); }
inline void U32le(std::string* o, uint32_t v) { U16le(o, v & 0xFFFF); U16le(o, v >> 16); }

}  // namespace detail

// entries = (name in the zip, bytes). Backslashes in names become '/'.
inline std::string ZipStore(const std::vector<std::pair<std::string, std::string>>& entries)
{
    using detail::U16le;
    using detail::U32le;
    const uint32_t kDosTime = 0, kDosDate = (0u << 9) | (1u << 5) | 1u;   // 1980-01-01
    const uint32_t kUtf8 = 0x0800;
    std::string out, central;
    for (const auto& e : entries) {
        std::string name = e.first;
        for (char& c : name) { if (c == '\\') { c = '/'; } }
        const uint32_t crc = Crc32(e.second);
        const uint32_t size = static_cast<uint32_t>(e.second.size());
        const uint32_t offset = static_cast<uint32_t>(out.size());
        U32le(&out, 0x04034b50u);
        U16le(&out, 20); U16le(&out, kUtf8); U16le(&out, 0); U16le(&out, kDosTime); U16le(&out, kDosDate);
        U32le(&out, crc); U32le(&out, size); U32le(&out, size);
        U16le(&out, static_cast<uint32_t>(name.size())); U16le(&out, 0);
        out += name;
        out += e.second;
        U32le(&central, 0x02014b50u);
        U16le(&central, 20); U16le(&central, 20); U16le(&central, kUtf8); U16le(&central, 0);
        U16le(&central, kDosTime); U16le(&central, kDosDate);
        U32le(&central, crc); U32le(&central, size); U32le(&central, size);
        U16le(&central, static_cast<uint32_t>(name.size())); U16le(&central, 0); U16le(&central, 0);
        U16le(&central, 0); U16le(&central, 0); U32le(&central, 0); U32le(&central, offset);
        central += name;
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out += central;
    U32le(&out, 0x06054b50u);
    U16le(&out, 0); U16le(&out, 0);
    U16le(&out, static_cast<uint32_t>(entries.size())); U16le(&out, static_cast<uint32_t>(entries.size()));
    U32le(&out, static_cast<uint32_t>(central.size())); U32le(&out, cdOffset);
    U16le(&out, 0);
    return out;
}

}  // namespace dlvr::launcher
