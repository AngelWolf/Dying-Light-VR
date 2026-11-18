// lidhand_logic_test.cpp - unit tests for lidhand_logic.h (plan docs/superpowers/plans/2026-11-18-hand-open-lids-phase2.md Task 2).
// Standalone: cl /nologo /EHsc /W4 /std:c++17 /I <tree>\src\ovr2xr lidhand_logic_test.cpp. Prints "lidhand_logic_test: N failure(s)"
// and exits N. Expected values from OUR data: tools/lid_hinge_table.py --dump-frame 49 (the chest's clip, frame 49).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lidhand_logic.h"

namespace lh = dlvr::lidhand;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}
static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// (a) LidLocal reproduces the game's own clip: the chest MESH REST rotated 58.400368 deg about its hinge == clip frame 49's local
//     (angle measured from the mesh rest - the clip's frame 0 sits 0.22 deg off it, so the first version of this test, at 58.180975 =
//     the angle from frame 0, failed by exactly that).
static void TestLidLocalMatchesClip(bool deliberateMiss)
{
    const float rest[12] = { 0.002451f, -0.000000f, -0.999997f, 0.379868f, 0.000000f, 1.000000f, -0.000000f, -0.000000f,
                             0.999997f, 0.000000f, 0.002451f, -0.255129f };
    const float axis[3] = { 0.0f, 1.0f, 0.0f };
    const float clipF49[12] = { 0.853012f, 0.000000f, -0.521891f, 0.379868f, -0.000000f, 1.000000f, -0.000000f, -0.000000f,
                                0.521891f, 0.000000f, 0.853012f, -0.255129f };
    float out[12];
    lh::LidLocal(rest, axis, deliberateMiss ? 50.0f : 58.400368f, out);
    double worst = 0.0;
    for (int i = 0; i < 12; ++i) { worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - clipF49[i])); }
    Check(deliberateMiss ? worst > 1e-3 : worst <= 1e-3, deliberateMiss ? "LidLocal deliberate miss must differ" : "LidLocal == clip f49");
    // and the angle back out of the local
    const float back = lh::LidAngleFromLocal(rest, out, axis);
    Check(deliberateMiss || Near(back, 58.400368, 1e-3), "LidAngleFromLocal round trip");
}

// (b) the hand's angle about the hinge, relative to where it grabbed: +30 one way, -30 the other.
static void TestHandAngle()
{
    const float pivot[3] = { 1.0f, 0.5f, 2.0f };
    const float axis[3] = { 0.0f, 1.0f, 0.0f };
    const float grab[3] = { 1.0f + 0.4f, 0.8f, 2.0f };       // 0.4 m out along +X, any height
    const double r = 30.0 * 3.14159265358979323846 / 180.0;
    // a +30 deg turn about +Y takes +X toward -Z
    const float handP[3] = { static_cast<float>(1.0 + 0.4 * std::cos(r)), 0.6f, static_cast<float>(2.0 - 0.4 * std::sin(r)) };
    const float handM[3] = { static_cast<float>(1.0 + 0.4 * std::cos(r)), 0.6f, static_cast<float>(2.0 + 0.4 * std::sin(r)) };
    Check(Near(lh::HandAngleDeg(pivot, axis, grab, handP), 30.0, 1e-3), "HandAngleDeg +30");
    Check(Near(lh::HandAngleDeg(pivot, axis, grab, handM), -30.0, 1e-3), "HandAngleDeg -30");
    const float onAxis[3] = { 1.0f, 3.0f, 2.0f };            // on the hinge line: no direction -> 0, never NaN
    Check(lh::HandAngleDeg(pivot, axis, grab, onAxis) == 0.0f, "HandAngleDeg on the axis = 0");
}

// (c) the open point fires once on the way up, never on the way back, and the clamp holds the lid in 0..open.
static void TestOpenPointAndClamp()
{
    int fires = 0;
    float prev = 0.0f;
    for (int i = 0; i <= 60; ++i) { const float now = static_cast<float>(i); if (lh::CrossesOpenPoint(prev, now, 58.181f, 0.5f)) { ++fires; } prev = now; }
    for (int i = 60; i >= 0; --i) { const float now = static_cast<float>(i); if (lh::CrossesOpenPoint(prev, now, 58.181f, 0.5f)) { ++fires; } prev = now; }
    Check(fires == 1, "CrossesOpenPoint fires exactly once over 0->60->0");
    Check(lh::ClampLid(-5.0f, 58.181f) == 0.0f && lh::ClampLid(70.0f, 58.181f) == 58.181f && lh::ClampLid(20.0f, 58.181f) == 20.0f,
          "ClampLid 0..open");
}

