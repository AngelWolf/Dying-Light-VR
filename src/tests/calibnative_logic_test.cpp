// Standalone unit test for src/src/ovr2xr/calibnative_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 2. Exit code = number of failures.
//   calibnative_logic_test.exe [<oracle dump dir> [<game dir>]]
// The dump dir is written by `py -3 -I tools/calib_native_font.py "<game>" --dump <dir>`. The integration checks are SKIPPED
// (named, counted, printed) when a folder is absent - a skip is never silent. All file I/O lives HERE; the header is pure.
#include "calibnative_logic.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dlvr::calibnative;
typedef std::vector<uint8_t> Bytes;

static int g_fail = 0;
static int g_skip = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void P16(Bytes& b, unsigned v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void P32(Bytes& b, uint32_t v) { P16(b, v & 0xFFFF); P16(b, v >> 16); }
static void PStr(Bytes& b, const char* s) { while (*s) b.push_back(uint8_t(*s++)); }
static void PBE32(Bytes& b, uint32_t v) { for (int k = 3; k >= 0; --k) b.push_back(uint8_t(v >> (8 * k))); }
static bool Has(const char* hay, const char* needle) { return std::strstr(hay, needle) != nullptr; }

static const char kHello[] = "hello hello hello hello"; // 23 bytes
static const size_t kHelloN = 23;

// deflate body: ONE stored block (BFINAL 1, BTYPE 00), LEN / NLEN, the bytes.
static Bytes StoredDeflate(const char* s, size_t n)
{
    Bytes b;
    b.push_back(0x01);
    P16(b, unsigned(n)); P16(b, unsigned(~n) & 0xFFFF);
    b.insert(b.end(), s, s + n);
    return b;
}
static Bytes StoredZlib(const char* s, size_t n)
{
    Bytes b; b.push_back(0x78); b.push_back(0x01);
    Bytes d = StoredDeflate(s, n); b.insert(b.end(), d.begin(), d.end());
    PBE32(b, Adler32(reinterpret_cast<const uint8_t*>(s), n));
    return b;
}

static void TestChecksums()
{
    CHECK(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
    CHECK(Adler32(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398u);
}

static void TestInflate()
{
    char why[160]; bool ac = true; Bytes out;
    Bytes z = StoredZlib(kHello, kHelloN);

    why[0] = 0;
    CHECK(InflateZlib(z.data(), z.size(), kHelloN, out, &ac, why, sizeof why));
    CHECK(out.size() == kHelloN && std::memcmp(out.data(), kHello, kHelloN) == 0);
    CHECK(ac);

    Bytes bad = z; bad[2 + 5 + 3] ^= 0x01; // one payload byte
    ac = true; why[0] = 0;
    CHECK(!InflateZlib(bad.data(), bad.size(), kHelloN, out, &ac, why, sizeof why));
    CHECK(Has(why, "adler"));

    bad = z; bad[1] = 0x00; // FCHECK wrong: 0x7800 % 31 != 0
    why[0] = 0;
    CHECK(!InflateZlib(bad.data(), bad.size(), kHelloN, out, &ac, why, sizeof why));
    CHECK(Has(why, "header"));

    ac = true; why[0] = 0;
    CHECK(InflateZlib(z.data(), z.size(), 5, out, &ac, why, sizeof why));
    CHECK(out.size() == 5 && std::memcmp(out.data(), kHello, 5) == 0);
    CHECK(!ac);

    // wanting MORE than the stream holds is a refusal, by name
    why[0] = 0;
    CHECK(!InflateZlib(z.data(), z.size(), 99, out, &ac, why, sizeof why));
    CHECK(why[0] != 0);

    // raw deflate (ZIP method 8) + CRC-32
    Bytes d = StoredDeflate(kHello, kHelloN);
    uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(kHello), kHelloN);
    why[0] = 0;
    CHECK(InflateRaw(d.data(), d.size(), kHelloN, crc, out, why, sizeof why));
    CHECK(out.size() == kHelloN && std::memcmp(out.data(), kHello, kHelloN) == 0);
    why[0] = 0;
    CHECK(!InflateRaw(d.data(), d.size(), kHelloN, crc + 1, out, why, sizeof why));
    CHECK(Has(why, "crc"));
}

// ---- ZIP built in the test: one STORED entry data/panel/x.fm = "abc" ----
static Bytes MakeZip(size_t commentLen)
{
    const char* name = "data/panel/x.fm"; const unsigned nl = unsigned(std::strlen(name));
    const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>("abc"), 3);
    Bytes z;
    P32(z, 0x04034b50); P16(z, 20); P16(z, 0); P16(z, 0); P16(z, 0); P16(z, 0);
    P32(z, crc); P32(z, 3); P32(z, 3); P16(z, nl); P16(z, 0); PStr(z, name); PStr(z, "abc");
    const uint32_t cdOff = uint32_t(z.size());
    P32(z, 0x02014b50); P16(z, 20); P16(z, 20); P16(z, 0); P16(z, 0); P16(z, 0); P16(z, 0);
    P32(z, crc); P32(z, 3); P32(z, 3); P16(z, nl); P16(z, 0); P16(z, 0); P16(z, 0); P16(z, 0);
    P32(z, 0); P32(z, 0); PStr(z, name);
    const uint32_t cdSize = uint32_t(z.size()) - cdOff;
    P32(z, 0x06054b50); P16(z, 0); P16(z, 0); P16(z, 1); P16(z, 1); P32(z, cdSize); P32(z, cdOff); P16(z, unsigned(commentLen));
    for (size_t k = 0; k < commentLen; ++k) z.push_back('c');
    return z;
}

static void TestZip(size_t commentLen)
{
    char why[160]; Bytes z = MakeZip(commentLen);
    // the "tail" is a suffix that does NOT start at 0, so tailOff is exercised
    const size_t k = 7; ZipDir dir; why[0] = 0;
    CHECK(ZipFindDir(z.data() + k, z.size() - k, k, &dir, why, sizeof why));
    CHECK(dir.cdSize == 46 + 15);
    CHECK(dir.cdOff == 30 + 15 + 3);

    ZipEntry e; why[0] = 0;
    CHECK(ZipFindEntry(z.data() + dir.cdOff, size_t(dir.cdSize), "data/panel/x.fm", &e, why, sizeof why));
    CHECK(e.method == 0 && e.csize == 3 && e.usize == 3 && e.localOff == 0);
    CHECK(e.crc == Crc32(reinterpret_cast<const uint8_t*>("abc"), 3));

    uint64_t dataOff = 0; why[0] = 0;
    CHECK(ZipDataOffset(z.data() + e.localOff, z.size() - size_t(e.localOff), e.localOff, &dataOff, why, sizeof why));
    CHECK(dataOff == 30 + 15);
    CHECK(std::memcmp(z.data() + dataOff, "abc", 3) == 0);

    why[0] = 0;
    CHECK(!ZipFindEntry(z.data() + dir.cdOff, size_t(dir.cdSize), "data/panel/nope.fm", &e, why, sizeof why));
    CHECK(Has(why, "data/panel/nope.fm"));

    Bytes junk(100, 0x55); why[0] = 0;
    CHECK(!ZipFindDir(junk.data(), junk.size(), 0, &dir, why, sizeof why));
    CHECK(why[0] != 0);
}

// ---- RP6L head built in the test: 2 sections, 3 parts, 2 files ("a" = 1 part, "boxed_semibold_30" = 2 parts) ----
static Bytes MakeRp(uint32_t magic, uint8_t badFirstPart)
{
    const char blob[] = "a\0boxed_semibold_30"; // + implicit final NUL
    const uint32_t blobN = uint32_t(sizeof blob);
    Bytes h;
    P32(h, magic); P32(h, 1); P32(h, 0); P32(h, 3); P32(h, 2); P32(h, 2); P32(h, blobN); P32(h, 2); P32(h, 0);
    // sections: type, offset, unpacked, packed, ?
    P32(h, 0x20202020); P32(h, 5165); P32(h, 11096); P32(h, 447); P32(h, 0);
    P32(h, 0x20202022); P32(h, 6570388); P32(h, 72365228); P32(h, 9719741); P32(h, 0);
    // parts: u8 section, u8, u16, u32 offset, u32 size, u32
    h.push_back(0); h.push_back(0); P16(h, 0); P32(h, 10); P32(h, 20); P32(h, 0);
    h.push_back(0); h.push_back(0); P16(h, 0); P32(h, 304); P32(h, 151); P32(h, 0);
    h.push_back(1); h.push_back(0); P16(h, 0); P32(h, 274624); P32(h, 8388608); P32(h, 0);
    // files: u8 nParts, u8, u8 type, u8, u32, u32 firstPart
    h.push_back(1); h.push_back(0); h.push_back(7); h.push_back(0); P32(h, 0); P32(h, 0);
    h.push_back(2); h.push_back(0); h.push_back(32); h.push_back(0); P32(h, 0); P32(h, badFirstPart);
    // names: offsets then blob
    P32(h, 0); P32(h, 2);
    h.insert(h.end(), blob, blob + blobN);
    return h;
}

static void TestRp()
{
    char why[160]; std::vector<RpSection> secs; std::vector<RpFile> files; size_t need = 0;
    Bytes h = MakeRp(0x4C365052u, 1);
    CHECK(h.size() == 36 + 40 + 48 + 24 + 8 + 20);

    why[0] = 0;
    CHECK(RpParse(h.data(), h.size(), secs, files, &need, why, sizeof why));
    if (why[0]) std::printf("  RpParse (synthetic head) why: %s\n", why);
    CHECK(need == h.size());
    CHECK(secs.size() == 2 && secs[1].type == 0x20202022u && secs[1].offset == 6570388 && secs[1].unpacked == 72365228 &&
          secs[1].packed == 9719741);
    CHECK(files.size() == 2);
    if (files.size() == 2) {
        CHECK(files[0].name == "a" && files[0].type == 7 && files[0].parts.size() == 1);
        CHECK(files[1].name == "boxed_semibold_30" && files[1].type == 32 && files[1].parts.size() == 2);
        if (files[1].parts.size() == 2) {
            CHECK(files[1].parts[0].section == 0 && files[1].parts[0].offset == 304 && files[1].parts[0].size == 151);
            CHECK(files[1].parts[1].section == 1 && files[1].parts[1].offset == 274624 && files[1].parts[1].size == 8388608);
        }
    }

    // too short to hold the 36-byte header, then too short to hold the directory: need says how much
    need = 0; why[0] = 0;
    CHECK(!RpParse(h.data(), 10, secs, files, &need, why, sizeof why));
    CHECK(need == 36 && Has(why, "short head: need 36"));
    need = 0; why[0] = 0;
    CHECK(!RpParse(h.data(), 40, secs, files, &need, why, sizeof why));
    CHECK(need == h.size() && Has(why, "short head: need"));

    Bytes bad = MakeRp(0x58365052u, 1); // 'RP6X'
    why[0] = 0;
    CHECK(!RpParse(bad.data(), bad.size(), secs, files, &need, why, sizeof why));
    CHECK(Has(why, "not RP6L"));

    bad = MakeRp(0x4C365052u, 2); // file 1: firstPart 2 + 2 parts > 3
    why[0] = 0;
    CHECK(!RpParse(bad.data(), bad.size(), secs, files, &need, why, sizeof why));
    CHECK(Has(why, "part index out of range"));
}

static void TestBc3()
{
    uint8_t blk[16]; uint8_t out[16];
    std::memset(blk, 0, sizeof blk); blk[0] = 255; blk[1] = 0;
    uint64_t bits = 0; // 3-bit index 2 in every one of the 16 slots (48 bits)
    for (int t = 0; t < 16; ++t) bits |= uint64_t(2) << (3 * t);
    for (int k = 0; k < 6; ++k) blk[2 + k] = uint8_t(bits >> (8 * k));
    Bc3Alpha(blk, 4, 4, out);
    for (int t = 0; t < 16; ++t) CHECK(out[t] == (6 * 255 + 0) / 7); // 218

    std::memset(blk, 0, sizeof blk); blk[0] = 0; blk[1] = 255; // a0 <= a1: 6-step palette + 0 + 255
    bits = 0;
    for (int t = 0; t < 16; ++t) bits |= uint64_t(t % 3 == 0 ? 6 : (t % 3 == 1 ? 7 : 2)) << (3 * t);
    for (int k = 0; k < 6; ++k) blk[2 + k] = uint8_t(bits >> (8 * k));
    Bc3Alpha(blk, 4, 4, out);
    for (int t = 0; t < 16; ++t) CHECK(out[t] == (t % 3 == 0 ? 0 : (t % 3 == 1 ? 255 : (4 * 0 + 255) / 5))); // 0 / 255 / 51

    // two blocks side by side in an 8 x 4 image land in the right columns
    uint8_t two[32]; uint8_t o8[32]; std::memset(two, 0, sizeof two);
    two[0] = 200; two[1] = 100; // block 0: all index 0 -> a0 = 200
    two[16] = 30; two[17] = 60; // block 1: a0 <= a1, all index 0 -> 30
    Bc3Alpha(two, 8, 4, o8);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) CHECK(o8[y * 8 + x] == (x < 4 ? 200 : 30));
}

