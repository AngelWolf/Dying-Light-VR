// Standalone unit test for src/src/ovr2xr/vehlean_logic.h - NOT part of the DLL build.
// The buggy lean (2026-11-10, Jay: "Yes, build both"). Exit code = number of failures.
#include "vehlean_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::vehlean;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) <= 1e-6f; }

// The arm solve's own map of a tracking-space offset (ctl - head) into the world (boneblend.cpp SolveArmIk, T[]):
//   x' = x cby + z sby,  y' = y,  z' = -x sby + z cby
static void SolveMap(float cby, float sby, const float v[3], float o[3])
{
    o[0] = v[0] * cby + v[2] * sby;
    o[1] = v[1];
    o[2] = -v[0] * sby + v[2] * cby;
}

// The head offset goes into the world EXACTLY as the solve maps controller offsets, so anchor + off + map(ctl - head) == anchor +
// map(ctl): the hands no longer depend on the head in a vehicle.
static void SameMapAsTheSolve()
{
    const float yaws[] = { 0.0f, 0.7f, -2.1f, 3.14159265f };
    const float h[3] = { 0.12f, -0.05f, -0.21f };
    for (float y : yaws) {
        const float c = std::cos(y), s = std::sin(y);
        float a[3], b[3];
        HeadOffsetWorld(c, s, h, a);
        SolveMap(c, s, h, b);
        CHECK(Near(a[0], b[0]) && Near(a[1], b[1]) && Near(a[2], b[2]));
    }
}

static void HandsIndependentOfHead()
{
    const float c = std::cos(1.1f), s = std::sin(1.1f);
    const float ctl[3] = { 0.30f, -0.40f, -0.45f };
    const float heads[2][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -0.20f } };   // upright / leaning 20 cm forward (-Z)
    float t[2][3];
    for (int k = 0; k < 2; ++k) {
        float off[3], rel[3], m[3];
        HeadOffsetWorld(c, s, heads[k], off);
        for (int i = 0; i < 3; ++i) { rel[i] = ctl[i] - heads[k][i]; }
        SolveMap(c, s, rel, m);
        for (int i = 0; i < 3; ++i) { t[k][i] = off[i] + m[i]; }
    }
    CHECK(Near(t[0][0], t[1][0]) && Near(t[0][1], t[1][1]) && Near(t[0][2], t[1][2]));
}

static void ZeroHeadZeroOffset()
{
    const float z[3] = { 0.0f, 0.0f, 0.0f };
    float o[3];
    HeadOffsetWorld(0.3f, 0.95f, z, o);
    CHECK(o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f);
}

int main()
{
    SameMapAsTheSolve();
    HandsIndependentOfHead();
    ZeroHeadZeroOffset();
    std::printf("vehlean_logic: %d failure(s)\n", g_fail);
    return g_fail;
}
