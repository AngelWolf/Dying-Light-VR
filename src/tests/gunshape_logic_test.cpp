// Standalone unit test for src/src/ovr2xr/gunshape_logic.h - NOT part of the DLL build.
// Solid guns: docs/superpowers/plans/2026-11-02-solid-guns-reload-sound-haptics.md Task 1 (+ the Task 1 rulings in the ledger
// .superpowers/sdd/2026-11-02-solid-guns/progress.md). Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\gunshape_logic_test.cpp
#include "gunshape_logic.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace dlvr::gunshape;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

namespace {

const float kIdentity34[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
const float kDeg = 3.14159265358979f / 180.0f;

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Obb Box(float cx, float cy, float cz, float hx, float hy, float hz, bool wall = false)
{
    Obb b{};
    b.c[0] = cx; b.c[1] = cy; b.c[2] = cz;
    b.axis[0][0] = 1; b.axis[1][1] = 1; b.axis[2][2] = 1;
    b.half[0] = hx; b.half[1] = hy; b.half[2] = hz;
    b.wall = wall;
    return b;
}

// A unit box at the origin, no well.
const Obb kUnit[1] = { Box(0, 0, 0, 0.5f, 0.5f, 0.5f) };
GunShape UnitGun() { return GunShape{ kUnit, 1, Channel{}, false }; }

// A gun with a well: two side walls at x = +-0.05 (each 0.02 thick, from y -0.10 to 0), a receiver above (y 0 .. 0.05), the channel
// between the walls: open end at y = -0.10, axis +Y (into the gun), depth 0.10, half-width 0.03 along X, half-depth 0.02 along Z.
const Obb kWell[3] = {
    Box(-0.05f, -0.05f, 0, 0.01f, 0.05f, 0.04f, true),
    Box(+0.05f, -0.05f, 0, 0.01f, 0.05f, 0.04f, true),
    Box(0, 0.025f, 0, 0.06f, 0.025f, 0.04f),
};
GunShape WellGun()
{
    Channel ch{};
    ch.origin[1] = -0.10f;
    ch.axis[1] = 1.0f;
    ch.wAxis[0] = 1.0f;
    ch.halfW = 0.03f; ch.halfD = 0.02f; ch.depth = 0.10f;
    return GunShape{ kWell, 3, ch, true };
}

// The mag in its hand frame: insertion axis = axis[0]. Hand frame rotated so axis[0] points along world +Y, tilted by `tiltDeg`
// about Z; the mag's centre at hand-frame origin, half-length 0.04 along its insertion axis, 0.025 x 0.015 across.
Obb Mag()
{
    Obb m{};
    m.axis[0][0] = 1; m.axis[1][1] = 1; m.axis[2][2] = 1;
    m.half[0] = 0.04f; m.half[1] = 0.025f; m.half[2] = 0.015f;
    return m;
}
// hand34: maps hand-local X (the insertion axis) to world (sin(-t), cos t, 0)-ish: rotation about Z by (90 deg - tilt) so X -> +Y
// at tilt 0; translation (x, y, z).
void Hand(float tiltDeg, float x, float y, float z, float out[12])
{
    const float a = (90.0f - tiltDeg) * kDeg;
    const float c = std::cos(a), s = std::sin(a);
    const float m[12] = { c, -s, 0, x,  s, c, 0, y,  0, 0, 1, z };
    for (int i = 0; i < 12; ++i) { out[i] = m[i]; }
}

void PointOutsideHullUnmoved()
{
    const GunShape g = UnitGun();
    const float S[3] = { 2, 0, 0 }, tip[3] = { 1, 0, 0 };
    float out[3];
    CHECK(!StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(out[0] == 1.0f && out[1] == 0.0f && out[2] == 0.0f);
}

void PointIntoBoxStopsAtFace()
{
    const GunShape g = UnitGun();
    const float S[3] = { 2, 0, 0 }, tip[3] = { 0, 0, 0 };
    float out[3];
    CHECK(StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(Near(out[0], 0.501f, 1e-5f));   // the face at 0.5, 1 mm pull-back toward S (handcoll's rule)
    CHECK(Near(out[1], 0.0f, 1e-6f) && Near(out[2], 0.0f, 1e-6f));
}

void PointStopFollowsGunPose()
{
    // The gun moved +10 m along X and turned 90 deg about Y: the hull follows the gun's world matrix.
    const GunShape g = UnitGun();
    const float gun34[12] = { 0, 0, 1, 10,  0, 1, 0, 0,  -1, 0, 0, 0 };
    const float S[3] = { 12, 0, 0 }, tip[3] = { 10, 0, 0 };
    float out[3];
    CHECK(StopPointOnHull(g, gun34, S, tip, out));
    CHECK(Near(out[0], 10.501f, 1e-4f));
}

void PointIntoChannelPasses()
{
    const GunShape g = WellGun();
    const float S[3] = { 0, -0.30f, 0 }, tip[3] = { 0, -0.05f, 0 };   // up the well, short of the receiver
    float out[3];
    CHECK(!StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(Near(out[1], -0.05f, 1e-6f));
}

void GripSpotReachableThroughHull()
{
    // A grip spot ON the unit box's face (x = 0.5): the hand still gets there (1 mm short).
    const GunShape g = UnitGun();
    const float S[3] = { 2, 0.2f, 0 }, spot[3] = { 0.5f, 0.2f, 0 };
    float out[3];
    StopPointOnHull(g, kIdentity34, S, spot, out);
    const float d[3] = { out[0] - spot[0], out[1] - spot[1], out[2] - spot[2] };
    CHECK(std::sqrt(Dot(d, d)) <= 0.002f);
}

void ShoulderInsideBoxIgnored()
{
    // handcoll's rule: a box that already contains S never pulls the hand back to S.
    const GunShape g = UnitGun();
    const float S[3] = { 0.1f, 0, 0 }, tip[3] = { 3, 0, 0 };
    float out[3];
    CHECK(!StopPointOnHull(g, kIdentity34, S, tip, out));
}

void MagAlignedSlidesInChannel()
{
    // Mag aligned, inside the channel, pressed 1 cm into the right wall: pushed back across the channel only.
    const GunShape g = WellGun();
    float hand[12];
    Hand(0.0f, 0.03f, -0.07f, 0, hand);   // centre x 0.03: the mag's +Y side (half 0.025 across, along world X) reaches 0.055 > 0.04
    float shift[3];
    CHECK(StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    const float axis[3] = { 0, 1, 0 };
    CHECK(Near(Dot(shift, axis), 0.0f, 1e-6f));
    CHECK(shift[0] < -0.01f + 1e-4f && shift[0] > -0.02f);   // out of the wall at x 0.04: 0.055 - 0.04 = 0.015
}

void MagFreeInChannelUnmoved()
{
    const GunShape g = WellGun();
    float hand[12];
    Hand(0.0f, 0.0f, -0.07f, 0, hand);
    float shift[3];
    CHECK(!StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    CHECK(shift[0] == 0.0f && shift[1] == 0.0f && shift[2] == 0.0f);
}

void ChannelAdmitsWithinAlign()
{
    // 44 deg of tilt with alignDeg 45: in the channel, so the walls push across only (no axial push back out).
    const GunShape g = WellGun();
    float hand[12];
    Hand(44.0f, 0.0f, -0.07f, 0, hand);
    float shift[3];
    StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift);
    const float axis[3] = { 0, 1, 0 };
    CHECK(Near(Dot(shift, axis), 0.0f, 1e-6f));
}

void AdmittedMagAtLipGuidedAcross()
{
    // Aligned, 2.5 cm off centre at the opening, its top 2 mm over the right wall's lip and 1 cm into it sideways: the plain minimal
    // push would be 2 mm DOWN (refused at the lip); the channel rule guides it across instead, so it can still go up the well.
    const GunShape g = WellGun();
    float hand[12];
    Hand(0.0f, 0.025f, -0.138f, 0, hand);
    float shift[3];
    CHECK(StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    const float axis[3] = { 0, 1, 0 };
    CHECK(Near(Dot(shift, axis), 0.0f, 1e-6f));
    CHECK(shift[0] < -0.005f);
}

void AdmittedMagFarBelowNotPushed()
{
    // Review finding 1: a mag within alignDeg (30 deg) but 3 mm off centre and 0.3 m BELOW the well (in mid-air, e.g. at the pouch)
    // must not be pushed by walls it does not touch.
    const GunShape g = WellGun();
    float hand[12];
    Hand(30.0f, 0.003f, -0.40f, 0, hand);
    float shift[3];
    CHECK(!StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    CHECK(shift[0] == 0.0f && shift[1] == 0.0f && shift[2] == 0.0f);
}

void ChannelLipStopsOffAxis()
{
    // 46 deg: not admitted - the full minimal push, which here moves the mag back DOWN out of the well (axial component < 0).
    const GunShape g = WellGun();
    float hand[12];
    Hand(46.0f, 0.0f, -0.085f, 0, hand);
    float shift[3];
    CHECK(StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    const float axis[3] = { 0, 1, 0 };
    CHECK(Dot(shift, axis) < 0.0f);
}

void MagSidewaysOutBlocked()
{
    // Half way out of the well and moved sideways into the left wall: the push cancels the sideways move.
    const GunShape g = WellGun();
    float hand[12];
    Hand(0.0f, -0.035f, -0.10f, 0, hand);   // mag x side reaches -0.06 into the wall (inner face -0.04)
    float shift[3];
    CHECK(StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    CHECK(shift[0] > 0.0f);
}

void ReceiverStopsMagPastSeat()
{
    // Aligned, but pushed up into the receiver above the well: the receiver is not a wall, so it pushes back down.
    const GunShape g = WellGun();
    float hand[12];
    Hand(0.0f, 0.0f, -0.03f, 0, hand);   // mag top at +0.01, receiver bottom at 0
    float shift[3];
    CHECK(StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    CHECK(shift[1] < -0.005f);
}

void RestingContactPulsesOnce()
{
    dlvr::handcoll::ContactEdge e;
    int rises = 0;
    for (int i = 0; i < 300; ++i) { rises += e.Update(true, i * 0.011) ? 1 : 0; }   // ~3.3 s resting on the gun
    CHECK(rises == 1);
}

void NonFiniteInputRefuses()
{
    const GunShape g = UnitGun();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float S[3] = { nan, 0, 0 }, tip[3] = { 0, 0, 0 };
    float out[3];
    CHECK(!StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(out[0] == 0.0f);
    float hand[12];
    Hand(0.0f, 0.0f, -0.07f, 0, hand);
    hand[3] = nan;
    float shift[3];
    CHECK(!StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
    CHECK(shift[0] == 0.0f && shift[1] == 0.0f && shift[2] == 0.0f);
}

void HiddenPartIgnored()
{
    // Two parts: part 0 = the body (unit box at the origin), part 1 = a suppressor box in front of it (x 1.5 .. 2.5). With part 1
    // hidden (the game hides the element), a hand coming from x 4 passes the suppressor and stops on the body; shown, it stops on it.
    Obb b[2] = { Box(0, 0, 0, 0.5f, 0.5f, 0.5f), Box(2, 0, 0, 0.5f, 0.5f, 0.5f) };
    b[1].part = 1;
    GunShape g{ b, 2, Channel{}, false };
    const float S[3] = { 4, 0, 0 }, tip[3] = { 0, 0, 0 };
    float out[3];
    CHECK(StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(Near(out[0], 2.501f, 1e-5f));
    g.hiddenParts = 1u << 1;
    CHECK(StopPointOnHull(g, kIdentity34, S, tip, out));
    CHECK(Near(out[0], 0.501f, 1e-5f));
    // and the mag: a hidden part never pushes it
    float hand[12];
    Hand(0.0f, 2.0f, 0.0f, 0, hand);
    float shift[3];
    CHECK(!StopBoxOnHull(g, kIdentity34, Mag(), hand, 45.0f, shift));
}

void GripExemptsStop()
{
    // Task 4: only the LEFT (off) hand, never while it is in a grip on the gun (two-hand grip, a live reload step), never without a
    // shape row for the held gun.
    CHECK(ShouldStop(1, false, true));
    CHECK(!ShouldStop(0, false, true));    // the right hand holds the gun
    CHECK(!ShouldStop(1, true, true));     // gripping: the hand sits where the grip puts it
    CHECK(!ShouldStop(1, false, false));   // no row for this gun
    CHECK(!ShouldStop(2, false, true));
}

void RelFromLastRecoversOffset()
{
    // Review finding 3: the mag in the COMMANDED hand frame = inv(last commanded H) x the mag as last DRAWN. Built as H x X, it must
    // give back X.
    const float c = std::cos(0.7f), s = std::sin(0.7f);
    const float H[12] = { c, -s, 0, 0.3f,  s, c, 0, -1.2f,  0, 0, 1, 0.5f };
    const float X[12] = { 0, 0, 1, 0.02f,  0, 1, 0, -0.05f,  -1, 0, 0, 0.11f };
    float drawn[12];
    detail::Mul34(H, X, drawn);
    float rel[12];
    CHECK(RelFromLast(H, drawn, rel));
    for (int i = 0; i < 12; ++i) { CHECK(Near(rel[i], X[i], 1e-5f)); }
    const float bad[12] = { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
    CHECK(!RelFromLast(bad, drawn, rel));   // degenerate H refused
}

void EscapeDetectedOnlyInsideTheWell()
{
    // RUN SG2 (Jay: "mag can also come out the side of the magwell"): the probe's predicate. The mag's TOP is past the lip (inside the
    // well) and its CENTRE is outside the footprint = escaping sideways. Centred = no; below the well = no; past the side = yes.
    const GunShape g = WellGun();
    float hand[12], off = 0.0f;
    Hand(0.0f, 0.0f, -0.07f, 0, hand);
    CHECK(!MagEscapingWell(g, kIdentity34, Mag(), hand, &off));     // centred in the well
    Hand(0.0f, 0.045f, -0.08f, 0, hand);
    CHECK(MagEscapingWell(g, kIdentity34, Mag(), hand, &off));      // top inside, centre 4.5 cm across (halfW 3 cm)
    CHECK(Near(off, 0.045f, 1e-4f));
    Hand(0.0f, 0.045f, -0.30f, 0, hand);
    CHECK(!MagEscapingWell(g, kIdentity34, Mag(), hand, &off));     // well below the gun
    const GunShape noWell = UnitGun();
    CHECK(!MagEscapingWell(noWell, kIdentity34, Mag(), hand, &off));
}

void EmptyGunNeverStops()
{
    const GunShape g{ nullptr, 0, Channel{}, false };
    const float S[3] = { 2, 0, 0 }, tip[3] = { 0, 0, 0 };
    float out[3];
    CHECK(!StopPointOnHull(g, kIdentity34, S, tip, out));
}

// ---- THE HAND AS SPHERES (Jay 2026-11-02: "consider the players hand & fingers"; SG4 renders: the old single point let 554 of 2216
// hand vertices into the pistol) ----

// No sphere of the hand (wrist at w) touches any body.
bool HandClear(const HandBody* bodies, int nb, const float (*off)[3], const float* r, int n, const float w[3])
{
    for (int b = 0; b < nb; ++b) {
        float nr[9];
        detail::UnitRot(bodies[b].pose34, nr);
        for (int i = 0; i < n; ++i) {
            const float c[3] = { w[0] + off[i][0], w[1] + off[i][1], w[2] + off[i][2] };
            float lc[3];
            detail::ToLocalPt(nr, bodies[b].pose34, c, lc);
            for (int k = 0; k < bodies[b].g->nBoxes; ++k) {
                if (detail::Hidden(*bodies[b].g, bodies[b].g->boxes[k])) { continue; }
                if (SphereTouchesBox(bodies[b].g->boxes[k], lc, r[i])) { return false; }
            }
        }
    }
    return true;
}

void SphereTouchesBoxByDistance()
{
    const Obb b = Box(0, 0, 0, 0.5f, 0.5f, 0.5f);
    const float face[3] = { 0.59f, 0, 0 }, faceFar[3] = { 0.61f, 0, 0 };
    CHECK(SphereTouchesBox(b, face, 0.1f));
    CHECK(!SphereTouchesBox(b, faceFar, 0.1f));
    // the corner: (0.55, 0.55, 0.55) is sqrt(3) x 0.05 = 0.0866 from the corner
    const float corner[3] = { 0.55f, 0.55f, 0.55f };
    CHECK(SphereTouchesBox(b, corner, 0.09f));
    CHECK(!SphereTouchesBox(b, corner, 0.08f));   // the face test alone (0.05 < 0.08) would say touching - the corner does not
}

void SpherePushOutMinimal()
{
    const Obb b = Box(0, 0, 0, 0.5f, 0.5f, 0.5f);
    float out[3];
    const float near[3] = { 0.55f, 0.1f, 0 };   // 0.05 from the +X face, r 0.1 -> out by 0.05 along +X
    CHECK(SpherePushOut(b, near, 0.1f, out));
    CHECK(Near(out[0], 0.05f, 1e-5f) && Near(out[1], 0, 1e-6f) && Near(out[2], 0, 1e-6f));
    const float in[3] = { 0, 0.45f, 0 };   // centre inside, nearest face +Y (0.05 away) -> out by 0.05 + r along +Y
    CHECK(SpherePushOut(b, in, 0.1f, out));
    CHECK(Near(out[0], 0, 1e-6f) && Near(out[1], 0.15f, 1e-5f) && Near(out[2], 0, 1e-6f));
    const float far[3] = { 0.7f, 0, 0 };
    CHECK(!SpherePushOut(b, far, 0.1f, out));
}

// The defect SG4 showed: the wrist's line misses the gun but a FINGER does not - the hand must stop on the finger.
void FingerOffTheWristLineStops()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[2][3] = { { 0, 0, 0 }, { 0, 0.6f, 0 } };   // the wrist, and a fingertip 0.6 to the side (the box is +-0.5)
    const float r[2] = { 0.02f, 0.02f };
    const float from[3] = { -2, 0.3f, 0 }, to[3] = { 0, 0.3f, 0 };   // the wrist sphere alone: y 0.3 IS inside the box's band too
    float out[3];
    HandHit hit{};
    CHECK(StopHandOnBodies(body, 1, off, r, 2, from, to, false, out, &hit));
    CHECK(out[0] < -0.5f && out[0] > -0.53f);   // stopped at the -X face (wrist sphere r 0.02), within ~1 mm + the bisection
    CHECK(HandClear(body, 1, off, r, 2, out));
    // and the finger alone: the wrist's line clear of the box (y 0.6 + finger at y -0.3 -> 0.3 inside the band)
    const float off2[2][3] = { { 0, 0, 0 }, { 0, -0.3f, 0 } };
    const float from2[3] = { -2, 0.6f, 0 }, to2[3] = { 0, 0.6f, 0 };
    CHECK(StopHandOnBodies(body, 1, off2, r, 2, from2, to2, false, out, &hit));
    CHECK(hit.sphere == 1);
    CHECK(out[0] < -0.5f && out[0] > -0.53f);
    CHECK(HandClear(body, 1, off2, r, 2, out));
}

void HandFreePathUnmoved()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    const float from[3] = { -2, 1, 0 }, to[3] = { 2, 1, 0 };
    float out[3];
    HandHit hit{};
    CHECK(!StopHandOnBodies(body, 1, off, r, 1, from, to, false, out, &hit));
    CHECK(out[0] == to[0] && out[1] == to[1] && out[2] == to[2]);
}

// The gun swung INTO a still hand (the right hand moves it): the hand starts inside - pushed out, never left in.
void HandStartingInsideIsPushedOut()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[2][3] = { { 0, 0, 0 }, { 0.05f, 0, 0 } };
    const float r[2] = { 0.02f, 0.02f };
    const float from[3] = { -0.52f, 0, 0 }, to[3] = { -0.52f, 0, 0 };   // the finger 0.03 inside the -X face
    float out[3];
    HandHit hit{};
    CHECK(StopHandOnBodies(body, 1, off, r, 2, from, to, false, out, &hit));
    CHECK(hit.startedInside);
    CHECK(HandClear(body, 1, off, r, 2, out));
    CHECK(out[0] < -0.52f && out[0] > -0.6f);   // out through the NEAR face, not through the box
}

// A thin plate (5 mm) crossed in one solve: never tunnels.
void HandNeverTunnelsThinPlate()
{
    const Obb plate[1] = { Box(0, 0, 0, 0.0025f, 0.2f, 0.2f) };
    const GunShape g{ plate, 1, Channel{}, false };
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.008f };
    const float from[3] = { -0.3f, 0, 0 }, to[3] = { 0.3f, 0, 0 };
    float out[3];
    HandHit hit{};
    CHECK(StopHandOnBodies(body, 1, off, r, 1, from, to, false, out, &hit));
    CHECK(out[0] < -0.0105f && out[0] > -0.014f);
}

void HandHiddenPartIgnored()
{
    Obb parts[1] = { Box(0, 0, 0, 0.5f, 0.5f, 0.5f) };
    parts[0].part = 3;
    GunShape g{ parts, 1, Channel{}, false };
    g.hiddenParts = 1u << 3;
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    const float from[3] = { -2, 0, 0 }, to[3] = { 0, 0, 0 };
    float out[3];
    HandHit hit{};
    CHECK(!StopHandOnBodies(body, 1, off, r, 1, from, to, false, out, &hit));
}

// A reload's mag step: the hand at the well's mouth is not stopped by the walls or the grip there (mouth on); without it, it is.
void MouthExemptsOnlyAtTheWell()
{
    const GunShape g = WellGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.012f };
    // up into the mouth beside the left wall's bottom (x -0.05, wall bottom y -0.10): ends 5 mm into the wall's bottom
    const float from[3] = { -0.05f, -0.30f, 0 }, to[3] = { -0.05f, -0.09f, 0 };
    float out[3];
    HandHit hit{};
    CHECK(!StopHandOnBodies(body, 1, off, r, 1, from, to, true, out, &hit));    // mouth: free
    CHECK(StopHandOnBodies(body, 1, off, r, 1, from, to, false, out, &hit));    // no mouth: stopped on the wall
    // ...but the mouth is only the mouth: the same hand far to the side (x -0.20, outside footprint + margin) is still stopped by the
    // receiver when it rises into it
    const Obb wide[1] = { Box(0, 0.025f, 0, 0.30f, 0.025f, 0.04f) };
    GunShape gw = WellGun();
    gw.boxes = wide;
    gw.nBoxes = 1;
    const HandBody bw[1] = { { &gw, kIdentity34 } };
    const float fromS[3] = { -0.20f, -0.30f, 0 }, toS[3] = { -0.20f, 0.02f, 0 };
    CHECK(StopHandOnBodies(bw, 1, off, r, 1, fromS, toS, true, out, &hit));
    // and deeper than kMouthIn into the gun inside the footprint: stopped (the receiver at y 0)
    const float fromD[3] = { 0, -0.30f, 0 }, toD[3] = { 0, 0.02f, 0 };
    CHECK(StopHandOnBodies(body, 1, off, r, 1, fromD, toD, true, out, &hit));
    // a NON-wall box at the mouth (the grip's bottom edge outside the wall, x -0.07 +- 0.01, y -0.10 .. -0.09): a finger whose centre
    // is in the mouth zone (x -0.055 <= halfW 0.03 + margin 0.03; along 0.005 <= kMouthIn) is not stopped by it with the mouth on -
    // the rule the walls' own skip does not cover (mutation "no mouth rule" passed without this case)
    const Obb gripEdge[1] = { Box(-0.07f, -0.095f, 0, 0.01f, 0.005f, 0.04f) };
    GunShape ge = WellGun();
    ge.boxes = gripEdge;
    ge.nBoxes = 1;
    const HandBody be[1] = { { &ge, kIdentity34 } };
    const float fromE[3] = { -0.055f, -0.30f, 0 }, toE[3] = { -0.055f, -0.095f, 0 };
    CHECK(!StopHandOnBodies(be, 1, off, r, 1, fromE, toE, true, out, &hit));
    CHECK(StopHandOnBodies(be, 1, off, r, 1, fromE, toE, false, out, &hit));
}

// The seated mag is a second body in its own frame.
void HandStopsOnSecondBody()
{
    const GunShape g = UnitGun();
    const Obb magBox[1] = { Box(0, 0, 0, 0.05f, 0.05f, 0.05f) };
    const GunShape mg{ magBox, 1, Channel{}, false };
    const float magPose[12] = { 1, 0, 0, 3,  0, 1, 0, 0,  0, 0, 1, 0 };   // at x = 3
    const HandBody bodies[2] = { { &g, kIdentity34 }, { &mg, magPose } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.01f };
    const float from[3] = { 2, 0, 0 }, to[3] = { 3, 0, 0 };
    float out[3];
    HandHit hit{};
    CHECK(StopHandOnBodies(bodies, 2, off, r, 1, from, to, false, out, &hit));
    CHECK(hit.body == 1);
    CHECK(out[0] < 2.94f && out[0] > 2.93f);
}

void HandNonFiniteRefuses()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float from[3] = { nan, 0, 0 }, to[3] = { 0, 0, 0 };
    float out[3];
    HandHit hit{};
    CHECK(!StopHandOnBodies(body, 1, off, r, 1, from, to, false, out, &hit));
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
}

}  // namespace

// ---- THE GHOST HAND (D-309's replacement: one wrist, one owner, slide - never a push along the nearest face) ----

// The ghost is entered only from a CLEAR target: a hand that starts inside passes free (no push-out) until it is clear.
void GhostEntersOnlyFromClear()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float inside[3] = { 0, 0, 0 };
    CHECK(!GhostStep(body, 1, off, r, 1, inside, false, &st, out, &res));
    CHECK(res.waiting && !st.have);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    const float clear[3] = { -0.6f, 0, 0 };
    CHECK(!GhostStep(body, 1, off, r, 1, clear, false, &st, out, &res));
    CHECK(st.have && !res.waiting);
    CHECK(out[0] == clear[0]);
}

// Pressing into a face while moving along it: the motion INTO the face is removed, the rest kept (collide-and-slide).
void GhostSlidesAlongFace()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.6f, 0, 0 };
    GhostStep(body, 1, off, r, 1, start, false, &st, out, &res);
    const float to[3] = { -0.3f, 0.2f, 0.05f };   // 0.23 into the -X face, 0.2 / 0.05 along it
    CHECK(GhostStep(body, 1, off, r, 1, to, false, &st, out, &res));
    CHECK(res.touching);
    CHECK(out[0] < -0.52f && out[0] > -0.523f);   // on the face (r 0.02 + 1 mm)
    CHECK(Near(out[1], 0.2f, 0.002f));            // the tangential motion kept
    CHECK(Near(out[2], 0.05f, 0.002f));
    CHECK(HandClear(body, 1, off, r, 1, out));
    CHECK(Near(st.nrm[0], -1.0f, 0.01f));         // the normal: out of the -X face
}

// SG5's seizure: a hand held pressing into the gun must not move from step to step.
void GhostHeldPressStable()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[2][3] = { { 0, 0, 0 }, { 0.04f, 0.03f, 0 } };
    const float r[2] = { 0.02f, 0.012f };
    GhostState st{};
    GhostResult res{};
    float out[3], prev[3] = { 0, 0, 0 };
    const float start[3] = { -0.7f, 0.1f, 0 };
    GhostStep(body, 1, off, r, 2, start, false, &st, out, &res);
    const float to[3] = { -0.45f, 0.1f, 0 };
    float worst = 0.0f;
    for (int k = 0; k < 60; ++k) {
        CHECK(GhostStep(body, 1, off, r, 2, to, false, &st, out, &res));
        CHECK(!res.depen);
        CHECK(HandClear(body, 1, off, r, 2, out));
        if (k > 3) {   // the first steps settle the 1 mm skin once (the slide budget's catch-up) - then it must not move
            const float d[3] = { out[0] - prev[0], out[1] - prev[1], out[2] - prev[2] };
            const float m = std::sqrt(Dot(d, d));
            if (m > worst) { worst = m; }
        }
        prev[0] = out[0]; prev[1] = out[1]; prev[2] = out[2];
    }
    CHECK(worst < 0.0001f);   // < 0.1 mm step to step
}

// The hand's own turn put a finger into the face it was on: walked back along THAT face's normal only (no sideways shove).
void GhostTurnWalksBackAlongNormal()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float r[2] = { 0.02f, 0.01f };
    // The wrist on the -X face near the +Y edge; the finger 4 cm up, just under the top. After the turn the finger is 14 mm into the
    // -X face but only 5 mm under the +Y face: the NEAREST face is +Y (the old push-out would shove the hand UP) - the ghost must walk
    // back along -X, the face it was on.
    const float offA[2][3] = { { 0, 0, 0 }, { 0, 0.04f, 0 } };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.7f, 0.455f, 0 };
    GhostStep(body, 1, offA, r, 2, start, false, &st, out, &res);
    const float to[3] = { -0.4f, 0.455f, 0 };
    CHECK(GhostStep(body, 1, offA, r, 2, to, false, &st, out, &res));
    const float y0 = out[1], z0 = out[2], x0 = out[0];
    const float offB[2][3] = { { 0, 0, 0 }, { 0.035f, 0.04f, 0 } };   // turned into the face
    CHECK(GhostStep(body, 1, offB, r, 2, to, false, &st, out, &res));
    CHECK(res.depen && !res.dropped);
    CHECK(out[0] < x0 - 0.004f);                 // back along -X
    CHECK(Near(out[1], y0, 1e-5f) && Near(out[2], z0, 1e-5f));   // and nowhere else
    CHECK(HandClear(body, 1, offB, r, 2, out));
}

// A turn deeper than the cap: the ghost is dropped, the hand follows the target (no large shove).
void GhostDeepTurnDrops()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float r[2] = { 0.02f, 0.01f };
    const float offA[2][3] = { { 0, 0, 0 }, { 0, 0.04f, 0 } };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.7f, 0, 0 };
    GhostStep(body, 1, offA, r, 2, start, false, &st, out, &res);
    const float to[3] = { -0.4f, 0, 0 };
    GhostStep(body, 1, offA, r, 2, to, false, &st, out, &res);
    const float offB[2][3] = { { 0, 0, 0 }, { 0.09f, 0.04f, 0 } };   // the finger 6+ cm into the face
    CHECK(!GhostStep(body, 1, offB, r, 2, to, false, &st, out, &res));
    CHECK(res.dropped && !st.have);
    CHECK(out[0] == to[0] && out[1] == to[1] && out[2] == to[2]);
}

