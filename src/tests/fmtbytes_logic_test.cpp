// Standalone unit test for src/src/bootstrap/fmtbytes_logic.h - NOT part of the DLL build.
// RIFT float (docs/superpowers/specs/2026-10-21-rift-float-design.md 4.4): the 4-byte panel capture must refuse an 8-byte layer.
// Exit code = failures.
#include "fmtbytes_logic.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using dlvr::fmtbytes::BytesPerPixel;

static void rgba8_family_is_4()
{
    CHECK(BytesPerPixel(DXGI_FORMAT_R8G8B8A8_TYPELESS) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_R8G8B8A8_UNORM) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_B8G8R8A8_TYPELESS) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_B8G8R8A8_UNORM) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_R10G10B10A2_UNORM) == 4);
    CHECK(BytesPerPixel(DXGI_FORMAT_R11G11B10_FLOAT) == 4);
}
static void rgba16f_is_8()
{
    CHECK(BytesPerPixel(DXGI_FORMAT_R16G16B16A16_FLOAT) == 8);
    CHECK(BytesPerPixel(DXGI_FORMAT_R16G16B16A16_TYPELESS) == 8);
    CHECK(BytesPerPixel(DXGI_FORMAT_R16G16B16A16_UNORM) == 8);
}
static void unknown_is_0()
{
    CHECK(BytesPerPixel(DXGI_FORMAT_UNKNOWN) == 0);
    CHECK(BytesPerPixel(DXGI_FORMAT_R32_FLOAT) == 0);
}

int main()
{
    rgba8_family_is_4();
    rgba16f_is_8();
    unknown_is_0();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
