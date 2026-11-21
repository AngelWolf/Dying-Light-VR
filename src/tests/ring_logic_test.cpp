// Standalone unit test for src/src/ovr2xr/ring_logic.h - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-20-holster-pivot-posture-rings.md Task 5. Exit code = number of failures.
#include "ring_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::holster::ring;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

static void AlphaInsideIsFull()
{
    CHECK(Near(Alpha(-0.05f, true), 1.0f));
    CHECK(Near(Alpha(0.10f, true), 1.0f));   // inside by the exit radius (hysteresis) still counts as inside
}

static void AlphaFadesOverThirtyCm()
{
    CHECK(Near(Alpha(0.0f, false), 0.75f));
    CHECK(Near(Alpha(0.15f, false), 0.375f));
    CHECK(Near(Alpha(0.30f, false), 0.0f));
    CHECK(Near(Alpha(0.50f, false), 0.0f));
    CHECK(Near(Alpha(-0.02f, false), 0.75f));   // a negative distance outside the latch: clamped to the edge
}

static void PickNearestThree()
{
    const float d[5] = { 0.20f, 0.05f, 0.40f, 0.10f, 0.25f };
    const bool on[5] = { true, true, true, true, true };
    int idx[3] = { -1, -1, -1 };
    const int n = Pick(d, on, 5, idx, 3);
    CHECK(n == 3);
    CHECK(idx[0] == 1 && idx[1] == 3 && idx[2] == 0);   // 0.40 is beyond the fade; 0.25 is 4th
}

static void PickSkipsDisabled()
{
    const float d[3] = { 0.01f, 0.02f, 0.03f };
    const bool on[3] = { false, true, false };
    int idx[3] = { -1, -1, -1 };
    const int n = Pick(d, on, 3, idx, 3);
    CHECK(n == 1);
    CHECK(idx[0] == 1);
}

int main()
{
    AlphaInsideIsFull();
    AlphaFadesOverThirtyCm();
    PickNearestThree();
    PickSkipsDisabled();
    std::printf("ring_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
