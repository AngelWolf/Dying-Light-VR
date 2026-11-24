// Standalone unit test for src/src/bootstrap/fixed_data3_md5.h - NOT part of the DLL build.
// Jay 2026-10-18: "Md5 + refuse AFW if wrong" (a tester's stock Data3, docs/BETA-FINDINGS-2026-10-18.md §2). Exit code = failures.
#include "fixed_data3_md5.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void match_is_ours()
{
    CHECK(Data3Verdict(true, kFixedData3Md5) == kData3Ours);
}
static void case_is_ignored()
{
    CHECK(Data3Verdict(true, "935318518B7A3D0624CA4FFB84DF18BE") == kData3Ours);
}
static void other_md5_is_not_ours()
{
    CHECK(Data3Verdict(true, "00000000000000000000000000000000") == kData3NotOurs);
    CHECK(Data3Verdict(true, "935318518b7a3d0624ca4ffb84df18b") == kData3NotOurs);   // one hex digit short
    CHECK(Data3Verdict(true, "60556476ae500fc3db9cd18a7f69b3b9") == kData3NotOurs);  // the previous pin (grip glyph, 11-22)
    CHECK(Data3Verdict(true, "394248e75c65b83ad2f26151a9b3566a") == kData3NotOurs);  // before that (Melee_VRShove, 10-31; 1.0 - 1.065)
    CHECK(Data3Verdict(true, "9b5a4c7fffa7f8352b93b1f0819a9cf4") == kData3NotOurs);  // the previous pin (Controls tab, 10-28)
    CHECK(Data3Verdict(true, "489618771b780df6c5bfe374d2455972") == kData3NotOurs);  // the previous pin (physical crouch, 10-27)
    CHECK(Data3Verdict(true, "f9ae3efc36888623a4371f60a3b1fb8f") == kData3NotOurs);  // the previous pin (Survivor Sense X hold 1.25 s, 10-25)
    CHECK(Data3Verdict(true, "1f7543a5b24ab0d5df27531e8fbf74be") == kData3NotOurs);  // before that (Survivor Sense X hold 2 s, 10-25)
    CHECK(Data3Verdict(true, "08073c05cb2c9db8dafa87849af452b6") == kData3NotOurs);  // before that (Survivor Sense X press, 10-25)
    CHECK(Data3Verdict(true, "301bff791ef0f9a738767287b414c576") == kData3NotOurs);  // before that (B1 option A, 10-25)
    CHECK(Data3Verdict(true, "782c81ac5fd994ae4fe8b3843c0cca85") == kData3NotOurs);  // before that (grapple skill unbound, 10-23)
    CHECK(Data3Verdict(true, "11d97f76edf4ca041a7c2ed155cdecb6") == kData3NotOurs);  // the one before (kick on Y, 10-23)
    CHECK(Data3Verdict(true, "5cd45bbd392e73a459969961911be74a") == kData3NotOurs);  // before that (cutscene bars, 10-22)
}
static void unreadable_is_its_own_state()
{
    CHECK(Data3Verdict(false, kFixedData3Md5) == kData3Unreadable);
    CHECK(Data3Verdict(true, nullptr) == kData3Unreadable);
}
static void pin_is_32_hex()
{
    int n = 0;
    for (const char* p = kFixedData3Md5; *p; ++p, ++n) {
        const char c = *p;
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    CHECK(n == 32);
}

int main()
{
    match_is_ours();
    case_is_ignored();
    other_md5_is_not_ours();
    unreadable_is_its_own_state();
    pin_is_32_hex();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
