// calibnative_logic.h - PURE, header-only helpers for reading the calibration screen's native font from the player's own
// install (build D, ruling D9): zlib / raw-deflate inflate (puff), a ZIP entry locator, the RP6L (.rpack) directory
// parser, and the BC3 alpha decoder. No file I/O, no engine, no logging here - every function works over byte buffers the
// caller read, and every refusal NAMES its reason in `why`. Tested by src/src/tests/calibnative_logic_test.cpp against
// tools/calib_native_font.py (the Python version of exactly this chain, proven on menu_common_PC.rpack).
//
// THREADING: puff builds its fixed-Huffman tables lazily on first use and is not thread-safe on that first call. The caller
// (the one-shot loader thread, Task 4) serialises: there is exactly one inflate user.
// COST: nothing here is per-frame; Task 4 runs it once on the loader thread. Inflating the rpack's 72 MB section to the
// 8.66 MB prefix we need is the expensive call (hundreds of ms), and it never runs on the render thread.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../third_party/puff/puff.h"
}

namespace dlvr {
namespace calibnative {

// ---------------------------------------------------------------- helpers
inline bool Fail(char* why, size_t whyN, const char* fmt, ...)
{
    if (why && whyN) {
        va_list ap; va_start(ap, fmt);
        std::vsnprintf(why, whyN, fmt, ap);
        va_end(ap);
        why[whyN - 1] = 0;
    }
    return false;
}
inline uint16_t Rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

inline uint32_t Crc32(const uint8_t* p, size_t n)
{
    struct Table { uint32_t t[256]; Table() { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1); t[i] = c; } } };
    static const Table tab;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab.t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
inline uint32_t Adler32(const uint8_t* p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n;) {
        const size_t blk = (n - i < 5552) ? (n - i) : 5552; // 5552: the most bytes before b can overflow 32 bits
        for (size_t k = 0; k < blk; ++k) { a += p[i + k]; b += a; }
        a %= 65521u; b %= 65521u; i += blk;
    }
    return (b << 16) | a;
}

// puff takes `unsigned long` (32 bit on Windows) sizes; a want past this is refused rather than wrapped.
static const size_t kPuffMax = 0x7FFF0000u;
// puff returns 1 ("output exhausted") WITHOUT writing the symbol / stored block that does not fit (a match is up to 258
// bytes, a stored block up to 65535), and does not report how much it wrote. Giving it this much extra room means that when
// it stops it has always written at least `want` bytes, so truncating to `want` is exact.
static const size_t kPuffSlack = 65535 + 258;

inline const char* PuffName(int r)
{
    switch (r) {
    case 2: return "ran out of input";
    case -1: return "invalid block type";
    case -2: return "stored block length mismatch";
    case -3: return "dynamic block: too many length or distance codes";
    case -4: return "dynamic block: code lengths set incomplete";
    case -5: return "dynamic block: lengths over-subscribed or incomplete";
    case -6: return "dynamic block: no end-of-block code";
    case -7: return "dynamic block: over-subscribed or incomplete code set";
    case -8: return "dynamic block: over-subscribed or incomplete distance codes";
    case -9: return "invalid literal/length or distance code";
    case -10: return "distance too far back";
    case -11: return "invalid literal/length code";
    default: return "unknown puff error";
    }
}

