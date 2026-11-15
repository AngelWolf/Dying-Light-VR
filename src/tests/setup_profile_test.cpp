// Standalone unit test for src/src/setup/setup_profile.h - NOT part of any DLL build. Links third_party/puff/puff.c.
// docs/superpowers/specs/2026-10-17-installer-design.md 5.6 (controller layout: read & warn). Exit code = failures.
// Fixture: tests/data/default_pro_sample.bin = a real default.pro with every value blanked except the I_LayoutPreset keys
// (all 0), by tools/scrub_profile_fixture.py - entry names, types and order are the game's own. md5 5d350ebc.
// Run from the tests folder (the fixture path is relative to it).
#include "setup_profile.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static std::vector<unsigned char> Fixture()
{
    std::ifstream f("data/default_pro_sample.bin", std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

// gzip of `raw` using stored (uncompressed) deflate blocks. The CRC field is written as 0 (ReadLayout does not check it).
static std::vector<unsigned char> GzipStored(const std::vector<unsigned char>& raw)
{
    std::vector<unsigned char> g = {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0x0B};
    size_t p = 0;
    do {
        const size_t n = (raw.size() - p > 65535) ? 65535 : raw.size() - p;
        const bool last = (p + n == raw.size());
        g.push_back(last ? 1 : 0);
        g.push_back(static_cast<unsigned char>(n & 0xFF));
        g.push_back(static_cast<unsigned char>(n >> 8));
        g.push_back(static_cast<unsigned char>(~n & 0xFF));
        g.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
        g.insert(g.end(), raw.begin() + p, raw.begin() + p + n);
        p += n;
    } while (p < raw.size());
    for (int i = 0; i < 4; ++i) { g.push_back(0); }   // CRC32 (unchecked)
    const unsigned sz = static_cast<unsigned>(raw.size());
    for (int i = 0; i < 4; ++i) { g.push_back(static_cast<unsigned char>(sz >> (8 * i))); }
    return g;
}

static void fixture_present()
{
    const auto f = Fixture();
    CHECK(f.size() == 3364);
    std::vector<unsigned char> raw;
    CHECK(GunzipBytes(f, &raw));
    CHECK(raw.size() == 17234);
}
static void profile_sample_all_default()
{
    const LayoutResult r = ReadLayout(Fixture());
    CHECK(r.verdict == LayoutVerdict::AllDefault);
    CHECK(r.nonDefaultKeys.empty());
    CHECK(r.layoutKeysFound >= 10);
}
static void profile_patched_nonzero_warns()
{
    std::vector<unsigned char> raw;
    CHECK(GunzipBytes(Fixture(), &raw));
    const char* key = "I_LayoutPresettriggersCtr1";
    const size_t klen = std::strlen(key) + 1;   // with its NUL
    bool patched = false;
    for (size_t i = 0; i + klen + 4 <= raw.size(); ++i) {
        if (std::memcmp(&raw[i], key, klen) == 0) {
            raw[i + klen] = 2;
            patched = true;
            break;
        }
    }
    CHECK(patched);
    const LayoutResult r = ReadLayout(GzipStored(raw));
    CHECK(r.verdict == LayoutVerdict::NonDefault);
    CHECK(r.nonDefaultKeys.size() == 1 && r.nonDefaultKeys[0] == key);
}
static void profile_keybinding_and_invert_ignored()
{
    std::vector<unsigned char> raw;
    CHECK(GunzipBytes(Fixture(), &raw));
    const char* keys[] = {"I_LayoutPresetKeybinding_JumpCtr2", "I_LayoutPresetinvert_yCtr1"};
    for (const char* key : keys) {
        const size_t klen = std::strlen(key) + 1;
        for (size_t i = 0; i + klen + 4 <= raw.size(); ++i) {
            if (std::memcmp(&raw[i], key, klen) == 0) {
                raw[i + klen] = 7;
                break;
            }
        }
    }
    CHECK(ReadLayout(GzipStored(raw)).verdict == LayoutVerdict::AllDefault);
}
static void profile_bad_gzip_is_unreadable()
{
    CHECK(ReadLayout({0x00, 0x01}).verdict == LayoutVerdict::Unreadable);
    CHECK(ReadLayout({}).verdict == LayoutVerdict::Unreadable);
    auto f = Fixture();
    f.resize(f.size() / 2);   // truncated
    CHECK(ReadLayout(f).verdict == LayoutVerdict::Unreadable);
}
static void profile_no_layout_keys_is_unreadable()
{
    const std::vector<unsigned char> hello = {'h', 'e', 'l', 'l', 'o'};
    CHECK(ReadLayout(GzipStored(hello)).verdict == LayoutVerdict::Unreadable);
}

int main()
{
    fixture_present();
    profile_sample_all_default();
    profile_patched_nonzero_warns();
    profile_keybinding_and_invert_ignored();
    profile_bad_gzip_is_unreadable();
    profile_no_layout_keys_is_unreadable();
    std::printf("setup_profile_test: %d failure(s)\n", g_fail);
    return g_fail;
}
