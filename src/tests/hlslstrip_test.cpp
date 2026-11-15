// Standalone unit test for src_beta/src/common/hlslstrip.h - NOT part of the DLL build. Exit code = failures.
// BETA leak hardening (Jay 2026-10-18: "Addresses + refs"): the runtime HLSL sources lose their comments at compile time.
#include "common/hlslstrip.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

constexpr char kSrc[] = "float4 a; // INV-465: the edge ring\n"
                        "/* D-192 ruled out */float b;\n"
                        "float c = 1.0 / 2.0;   // U-59\n"
                        "// whole-line comment\n"
                        "float d;";
constexpr auto kStrip = ::dlvr::StripHlslComments<::dlvr::HlslStrippedLen(kSrc) + 1>(kSrc);

constexpr char kNone[] = "float4 x : SV_Target;\n";
constexpr auto kNoneStrip = ::dlvr::StripHlslComments<::dlvr::HlslStrippedLen(kNone) + 1>(kNone);

int main()
{
    // comments gone, code and newlines kept (line numbers in compiler errors stay meaningful)
    CHECK(std::strcmp(kStrip.s, "float4 a; \nfloat b;\nfloat c = 1.0 / 2.0;   \n\nfloat d;") == 0);
    CHECK(std::strstr(kStrip.s, "INV-") == nullptr && std::strstr(kStrip.s, "D-192") == nullptr &&
          std::strstr(kStrip.s, "U-59") == nullptr);
    CHECK(sizeof(kStrip.s) == std::strlen(kStrip.s) + 1);            // sized exactly: no zero padding
    CHECK(std::strcmp(kNoneStrip.s, kNone) == 0);                     // nothing to strip = identical
    // a single '/' (division) is code, not a comment
    CHECK(std::strstr(kStrip.s, "1.0 / 2.0") != nullptr);
    std::printf("hlslstrip_test: %d failure(s)\n", g_fail);
    return g_fail;
}