// zlib stream (RFC 1950): checks CMF/FLG (CM 8, (CMF*256+FLG) % 31 == 0, no FDICT), inflates the deflate body with puff
// into `out` (resized to `want`), checks Adler-32 when the whole stream was inflated. `want` may be LESS than the stream's
// full size (the rpack's 72 MB section, of which we need 8.66 MB): puff's 1 (output exhausted) is then success, the
// Adler-32 is not checkable and `*adlerChecked = false`. Returns false + `why` on anything else.
inline bool InflateZlib(const uint8_t* src, size_t n, size_t want, std::vector<uint8_t>& out, bool* adlerChecked, char* why, size_t whyN)
{
    if (adlerChecked) *adlerChecked = false;
    out.clear();
    if (!src || n < 6) return Fail(why, whyN, "zlib header: stream is only %zu bytes", n);
    const unsigned cmf = src[0], flg = src[1];
    if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return Fail(why, whyN, "zlib header bad (CMF %02X FLG %02X: need CM 8, FCHECK ok, no FDICT)", cmf, flg);
    if (want > kPuffMax) return Fail(why, whyN, "zlib: want %zu bytes is past puff's limit", want);

    out.assign(want + kPuffSlack, 0);
    unsigned long dlen = static_cast<unsigned long>(out.size());
    unsigned long slen = static_cast<unsigned long>(n - 2 - 4); // the body, without the 4-byte Adler-32 trailer
    const int r = puff(out.data(), &dlen, src + 2, &slen);
    if (r == 0) {
        if (dlen < want) { out.clear(); return Fail(why, whyN, "zlib: the stream holds %lu bytes, %zu wanted", dlen, want); }
        out.resize(want);
        if (dlen == want) { // the WHOLE stream: the trailer can be checked
            const uint32_t have = Adler32(out.data(), out.size());
            const uint32_t sent = (uint32_t(src[n - 4]) << 24) | (uint32_t(src[n - 3]) << 16) | (uint32_t(src[n - 2]) << 8) | src[n - 1];
            if (have != sent) { out.clear(); return Fail(why, whyN, "zlib adler-32 mismatch (computed %08X, stream says %08X)", have, sent); }
            if (adlerChecked) *adlerChecked = true;
        }
        return true;
    }
    if (r == 1) { out.resize(want); return true; } // output exhausted: the prefix is complete, the trailer is out of reach
    out.clear();
    return Fail(why, whyN, "zlib: deflate body refused: puff %d (%s)", r, PuffName(r));
}

// raw deflate (a ZIP method-8 entry): inflate exactly `uncompressed` bytes, then CRC-32 == `crc`.
inline bool InflateRaw(const uint8_t* src, size_t n, size_t uncompressed, uint32_t crc, std::vector<uint8_t>& out, char* why, size_t whyN)
{
    out.clear();
    if (!src && n) return Fail(why, whyN, "raw deflate: null source");
    if (uncompressed > kPuffMax || n > kPuffMax) return Fail(why, whyN, "raw deflate: size %zu / %zu is past puff's limit", uncompressed, n);
    out.assign(uncompressed, 0);
    unsigned long dlen = static_cast<unsigned long>(uncompressed);
    unsigned long slen = static_cast<unsigned long>(n);
    const int r = puff(out.empty() ? nullptr : out.data(), &dlen, src, &slen);
    if (r != 0) {
        out.clear();
        return Fail(why, whyN, "raw deflate refused: puff %d (%s)", r, r == 1 ? "output larger than the entry's size" : PuffName(r));
    }
    if (dlen != uncompressed) { out.clear(); return Fail(why, whyN, "raw deflate: produced %lu bytes, entry says %zu", dlen, uncompressed); }
    const uint32_t have = Crc32(out.data(), out.size());
    if (have != crc) { out.clear(); return Fail(why, whyN, "crc mismatch (computed %08X, entry says %08X)", have, crc); }
    return true;
}

// ---------------------------------------------------------------- ZIP
// Given the archive's LAST <= 65,557 bytes (`tail`, which starts at file offset `tailOff`), find the EOCD record and
// return the central directory's offset / size. Then, over the central directory bytes, find `name` -> its method,
// sizes, CRC and the local header offset. Then, over the 30 + n + m local header bytes, the data offset.
struct ZipDir { uint64_t cdOff = 0, cdSize = 0; };
struct ZipEntry { uint16_t method = 0; uint32_t crc = 0; uint64_t csize = 0, usize = 0, localOff = 0; };

inline bool ZipFindDir(const uint8_t* tail, size_t n, uint64_t tailOff, ZipDir* out, char* why, size_t whyN)
{
    if (!tail || !out || n < 22) return Fail(why, whyN, "zip: tail is %zu bytes, an end-of-central-directory record needs 22", n);
    const size_t lowest = (n - 22 > 65535) ? (n - 22 - 65535) : 0; // the comment is at most 65535 bytes
    for (size_t i = n - 22 + 1; i-- > lowest;) {
        if (Rd32(tail + i) != 0x06054b50u) continue;
        const size_t clen = Rd16(tail + i + 20);
        if (i + 22 + clen > n) continue; // a signature inside someone's data, not a record
        const uint32_t cdSize = Rd32(tail + i + 12), cdOff = Rd32(tail + i + 16);
        if (cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu || Rd16(tail + i + 10) == 0xFFFFu)
            return Fail(why, whyN, "zip: ZIP64 archive (EOCD fields 0xFFFFFFFF), not supported");
        if (uint64_t(cdOff) + cdSize > tailOff + i) return Fail(why, whyN, "zip: central directory %u + %u runs past the EOCD at %llu", cdOff, cdSize, (unsigned long long)(tailOff + i));
        out->cdOff = cdOff; out->cdSize = cdSize;
        return true;
    }
    return Fail(why, whyN, "zip: no end-of-central-directory record in the last %zu bytes", n);
}

