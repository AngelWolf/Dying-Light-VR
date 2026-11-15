// Standalone unit test for src/src/ovr2xr/bodytrack_logic.h - NOT part of the DLL build. Exit code = number of failures.
// Build (VS x64 tools): cl /nologo /EHsc /W4 /O2 /I ..\ovr2xr bodytrack_logic_test.cpp
#include "bodytrack_logic.h"
#include "elbowreal_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::bodytrack;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool NearV(const float a[3], float x, float y, float z, float eps = 1e-4f)
{
    return Near(a[0], x, eps) && Near(a[1], y, eps) && Near(a[2], z, eps);
}

static void TestElbowOffset()
{
    // arm hanging straight down, elbow 10 cm behind the line (+z back)
    const float S[3] = { 0.0f, 0.0f, 0.0f }, E[3] = { 0.0f, -0.25f, 0.10f }, W[3] = { 0.0f, -0.50f, 0.0f };
    float u[3] = { 9, 9, 9 }, perp = -1, up = -1, lo = -1;
    CHECK(ElbowOffset(S, E, W, u, &perp, &up, &lo));
    CHECK(NearV(u, 0.0f, 0.0f, 1.0f));
    CHECK(Near(perp, 0.10f));
    CHECK(Near(up, std::sqrt(0.0625f + 0.01f)));
    CHECK(Near(lo, std::sqrt(0.0625f + 0.01f)));
    // the offset is perpendicular to the shoulder -> wrist line, whatever the line's direction
    const float W2[3] = { 0.3f, -0.4f, 0.0f }, E2[3] = { 0.10f, -0.25f, 0.08f };
    CHECK(ElbowOffset(S, E2, W2, u, &perp, nullptr, nullptr));
    CHECK(Near(u[0]*0.6f + u[1]*-0.8f, 0.0f));
    CHECK(Near(Len3(u), 1.0f));
    // a straight arm has no direction
    const float E3[3] = { 0.0f, -0.25f, 0.0f };
    u[0] = 7.0f;
    CHECK(!ElbowOffset(S, E3, W, u, &perp, &up, &lo));
    CHECK(u[0] == 7.0f);
    CHECK(Near(up, 0.25f));
    // shoulder on the wrist: refused
    CHECK(!ElbowOffset(S, E, S, u, &perp, nullptr, nullptr));
    // not finite: refused
    const float bad[3] = { NAN, 0.0f, 0.0f };
    CHECK(!ElbowOffset(S, bad, W, u, &perp, nullptr, nullptr));
}

static void TestGlitch()
{
    const float a[3] = { 0.0f, 0.0f, 0.0f };
    const float near5[3] = { 0.05f, 0.0f, 0.0f }, far50[3] = { 0.5f, 0.0f, 0.0f };
    CHECK(!IsGlitch(a, near5, 0.011f));         // 5 cm in one 90 Hz frame: a fast real arm
    CHECK(IsGlitch(a, far50, 0.011f));          // 50 cm in one frame: not an arm
    CHECK(!IsGlitch(a, far50, 0.10f));          // 50 cm in 0.1 s = 5 m/s: possible
    CHECK(!IsGlitch(a, far50, 0.0f));           // no reference
    CHECK(!IsGlitch(a, far50, 0.5f));           // gap: no reference
    const float nan3[3] = { NAN, 0.0f, 0.0f };
    CHECK(IsGlitch(a, nan3, 0.011f));
    // the limit itself: speed * dt + slack
    const float edgeIn[3] = { kMaxElbowSpeedMps * 0.02f + kGlitchSlackM - 0.001f, 0.0f, 0.0f };
    const float edgeOut[3] = { kMaxElbowSpeedMps * 0.02f + kGlitchSlackM + 0.001f, 0.0f, 0.0f };
    CHECK(!IsGlitch(a, edgeIn, 0.02f));
    CHECK(IsGlitch(a, edgeOut, 0.02f));
}

static void TestPerpWeight()
{
    CHECK(PerpWeight(0.0f) == 0.0f);
    CHECK(PerpWeight(kPerpLoM) == 0.0f);
    CHECK(PerpWeight(kPerpHiM) == 1.0f);
    CHECK(PerpWeight(0.5f) == 1.0f);
    CHECK(Near(PerpWeight(0.5f * (kPerpLoM + kPerpHiM)), 0.5f));
    CHECK(PerpWeight(NAN) == 0.0f);
}

static void TestAppToWorld()
{
    // boneblend.cpp's ry: { cby, 0, sby,  0, 1, 0,  -sby, 0, cby } applied as ry . v
    const float yaw = 0.7f, c = std::cos(yaw), s = std::sin(yaw);
    const float ry[9] = { c, 0, s, 0, 1, 0, -s, 0, c };
    const float v[3] = { 0.3f, -0.2f, 0.9f };
    float o[3];
    AppToWorld(v, c, s, o);
    CHECK(NearV(o, ry[0]*v[0] + ry[1]*v[1] + ry[2]*v[2], ry[3]*v[0] + ry[4]*v[1] + ry[5]*v[2],
                ry[6]*v[0] + ry[7]*v[1] + ry[8]*v[2]));
    const float x[3] = { 1, 0, 0 };
    AppToWorld(x, 0.0f, 1.0f, o);   // yaw 90 deg
    CHECK(NearV(o, 0.0f, 0.0f, -1.0f));
}