// ---- integration (real files) ----
static bool ReadAt(const std::string& path, uint64_t off, size_t n, Bytes& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    bool ok = _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0;
    out.assign(n, 0);
    if (ok) { size_t got = std::fread(out.data(), 1, n, f); out.resize(got); ok = (got == n); }
    std::fclose(f);
    return ok;
}
static bool FileSize(const std::string& path, uint64_t* sz)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    bool ok = _fseeki64(f, 0, SEEK_END) == 0;
    if (ok) *sz = static_cast<uint64_t>(_ftelli64(f));
    std::fclose(f);
    return ok;
}
static bool Exists(const std::string& p) { uint64_t s; return FileSize(p, &s); }

static void TestOracleBc3(const std::string& dumpDir)
{
    Bytes bc3, alpha;
    uint64_t sa = 0, sb = 0;
    const std::string pb = dumpDir + "\\atlas_bc3.bin", pa = dumpDir + "\\atlas_alpha.bin";
    if (!FileSize(pb, &sb) || !FileSize(pa, &sa)) {
        std::printf("SKIPPED: %s or %s not found\n", pb.c_str(), pa.c_str()); ++g_skip; return;
    }
    CHECK(ReadAt(pb, 0, size_t(sb), bc3) && ReadAt(pa, 0, size_t(sa), alpha));
    CHECK(sb == 2048u * 4096u && sa == 2048u * 4096u);
    if (bc3.size() != 2048u * 4096u || alpha.size() != bc3.size()) return;
    Bytes mine(bc3.size());
    Bc3Alpha(bc3.data(), 2048, 4096, mine.data());
    size_t diff = 0; for (size_t i = 0; i < mine.size(); ++i) diff += (mine[i] != alpha[i]);
    if (diff) std::printf("oracle Bc3Alpha: %zu of %zu pixels differ from atlas_alpha.bin\n", diff, mine.size());
    else std::printf("oracle Bc3Alpha: 0 of %zu pixels differ from atlas_alpha.bin\n", mine.size());
    CHECK(diff == 0);
}