inline bool ZipFindEntry(const uint8_t* cd, size_t n, const char* name, ZipEntry* out, char* why, size_t whyN)
{
    if (!cd || !name || !out) return Fail(why, whyN, "zip: null argument");
    const size_t want = std::strlen(name);
    size_t p = 0;
    while (p < n) {
        if (n - p < 46 || Rd32(cd + p) != 0x02014b50u)
            return Fail(why, whyN, "zip: central directory corrupt at offset %zu of %zu (looking for '%s')", p, n, name);
        const size_t nl = Rd16(cd + p + 28), el = Rd16(cd + p + 30), cl = Rd16(cd + p + 32);
        if (n - p < 46 + nl + el + cl) return Fail(why, whyN, "zip: central directory entry at %zu runs past the end", p);
        if (nl == want && std::memcmp(cd + p + 46, name, want) == 0) {
            const uint32_t cs = Rd32(cd + p + 20), us = Rd32(cd + p + 24), lo = Rd32(cd + p + 42);
            if (cs == 0xFFFFFFFFu || us == 0xFFFFFFFFu || lo == 0xFFFFFFFFu)
                return Fail(why, whyN, "zip: entry '%s' uses ZIP64 fields, not supported", name);
            out->method = Rd16(cd + p + 10); out->crc = Rd32(cd + p + 16);
            out->csize = cs; out->usize = us; out->localOff = lo;
            return true;
        }
        p += 46 + nl + el + cl;
    }
    return Fail(why, whyN, "zip: entry '%s' not found in the central directory (%zu bytes)", name, n);
}

inline bool ZipDataOffset(const uint8_t* local, size_t n, uint64_t localOff, uint64_t* dataOff, char* why, size_t whyN)
{
    if (!local || !dataOff || n < 30) return Fail(why, whyN, "zip: local header needs 30 bytes, got %zu", n);
    if (Rd32(local) != 0x04034b50u) return Fail(why, whyN, "zip: no local header signature at offset %llu", (unsigned long long)localOff);
    const size_t nl = Rd16(local + 26), el = Rd16(local + 28);
    if (n < 30 + nl + el) return Fail(why, whyN, "zip: short local header: need %zu bytes, got %zu", 30 + nl + el, n);
    *dataOff = localOff + 30 + nl + el;
    return true;
}

// ---------------------------------------------------------------- RP6L
// (tools/rp6l_names.py + tools/calib_native_font.py, PROVEN on menu_common_PC.rpack): header 9 x u32 (magic 0x4C365052
// 'RP6L', ver, flags, nParts, nSect, nFiles, namesSize, nNames, block); then nSect x 20 B (u32 type, offset, unpacked,
// packed, ?), nParts x 16 B (u8 section, u8, u16, u32 offset, u32 size, u32), nFiles x 12 B (u8 nParts, u8, u8 type, u8,
// u32, u32 firstPart), nNames x u32 name offsets, then the name blob. File i is named by name i.
struct RpSection { uint32_t type, offset, unpacked, packed; };
struct RpPart { uint8_t section; uint32_t offset, size; };
struct RpFile { std::string name; uint8_t type; std::vector<RpPart> parts; };