// The controller went through and out the far side (free there, > 10 cm away): the ghost jumps to it.
void GhostReleasesThroughTheGun()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.6f, 0, 0 };
    GhostStep(body, 1, off, r, 1, start, false, &st, out, &res);
    const float in[3] = { 0.0f, 0, 0 };
    CHECK(GhostStep(body, 1, off, r, 1, in, false, &st, out, &res));   // inside: held at the face
    CHECK(out[0] < -0.52f);
    const float far[3] = { 0.6f, 0, 0 };
    CHECK(!GhostStep(body, 1, off, r, 1, far, false, &st, out, &res));
    CHECK(res.released);
    CHECK(out[0] == far[0]);
}

// Pulling away from the face: the ghost follows freely, the normal clears.
void GhostFollowsAwayFromFace()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.6f, 0, 0 };
    GhostStep(body, 1, off, r, 1, start, false, &st, out, &res);
    const float in[3] = { -0.45f, 0, 0 };
    CHECK(GhostStep(body, 1, off, r, 1, in, false, &st, out, &res));
    const float away[3] = { -0.58f, 0.05f, 0 };
    CHECK(!GhostStep(body, 1, off, r, 1, away, false, &st, out, &res));
    CHECK(out[0] == away[0] && out[1] == away[1]);
    CHECK(st.nrm[0] == 0 && st.nrm[1] == 0 && st.nrm[2] == 0);
}