static void TestOracleChain(const std::string& dumpDir, const std::string& game)
{
    const std::string pak = game + "\\DW\\Data0.pak", rp = game + "\\DW\\Data\\menu_common_PC.rpack";
    uint64_t pakSz = 0;
    if (!FileSize(pak, &pakSz) || !Exists(rp) || !Exists(dumpDir + "\\boxed_semibold_30.fm") ||
        !Exists(dumpDir + "\\atlas_bc3.bin")) {
        std::printf("SKIPPED: %s or %s or the dump in %s not found\n", pak.c_str(), rp.c_str(), dumpDir.c_str()); ++g_skip; return;
    }
    char why[200]; why[0] = 0;

    // ---- the .fm from Data0.pak ----
    Bytes tail;
    const size_t tn = size_t(pakSz < 65557 ? pakSz : 65557);
    CHECK(ReadAt(pak, pakSz - tn, tn, tail));
    ZipDir dir;
    CHECK(ZipFindDir(tail.data(), tail.size(), pakSz - tn, &dir, why, sizeof why));
    if (why[0]) std::printf("  ZipFindDir why: %s\n", why);
    Bytes cd;
    CHECK(ReadAt(pak, dir.cdOff, size_t(dir.cdSize), cd));
    ZipEntry e; why[0] = 0;
    CHECK(ZipFindEntry(cd.data(), cd.size(), "data/panel/boxed_semibold_30.fm", &e, why, sizeof why));
    std::printf("fm entry: method %u, csize %llu, usize %llu, crc %08X, local %llu\n", unsigned(e.method),
                (unsigned long long)e.csize, (unsigned long long)e.usize, e.crc, (unsigned long long)e.localOff);
    CHECK(e.method == 8 && e.csize == 3662 && e.usize == 12179 && e.crc == 0x296B15EAu);
    Bytes loc;
    CHECK(ReadAt(pak, e.localOff, 30 + 2 * 65535, loc) || !loc.empty()); // short read at EOF is fine, the parser bounds-checks
    uint64_t dataOff = 0; why[0] = 0;
    CHECK(ZipDataOffset(loc.data(), loc.size(), e.localOff, &dataOff, why, sizeof why));
    Bytes packed, fm;
    CHECK(ReadAt(pak, dataOff, size_t(e.csize), packed));
    why[0] = 0;
    CHECK(InflateRaw(packed.data(), packed.size(), size_t(e.usize), e.crc, fm, why, sizeof why));
    if (why[0]) std::printf("  InflateRaw why: %s\n", why);
    Bytes fmRef; CHECK(ReadAt(dumpDir + "\\boxed_semibold_30.fm", 0, 12179, fmRef));
    CHECK(fm.size() == fmRef.size() && !fm.empty() && std::memcmp(fm.data(), fmRef.data(), fm.size()) == 0);

    // ---- the atlas from the rpack ----
    Bytes head; why[0] = 0;
    CHECK(ReadAt(rp, 0, 36, head));
    std::vector<RpSection> secs; std::vector<RpFile> files; size_t need = 0;
    CHECK(!RpParse(head.data(), head.size(), secs, files, &need, why, sizeof why) && need > 36);
    CHECK(ReadAt(rp, 0, need, head));
    why[0] = 0;
    CHECK(RpParse(head.data(), head.size(), secs, files, &need, why, sizeof why));
    if (why[0]) std::printf("  RpParse why: %s\n", why);
    const RpFile* ff = nullptr;
    for (const RpFile& f : files) if (f.name == "boxed_semibold_30") ff = &f;
    CHECK(ff != nullptr);
    if (!ff) return;
    CHECK(ff->parts.size() == 2);
    if (ff->parts.size() != 2) return;
    CHECK(ff->parts[0].section == 0 && ff->parts[0].size == 151);
    CHECK(ff->parts[1].section == 2 && ff->parts[1].offset == 274624 && ff->parts[1].size == 8388608);

    // section 0 whole: the Adler-32 must be checkable and pass
    {
        const RpSection& s0 = secs[ff->parts[0].section];
        Bytes raw, sec; bool ac = false; why[0] = 0;
        CHECK(ReadAt(rp, s0.offset, s0.packed, raw));
        CHECK(InflateZlib(raw.data(), raw.size(), s0.unpacked, sec, &ac, why, sizeof why));
        std::printf("section 0: want %u, adlerChecked %d %s\n", s0.unpacked, int(ac), why);
        CHECK(ac);
    }
    // section 2: only the prefix we need
    {
        const RpSection& s2 = secs[ff->parts[1].section];
        Bytes raw, sec, ref; bool ac = true; why[0] = 0;
        CHECK(ReadAt(rp, s2.offset, s2.packed, raw));
        const size_t want = size_t(ff->parts[1].offset) + ff->parts[1].size;
        CHECK(InflateZlib(raw.data(), raw.size(), want, sec, &ac, why, sizeof why));
        std::printf("section 2: want %zu of %u, adlerChecked %d %s\n", want, s2.unpacked, int(ac), why);
        CHECK(!ac);
        CHECK(sec.size() == want);
        CHECK(ReadAt(dumpDir + "\\atlas_bc3.bin", 0, ff->parts[1].size, ref));
        CHECK(sec.size() == want && ref.size() == ff->parts[1].size &&
              std::memcmp(sec.data() + ff->parts[1].offset, ref.data(), ref.size()) == 0);
    }
}

int main(int argc, char** argv)
{
    TestChecksums();
    TestInflate();
    TestZip(0);
    TestZip(10);
    TestRp();
    TestBc3();
    if (argc >= 2) {
        TestOracleBc3(argv[1]);
        if (argc >= 3) TestOracleChain(argv[1], argv[2]);
        else { std::printf("SKIPPED: no game dir argument (argv[2]), chain oracle not run\n"); ++g_skip; }
    } else {
        std::printf("SKIPPED: no oracle dump dir argument (argv[1]), both oracle checks not run\n"); g_skip += 2;
    }
    std::printf("%d failure(s), %d skipped\n", g_fail, g_skip);
    return g_fail;
}