// `need` = bytes the head must hold: 36 when called with n < 36, else the full directory size. Returns false + why
// ("not RP6L", "short head: need N", "name table out of range", "part index out of range").
inline bool RpParse(const uint8_t* head, size_t n, std::vector<RpSection>& secs, std::vector<RpFile>& files, size_t* need,
                    char* why, size_t whyN)
{
    secs.clear(); files.clear();
    if (need) *need = 36;
    if (!head || n < 36) return Fail(why, whyN, "short head: need 36");
    uint32_t h[9]; std::memcpy(h, head, 36);
    if (h[0] != 0x4C365052u) return Fail(why, whyN, "not RP6L (magic %08X)", h[0]);
    const uint64_t nParts = h[3], nSect = h[4], nFiles = h[5], namesSize = h[6], nNames = h[7];
    const uint64_t total = 36 + 20 * nSect + 16 * nParts + 12 * nFiles + 4 * nNames + namesSize;
    if (total > (64u << 20)) return Fail(why, whyN, "directory implausibly large: %llu bytes", (unsigned long long)total);
    if (need) *need = static_cast<size_t>(total);
    if (n < total) return Fail(why, whyN, "short head: need %llu", (unsigned long long)total);

    const uint8_t* sp = head + 36;
    const uint8_t* pp = sp + 20 * nSect;
    const uint8_t* fp = pp + 16 * nParts;
    const uint8_t* np = fp + 12 * nFiles;
    const uint8_t* blob = np + 4 * nNames;

    secs.resize(static_cast<size_t>(nSect));
    for (size_t i = 0; i < secs.size(); ++i) {
        const uint8_t* s = sp + 20 * i;
        secs[i].type = Rd32(s); secs[i].offset = Rd32(s + 4); secs[i].unpacked = Rd32(s + 8); secs[i].packed = Rd32(s + 12);
    }
    if (nFiles > nNames) { secs.clear(); return Fail(why, whyN, "name table out of range: %llu files but %llu names", (unsigned long long)nFiles, (unsigned long long)nNames); }
    files.resize(static_cast<size_t>(nFiles));
    for (size_t i = 0; i < files.size(); ++i) {
        const uint8_t* f = fp + 12 * i;
        const uint32_t off = Rd32(np + 4 * i);
        const unsigned cnt = f[0]; const uint32_t first = Rd32(f + 8);
        // the name: it must start inside the blob and be NUL-terminated inside it
        size_t e = off;
        while (e < namesSize && blob[e] != 0) ++e;
        if (off >= namesSize || e >= namesSize) { secs.clear(); files.clear(); return Fail(why, whyN, "name table out of range (name %zu at offset %u of %llu)", i, off, (unsigned long long)namesSize); }
        files[i].name.assign(reinterpret_cast<const char*>(blob) + off, e - off);
        files[i].type = f[2];
        if (uint64_t(first) + cnt > nParts) { secs.clear(); files.clear(); return Fail(why, whyN, "part index out of range (file %zu '%s': parts %u..%llu of %llu)", i, files[i].name.c_str(), first, (unsigned long long)(uint64_t(first) + cnt), (unsigned long long)nParts); }
        for (unsigned k = 0; k < cnt; ++k) {
            const uint8_t* q = pp + 16 * (first + k);
            RpPart part; part.section = q[0]; part.offset = Rd32(q + 4); part.size = Rd32(q + 8);
            if (part.section >= nSect) { secs.clear(); files.clear(); return Fail(why, whyN, "part index out of range (file %zu '%s': part %u names section %u of %llu)", i, files[i].name.c_str(), first + k, unsigned(part.section), (unsigned long long)nSect); }
            files[i].parts.push_back(part);
        }
    }
    return true;
}

// ---------------------------------------------------------------- BC3
// BC3 (DXT5): the ALPHA channel of a w x h image (w, h multiples of 4), 16 bytes per block, alpha in the first 8.
// Palette: a0 > a1 -> 2..7 = ((7-j)*a0 + j*a1) / 7, j 1..6; else 2..5 = ((5-j)*a0 + j*a1) / 5, then 0, 255.
inline void Bc3Alpha(const uint8_t* px, int w, int h, uint8_t* out)
{
    const int bw = w / 4;
    for (int by = 0; by < h / 4; ++by) for (int bx = 0; bx < bw; ++bx) {
        const uint8_t* b = px + (static_cast<size_t>(by) * bw + bx) * 16;
        const int a0 = b[0], a1 = b[1]; int pal[8] = { a0, a1 };
        if (a0 > a1) { for (int j = 1; j <= 6; ++j) pal[1 + j] = ((7 - j) * a0 + j * a1) / 7; }
        else { for (int j = 1; j <= 4; ++j) pal[1 + j] = ((5 - j) * a0 + j * a1) / 5; pal[6] = 0; pal[7] = 255; }
        uint64_t bits = 0; for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
        for (int t = 0; t < 16; ++t)
            out[static_cast<size_t>(by * 4 + t / 4) * w + bx * 4 + t % 4] = static_cast<uint8_t>(pal[(bits >> (3 * t)) & 7]);
    }
}

} // namespace calibnative
} // namespace dlvr
