// Standalone unit test for src/src/ovr2xr/videoforce_logic.h - NOT part of the DLL build.
// VR video force (docs/superpowers/specs/2026-10-21-vr-video-force-design.md 3.1): the bring-up stub's bytes, its return,
// the 7-byte site jump, and the rel32 refusals. Exit code = failures. Pass "dump" to print the stub for the capstone check.
#include "videoforce_logic.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using dlvr::videoforce::BuildSiteJump;
using dlvr::videoforce::BuildStub;
using dlvr::videoforce::kStubMax;
using dlvr::videoforce::Record;

static const uintptr_t kCode = 0x180000000000ull;
static const uintptr_t kRec  = kCode + 0x100;
static const uintptr_t kRet  = kCode - 0x10000;

// The expected stub, written out from the spec's table: each rip-relative disp = target - (code + end of instruction).
struct Expect {
    std::vector<uint8_t> b;
    void raw(std::initializer_list<int> xs) { for (int x : xs) { b.push_back(static_cast<uint8_t>(x)); } }
    void d32(int64_t v) { const int32_t x = static_cast<int32_t>(v); for (int i = 0; i < 4; ++i) { b.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF)); } }
    // opcode bytes, then a disp32 to `target`, then `tail` more bytes counted before the disp's reference point
    void rip(std::initializer_list<int> op, uintptr_t target, std::initializer_list<int> tail = {})
    {
        raw(op);
        const int64_t end = static_cast<int64_t>(kCode + b.size() + 4 + tail.size());
        d32(static_cast<int64_t>(target) - end);
        raw(tail);
    }
};

static std::vector<uint8_t> Expected()
{
    Expect e;
    e.raw({0x8B, 0x47, 0x18}); e.rip({0x89, 0x05}, kRec + 4);    // VSync
    e.raw({0x8B, 0x47, 0x20}); e.rip({0x89, 0x05}, kRec + 8);    // MaxFPS
    e.raw({0x8A, 0x47, 0x60}); e.rip({0x88, 0x05}, kRec + 12);   // HBAO+
    e.raw({0x8A, 0x47, 0x61}); e.rip({0x88, 0x05}, kRec + 13);   // DOF
    e.raw({0x8A, 0x47, 0x62}); e.rip({0x88, 0x05}, kRec + 14);   // PCSS
    e.raw({0x8A, 0x47, 0x76}); e.rip({0x88, 0x05}, kRec + 15);   // MotionBlur
    e.rip({0xC6, 0x05}, kRec + 0, {0x01});                       // ran = 1
    e.raw({0xC7, 0x47, 0x18, 0, 0, 0, 0});
    e.raw({0xC7, 0x47, 0x20, 0, 0, 0, 0});
    e.raw({0xC6, 0x47, 0x60, 0});
    e.raw({0xC6, 0x47, 0x61, 0});
    e.raw({0xC6, 0x47, 0x62, 0});
    e.raw({0xC6, 0x47, 0x76, 0});
    e.rip({0xE9}, kRet);
    return e.b;
}

static void stub_bytes_exact()
{
    uint8_t out[kStubMax] = {};
    const size_t n = BuildStub(out, sizeof(out), kCode, kRec, kRet);
    const std::vector<uint8_t> want = Expected();
    CHECK(n == 96);
    CHECK(n == want.size());
    for (size_t i = 0; i < want.size() && i < n; ++i) {
        if (out[i] != want[i]) {
            std::printf("  byte %zu: got %02X want %02X\n", i, out[i], want[i]);
            CHECK(out[i] == want[i]);
            break;
        }
    }
}

static void stub_returns_to_ret()
{
    uint8_t out[kStubMax] = {};
    const size_t n = BuildStub(out, sizeof(out), kCode, kRec, kRet);
    CHECK(n >= 5);
    if (n < 5) { return; }
    CHECK(out[n - 5] == 0xE9);
    int32_t rel = 0;
    std::memcpy(&rel, out + n - 4, 4);
    CHECK(kCode + n + static_cast<int64_t>(rel) == kRet);
}

static void site_jump()
{
    const uintptr_t site = 0x1802E63F2ull, stub = 0x180300000ull;
    uint8_t out[7] = {};
    CHECK(BuildSiteJump(out, site, stub));
    CHECK(out[0] == 0xE9);
    int32_t rel = 0;
    std::memcpy(&rel, out + 1, 4);
    CHECK(static_cast<int64_t>(rel) == static_cast<int64_t>(stub) - static_cast<int64_t>(site + 5));
    CHECK(out[5] == 0x90 && out[6] == 0x90);
}

static void out_of_reach_refuses()
{
    uint8_t out[kStubMax] = {};
    CHECK(BuildStub(out, sizeof(out), kCode, kRec, kCode + 0x80000000ull) == 0);
    CHECK(BuildStub(out, sizeof(out), kCode, kCode + 0x90000000ull, kRet) == 0);
    CHECK(BuildStub(out, kStubMax - 1, kCode, kRec, kRet) == 0);
    uint8_t j[7] = {};
    CHECK(!BuildSiteJump(j, 0x180000000ull, 0x180000000ull + 0xC0000000ull));
    CHECK(!BuildSiteJump(j, 0x180000000ull + 0xC0000000ull, 0x180000000ull));
}

// record_layout: compile-time (a constant CHECK trips C4127 under /W4 /WX); a wrong layout fails the build of this test.
static_assert(sizeof(Record) == 16, "Record size");
static_assert(offsetof(Record, ran) == 0, "ran");
static_assert(offsetof(Record, vsync) == 4, "vsync");
static_assert(offsetof(Record, maxFps) == 8, "maxFps");
static_assert(offsetof(Record, hbao) == 12, "hbao");
static_assert(offsetof(Record, dof) == 13, "dof");
static_assert(offsetof(Record, pcss) == 14, "pcss");
static_assert(offsetof(Record, motionBlur) == 15, "motionBlur");

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "dump") == 0) {
        uint8_t out[kStubMax] = {};
        const size_t n = BuildStub(out, sizeof(out), kCode, kRec, kRet);
        std::printf("code=%llx rec=%llx ret=%llx n=%zu\n", static_cast<unsigned long long>(kCode),
                    static_cast<unsigned long long>(kRec), static_cast<unsigned long long>(kRet), n);
        for (size_t i = 0; i < n; ++i) { std::printf("%02x", out[i]); }
        std::printf("\n");
        return 0;
    }
    stub_bytes_exact();
    stub_returns_to_ret();
    site_jump();
    out_of_reach_refuses();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