// (d) the nearest within reach.
static void TestNearest()
{
    const float d[3] = { 0.08f, 0.05f, 0.30f };
    Check(lh::NearestWithin(d, 3, 0.25f) == 1, "NearestWithin picks 0.05 over 0.08");
    const float far[1] = { 0.30f };
    Check(lh::NearestWithin(far, 1, 0.25f) == -1, "NearestWithin -1 beyond reach");
    const float none[2] = { -1.0f, -1.0f };
    Check(lh::NearestWithin(none, 2, 0.25f) == -1, "NearestWithin ignores no-reading (-1)");
}

// (e) releasing an item: stow only in the shoulder zone with the take action 1 (23 = inventory full).
static void TestItemRelease()
{
    Check(lh::OnItemRelease(true, 1) == lh::ItemRelease::Stow, "shoulder + action 1 = Stow");
    Check(lh::OnItemRelease(true, 23) == lh::ItemRelease::GiveBack, "shoulder + action 23 = GiveBack");
    Check(lh::OnItemRelease(false, 1) == lh::ItemRelease::GiveBack, "away from the shoulder = GiveBack");
}

// (f) trace 20261118_2010 - the lid's far edge (lid_hinge_table.inc, OUR mesh). In the lid bone's PARENT frame (parentW = identity):
//     at 0 deg the edge's middle sits R from the pivot (tool: basket 0.651, big chest 0.865, sedan_b 0.566), and a hand that moves
//     WITH the edge from closed to open turns HandAngleDeg by exactly +openDeg - the grab maths and the drawn edge agree in sign and size.
#include "lid_hinge_table.inc"
static void TestEdge(const char* object, double wantR, bool deliberateMiss)
{
    const LidHinge* h = nullptr;
    for (const LidHinge& x : kLidHinges) { if (std::strcmp(x.object, object) == 0) { h = &x; } }
    Check(h != nullptr, object);
    if (!h) { return; }
    const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float L0[12], Lo[12], a0[3], b0[3], ao[3], bo[3];
    lh::LidLocal(h->rest, h->axis, 0.0f, L0);
    lh::LidLocal(h->rest, h->axis, deliberateMiss ? -h->openDeg : h->openDeg, Lo);
    lh::EdgeWorld(I, L0, h->edge, a0, b0);
    lh::EdgeWorld(I, Lo, h->edge, ao, bo);
    const float m0[3] = { (a0[0] + b0[0]) / 2, (a0[1] + b0[1]) / 2, (a0[2] + b0[2]) / 2 };
    const float mo[3] = { (ao[0] + bo[0]) / 2, (ao[1] + bo[1]) / 2, (ao[2] + bo[2]) / 2 };
    char what[160];
    std::snprintf(what, sizeof(what), "%s edge R %.3f (want %.3f)", object, lh::LineDist(h->pivot, h->axis, m0), wantR);
    Check(Near(lh::LineDist(h->pivot, h->axis, m0), wantR, 0.002), what);
    const float turned = lh::HandAngleDeg(h->pivot, h->axis, m0, mo);
    std::snprintf(what, sizeof(what), "%s hand-with-edge turns %.3f (want %s%.3f)", object, turned, deliberateMiss ? "NOT " : "",
                  h->openDeg);
    Check(deliberateMiss ? !Near(turned, h->openDeg, 0.01) : Near(turned, h->openDeg, 0.01), what);
}

// (g) segment / line distances.
static void TestDistances()
{
    const float a[3] = { 0, 0, 0 }, b[3] = { 1, 0, 0 };
    const float mid[3] = { 0.5f, 0.2f, 0 }, past[3] = { 1.3f, 0.4f, 0 }, before[3] = { -0.3f, 0, 0 };
    Check(Near(lh::SegDist(a, b, mid), 0.2, 1e-6), "SegDist perpendicular 0.2");
    Check(Near(lh::SegDist(a, b, past), 0.5, 1e-6), "SegDist past the end 0.5");
    Check(Near(lh::SegDist(a, b, before), 0.3, 1e-6), "SegDist before the start 0.3");
    Check(Near(lh::SegDist(a, a, mid), std::sqrt(0.29), 1e-6), "SegDist zero-length = point distance");
    const float ax[3] = { 2, 0, 0 };
    Check(Near(lh::LineDist(a, ax, past), 0.4, 1e-6), "LineDist ignores the ends");
}

int main()
{
    TestEdge("basket_opening", 0.651, false);
    TestEdge("chest_loot_big", 0.865, false);
    TestEdge("sedan_b_trunk_opening", 0.566, false);
    TestEdge("basket_opening", 0.651, true);
    TestDistances();
    TestLidLocalMatchesClip(false);
    TestLidLocalMatchesClip(true);
    TestHandAngle();
    TestOpenPointAndClamp();
    TestNearest();
    TestItemRelease();
    std::printf("lidhand_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