// Into an edge (two faces): the slide keeps the motion along the edge, never through either face.
void GhostSlidesIntoCornerStaysOut()
{
    const GunShape g = UnitGun();
    const HandBody body[1] = { { &g, kIdentity34 } };
    const float off[1][3] = { { 0, 0, 0 } };
    const float r[1] = { 0.02f };
    GhostState st{};
    GhostResult res{};
    float out[3];
    const float start[3] = { -0.6f, 0.6f, 0 };
    GhostStep(body, 1, off, r, 1, start, false, &st, out, &res);
    const float to[3] = { -0.3f, 0.3f, 0.1f };   // diagonally into the -X / +Y edge, and along it (z)
    GhostStep(body, 1, off, r, 1, to, false, &st, out, &res);
    CHECK(HandClear(body, 1, off, r, 1, out));
    CHECK(Near(out[2], 0.1f, 0.003f));
}

int main()
{
    PointOutsideHullUnmoved();
    PointIntoBoxStopsAtFace();
    PointStopFollowsGunPose();
    PointIntoChannelPasses();
    GripSpotReachableThroughHull();
    ShoulderInsideBoxIgnored();
    MagAlignedSlidesInChannel();
    MagFreeInChannelUnmoved();
    ChannelAdmitsWithinAlign();
    AdmittedMagAtLipGuidedAcross();
    AdmittedMagFarBelowNotPushed();
    ChannelLipStopsOffAxis();
    MagSidewaysOutBlocked();
    ReceiverStopsMagPastSeat();
    RestingContactPulsesOnce();
    NonFiniteInputRefuses();
    HiddenPartIgnored();
    GripExemptsStop();
    RelFromLastRecoversOffset();
    EscapeDetectedOnlyInsideTheWell();
    EmptyGunNeverStops();
    SphereTouchesBoxByDistance();
    SpherePushOutMinimal();
    FingerOffTheWristLineStops();
    HandFreePathUnmoved();
    HandStartingInsideIsPushedOut();
    HandNeverTunnelsThinPlate();
    HandHiddenPartIgnored();
    MouthExemptsOnlyAtTheWell();
    HandStopsOnSecondBody();
    HandNonFiniteRefuses();
    GhostEntersOnlyFromClear();
    GhostSlidesAlongFace();
    GhostHeldPressStable();
    GhostTurnWalksBackAlongNormal();
    GhostDeepTurnDrops();
    GhostReleasesThroughTheGun();
    GhostFollowsAwayFromFace();
    GhostSlidesIntoCornerStaysOut();
    std::printf("gunshape_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
