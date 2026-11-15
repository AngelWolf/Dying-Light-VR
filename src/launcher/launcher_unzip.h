// A zip READER for a release's files.zip (spec section 10): the central directory, stored and deflated entries (inflated
// by the vendored puff, third_party/puff), every entry's CRC-32 checked. A truncated or damaged zip is REFUSED, naming the
// entry - never a partial staging. No ZIP64 (a release is far under 4 GB and 65535 entries). Pure and header-only (needs
// puff.c linked) - tested by src_beta/src/tests/launcher_update_test.cpp on a deflated zip written by Windows' tar.exe.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "launcher_zip.h"

extern "C" {
#include "puff.h"
}

namespace dlvr::launcher {

struct ZipEntry {
    std::string name;
    uint32_t method = 0;     // 0 stored, 8 deflated
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0;
    uint64_t dataOffset = 0;
};

namespace detail {

inline uint32_t Rd16(const std::string& z, size_t p) { return static_cast<uint8_t>(z[p]) | (static_cast<uint8_t>(z[p + 1]) << 8); }
inline uint32_t Rd32(const std::string& z, size_t p) { return Rd16(z, p) | (Rd16(z, p + 2) << 16); }

}  // namespace detail

inline bool ZipEntries(const std::string& z, std::vector<ZipEntry>* out, std::string* why)
{
    using detail::Rd16;
    using detail::Rd32;
    out->clear();
    if (z.size() < 22) { *why = "the zip is too short to be a zip"; return false; }
    size_t eocd = std::string::npos;
    const size_t stop = z.size() > 65557 ? z.size() - 65557 : 0;
    for (size_t p = z.size() - 22 + 1; p-- > stop;) {
        if (Rd32(z, p) == 0x06054b50u) { eocd = p; break; }
    }
    if (eocd == std::string::npos) { *why = "no end-of-directory record (the zip is truncated or not a zip)"; return false; }
    const uint32_t count = Rd16(z, eocd + 10);
    const uint32_t cdSize = Rd32(z, eocd + 12);
    const uint32_t cdOff = Rd32(z, eocd + 16);
    if (static_cast<uint64_t>(cdOff) + cdSize > eocd) { *why = "the zip's directory lies outside the file (truncated?)"; return false; }
    size_t p = cdOff;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > eocd || Rd32(z, p) != 0x02014b50u) { *why = "damaged directory entry " + std::to_string(i); return false; }
        ZipEntry e;
        e.method = Rd16(z, p + 10);
        e.crc = Rd32(z, p + 16);
        e.csize = Rd32(z, p + 20);
        e.usize = Rd32(z, p + 24);
        const uint32_t nl = Rd16(z, p + 28), xl = Rd16(z, p + 30), cl = Rd16(z, p + 32);
        const uint32_t local = Rd32(z, p + 42);
        if (p + 46 + nl > eocd) { *why = "damaged directory entry " + std::to_string(i); return false; }
        e.name = z.substr(p + 46, nl);
        p += 46 + nl + xl + cl;
        if (static_cast<uint64_t>(local) + 30 > z.size() || Rd32(z, local) != 0x04034b50u) {
            *why = e.name + ": its local header is missing (truncated?)";
            return false;
        }
        e.dataOffset = static_cast<uint64_t>(local) + 30 + Rd16(z, local + 26) + Rd16(z, local + 28);
        if (e.dataOffset + e.csize > z.size()) { *why = e.name + ": its data runs past the end of the zip (truncated)"; return false; }
        if (e.method != 0 && e.method != 8) { *why = e.name + ": compression method " + std::to_string(e.method) + " is not supported"; return false; }
        if (!e.name.empty() && e.name.back() == '/') { continue; }   // a folder entry
        out->push_back(e);
    }
    return true;
}

inline bool ZipExtract(const std::string& z, const ZipEntry& e, std::string* data, std::string* why)
{
    data->clear();
    if (e.dataOffset + e.csize > z.size()) { *why = e.name + ": truncated"; return false; }
    if (e.method == 0) {
        *data = z.substr(static_cast<size_t>(e.dataOffset), static_cast<size_t>(e.csize));
    } else {
        data->resize(static_cast<size_t>(e.usize));
        unsigned long dlen = static_cast<unsigned long>(e.usize);
        unsigned long slen = static_cast<unsigned long>(e.csize);
        const int rc = puff(reinterpret_cast<unsigned char*>(data->data()), &dlen,
                            reinterpret_cast<const unsigned char*>(z.data() + e.dataOffset), &slen);
        if (rc != 0 || dlen != e.usize) {
            *why = e.name + ": could not be inflated (puff " + std::to_string(rc) + ")";
            data->clear();
            return false;
        }
    }
    if (Crc32(*data) != e.crc) {
        *why = e.name + ": CRC mismatch (the zip is damaged)";
        data->clear();
        return false;
    }
    return true;
}

}  // namespace dlvr::launcher