static void TestPoleFromTracked()
{
    const float dir[3] = { 1, 0, 0 };
    const float w[3] = { 0.2f, 0.0f, -0.9f };
    float p[3];
    CHECK(PoleFromTracked(w, dir, p));
    CHECK(NearV(p, 0.0f, 0.0f, -1.0f));
    const float along[3] = { 0.995f, 0.0f, 0.0998f };   // 0.0998 off the arm: under kMinOffArm
    CHECK(!PoleFromTracked(along, dir, p));
}

static void TestElbowAtMatchesSolver()
{
    // ElbowAt must be elbowreal's own elbow (the face test measures the elbow the solve will draw)
    const float S[3] = { 0.1f, 1.4f, 0.0f }, T[3] = { 0.35f, 1.1f, -0.30f };
    float dir[3] = { T[0]-S[0], T[1]-S[1], T[2]-S[2] };
    const float dl = Len3(dir);
    dir[0] /= dl; dir[1] /= dl; dir[2] /= dl;
    float pole[3] = { 0.0f, -1.0f, 0.0f };
    const float k = Dot3(pole, dir);
    pole[0] -= dir[0]*k; pole[1] -= dir[1]*k; pole[2] -= dir[2]*k;
    const float pl = Len3(pole);
    pole[0] /= pl; pole[1] /= pl; pole[2] /= pl;
    const float A = 0.6f, L1 = 0.28f;
    float E1[3], E2[3], M[9];
    ElbowAt(S, dir, pole, std::cos(A), std::sin(A), L1, E1);
    dlvr::elbowreal::ForearmBasis(S, T, dir, pole, A, L1, E2, M);
    CHECK(NearV(E1, E2[0], E2[1], E2[2], 1e-5f));
}

static void TestFaceLimit()
{
    // arm forward along -z at the camera's height; pole UP puts the elbow up toward the camera
    const float S[3] = { 0.2f, 1.5f, 0.0f };
    const float dir[3] = { 0.0f, 0.0f, -1.0f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float A = 0.5f, L1 = 0.3f;
    const float cam[3] = { 0.2f, 1.5f + std::sin(A) * L1, -std::cos(A) * L1 };   // exactly at the "pole up" elbow
    // off: unchanged
    FaceResult r = FaceLimit(S, dir, up, A, L1, cam, true, 0.0f);
    CHECK(!r.moved && r.distM == -1.0f && NearV(r.pole, 0, 1, 0));
    r = FaceLimit(S, dir, up, A, L1, cam, false, 0.2f);
    CHECK(!r.moved && NearV(r.pole, 0, 1, 0));
    // far from the camera: unchanged, distance reported
    const float farCam[3] = { 5.0f, 5.0f, 5.0f };
    r = FaceLimit(S, dir, up, A, L1, farCam, true, 0.2f);
    CHECK(!r.moved && r.offsetDeg == 0.0f && r.distM > 0.2f && NearV(r.pole, 0, 1, 0));
    // at the camera: turned by the smallest step that reaches the limit, still perpendicular and unit
    r = FaceLimit(S, dir, up, A, L1, cam, true, 0.10f);
    CHECK(r.moved && !r.fallback);
    CHECK(r.distM >= 0.10f);
    CHECK(Near(Dot3(r.pole, dir), 0.0f) && Near(Len3(r.pole), 1.0f));
    {
        // no smaller step reaches it
        CHECK(std::fabs(r.offsetDeg) >= 5.0f);
        const float q[3] = { dir[1]*up[2] - dir[2]*up[1], dir[2]*up[0] - dir[0]*up[2], dir[0]*up[1] - dir[1]*up[0] };
        for (float a = 5.0f; a < std::fabs(r.offsetDeg) - 0.1f; a += 5.0f) {
            for (int sg = 1; sg >= -1; sg -= 2) {
                const float ar = sg * a * 0.01745329f;
                const float p[3] = { up[0]*std::cos(ar) + q[0]*std::sin(ar), up[1]*std::cos(ar) + q[1]*std::sin(ar),
                                     up[2]*std::cos(ar) + q[2]*std::sin(ar) };
                float E[3];
                ElbowAt(S, dir, p, std::cos(A), std::sin(A), L1, E);
                const float d[3] = { E[0]-cam[0], E[1]-cam[1], E[2]-cam[2] };
                CHECK(Len3(d) < 0.10f);
            }
        }
    }
    // unreachable limit: fallback to the farthest
    r = FaceLimit(S, dir, up, A, L1, cam, true, 5.0f);
    CHECK(r.moved && r.fallback && r.distM < 5.0f && r.distM > 0.0f);
}

int main()
{
    TestElbowOffset();
    TestGlitch();
    TestPerpWeight();
    TestAppToWorld();
    TestPoleFromTracked();
    TestElbowAtMatchesSolver();
    TestFaceLimit();
    if (g_fail == 0) { std::printf("bodytrack_logic_test PASSED\n"); }
    else { std::printf("bodytrack_logic_test %d FAILURE(S)\n", g_fail); }
    return g_fail;
}
