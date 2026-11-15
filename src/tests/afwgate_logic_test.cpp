// Standalone unit test for src/src/ovr2xr/afwgate_logic.h - NOT part of the DLL build.
// The rebuild gate's stale-camera and eye-position checks (docs/BETA-FINDINGS-2026-10-18.md §3, candidate A; INV-271, INV-384).
// Exit code = number of failures.
#include "afwgate_logic.h"

#include <cstdio>

using namespace dlvr::afwgate;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// INV-271: a stale snapshot hands this image the PREVIOUS image's matrix bit for bit (eye1==eye0).
static void identical_matrix_is_stale()
{
    float a[16], b[16];
    for (int i = 0; i < 16; ++i) { a[i] = b[i] = 0.25f * static_cast<float>(i) - 1.0f; }
    CHECK(SameMatrix(a, b));
    b[3] += 1e-6f;   // any real re-map differs somewhere (the eye offset, the jitter)
    CHECK(!SameMatrix(a, b));
}

// INV-384 (Jay's headset, 23,936 images): the GPU eye camera sits 0.0318 m from the captured camera = half his 63.6 mm IPD.
static void half_ipd_from_the_captured_camera_passes()
{
    const float cap[3] = { 10.0f, 2.0f, -5.0f };
    const float gpu[3] = { 10.0318f, 2.0f, -5.0f };
    CHECK(EyeOffsetOk(gpu, cap, 0.0636f));
    const float gpu2[3] = { 10.0f, 2.0f, -5.0318f };   // the direction does not matter, the distance does
    CHECK(EyeOffsetOk(gpu2, cap, 0.0636f));
}

// A stale matrix while walking: the GPU camera is where the eye WAS (~4 cm per frame at 3 m/s and 72 fps).
static void stale_position_while_walking_is_refused()
{
    const float cap[3] = { 0.0f, 0.0f, 0.0f };
    const float gpu[3] = { 0.0318f, 0.0f, 0.042f };
    CHECK(!EyeOffsetOk(gpu, cap, 0.0636f));
}

// The tolerance: 10 mm either side of half the IPD.
static void tolerance_is_ten_millimetres()
{
    const float cap[3] = { 0.0f, 0.0f, 0.0f };
    const float in[3]  = { 0.0318f + 0.0095f, 0.0f, 0.0f };
    const float out[3] = { 0.0318f + 0.0105f, 0.0f, 0.0f };
    const float low[3] = { 0.0318f - 0.0105f, 0.0f, 0.0f };
    CHECK(EyeOffsetOk(in, cap, 0.0636f));
    CHECK(!EyeOffsetOk(out, cap, 0.0636f));
    CHECK(!EyeOffsetOk(low, cap, 0.0636f));
}

// No usable eye separation = no verdict = refuse (INV-6: a degraded test fails closed).
static void unusable_separation_fails_closed()
{
    const float cap[3] = { 0.0f, 0.0f, 0.0f };
    const float gpu[3] = { 0.0318f, 0.0f, 0.0f };
    CHECK(!EyeOffsetOk(gpu, cap, 0.0f));
    CHECK(!EyeOffsetOk(gpu, cap, 0.2f));
    CHECK(SepUsable(0.0579f));   // a tester's Quest 3S
    CHECK(!SepUsable(0.0f));
}

int main()
{
    identical_matrix_is_stale();
    half_ipd_from_the_captured_camera_passes();
    stale_position_while_walking_is_refused();
    tolerance_is_ten_millimetres();
    unusable_separation_fails_closed();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
