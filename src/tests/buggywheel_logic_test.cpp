// Standalone unit test for src/src/ovr2xr/buggywheel_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-10-buggy-wheel-and-key.md Task 1. Exit code = number of failures.
#include "buggywheel_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::buggywheel;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// The wheel: centre at the origin, column along +Z, angle 0 at +X; rim 0.18 m; full lock 30 deg.
static Frame Base()
{
    Frame f{};
    f.enabled = true;
    f.localDriver = true;
    f.engineOn = false;
    f.canStart = true;
    f.timer = -1.0f;
    f.wheelAxis[2] = 1.0f;
    f.wheelRef[0] = 1.0f;
    f.rimRadiusM = 0.18f;
    f.maxWheelDeg = 30.0f;
    f.gameWheelDeg = 0.0f;
    f.haveKeyPoint = true;
    f.keyPos[0] = 0.40f; f.keyPos[1] = -0.30f; f.keyPos[2] = 0.0f;
    for (int i = 0; i < 2; ++i) {
        f.hand[i].tracked = true;
        f.hand[i].squeeze = 0.0f;
        f.hand[i].pos[0] = 1.0f; f.hand[i].pos[1] = 1.0f; f.hand[i].pos[2] = 1.0f;   // far from everything
        f.hand[i].pinch[0] = 1.0f; f.hand[i].pinch[1] = 1.0f; f.hand[i].pinch[2] = 1.0f;
    }
    return f;
}

// a hand on the rim at `deg` (in the wheel plane, rim radius)
static void OnRim(Frame& f, int i, float deg)
{
    const float r = 0.0174532925f * deg;
    f.hand[i].pos[0] = 0.18f * std::cos(r);
    f.hand[i].pos[1] = 0.18f * std::sin(r);
    f.hand[i].pos[2] = 0.0f;
}

static void OnKey(Frame& f) { for (int k = 0; k < 3; ++k) { f.hand[kRight].pos[k] = f.keyPos[k]; f.hand[kRight].pinch[k] = f.keyPos[k]; } }

// Prime (first poll ever sees the squeeze open), then press.
static Out Press(State& s, Frame& f, int i)
{
    f.hand[i].squeeze = 0.0f;
    s.Step(f);
    f.hand[i].squeeze = 1.0f;
    return s.Step(f);
}

static void RawSteerInvertsGameCurve()
{
    const float rs[] = { -1.0f, -0.5f, -0.1f, 0.0f, 0.3f, 1.0f };
    for (float r : rs) {
        const float raw = RawSteerFromWheel(r * 30.0f, 30.0f);
        const float game = (raw < 0 ? -1.0f : (raw > 0 ? 1.0f : 0.0f)) * std::pow(std::fabs(raw), 1.5f);   // +1063500
        CHECK(Near(game, r, 1e-5f));
    }
    CHECK(Near(RawSteerFromWheel(45.0f, 30.0f), 1.0f, 1e-6f));
    CHECK(Near(RawSteerFromWheel(-45.0f, 30.0f), -1.0f, 1e-6f));
    CHECK(Near(RawSteerFromWheel(10.0f, 0.0f), 0.0f, 1e-6f));   // no limit known -> no steer
}

static void SignedAngleRightHanded()
{
    const float z[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 }, y[3] = { 0, 1, 0 }, ny[3] = { 0, -1, 0 }, zz[3] = { 0, 0, 5 };
    CHECK(Near(SignedAngleAbout(z, x, y), 90.0f, 1e-4f));
    CHECK(Near(SignedAngleAbout(z, x, ny), -90.0f, 1e-4f));
    CHECK(Near(SignedAngleAbout(z, x, zz), 0.0f, 1e-6f));   // `to` has no in-plane part
}

static bool AllOff(const Out& o)
{
    return !o.holdInputs && !o.writeTurn && !o.startTimer && !o.wheelGripped && !o.pinned[0] && !o.pinned[1] &&
           o.events[0] == kNone && o.events[1] == kNone;
}

static void OffWritesNothing()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    f.enabled = false;
    CHECK(AllOff(Press(s, f, kRight)));
}

static void NotLocalDriverWritesNothing()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    f.localDriver = false;
    CHECK(AllOff(Press(s, f, kRight)));
}

static void KeyNeededHoldsInputs()
{
    State s; Frame f = Base();
    CHECK(s.Step(f).holdInputs);
    f.engineOn = true;
    CHECK(!s.Step(f).holdInputs);
    f.engineOn = false; f.timer = 0.5f;          // starting
    CHECK(!s.Step(f).holdInputs);
}

// Ledger ruling (pre-flight): with no key point there is no key to turn, so nothing may hold the engine off.
static void NoKeyPointNoHold()
{
    State s; Frame f = Base(); f.haveKeyPoint = false; OnKey(f);
    const Out o = Press(s, f, kRight);
    CHECK(!o.holdInputs);
    CHECK(o.events[kRight] == kNone);
}

static void KeyTurnStartsOnceAt30()
{
    State s; Frame f = Base(); OnKey(f);
    f.hand[kRight].rollDeg = 0.0f;
    const Out g = Press(s, f, kRight);
    CHECK(g.events[kRight] & kGrabKey);
    CHECK(g.pinned[kRight]);
    int ticks = 0, starts = 0, timerWrites = 0, startAt = -1;
    for (int d = 5; d <= 95; d += 5) {
        f.hand[kRight].rollDeg = -static_cast<float>(d);   // clockwise = negative twist (twist_logic.h)
        const Out o = s.Step(f);
        if (o.events[kRight] & kTick) { ++ticks; }
        if (o.events[kRight] & kStart) { ++starts; startAt = d; }
        if (o.startTimer) { ++timerWrites; }
    }
    CHECK(ticks == 3);        // 10, 20, 30
    CHECK(starts == 1);
    CHECK(startAt == 30);     // not before 30 (25 does nothing), not after
    CHECK(timerWrites == 1);
}

static void EarlyReleaseDoesNothing()
{
    State s; Frame f = Base(); OnKey(f);
    Press(s, f, kRight);
    bool started = false;
    for (int d = 5; d <= 25; d += 5) { f.hand[kRight].rollDeg = -static_cast<float>(d); started |= s.Step(f).startTimer; }
    f.hand[kRight].squeeze = 0.2f;
    const Out o = s.Step(f);
    started |= o.startTimer;
    CHECK(!started);
    CHECK(!o.pinned[kRight]);
    f.hand[kRight].squeeze = 1.0f; f.hand[kRight].rollDeg = -95.0f;   // a NEW press, already turned: the turn is from the new grip
    CHECK(!s.Step(f).startTimer);
}

static void KeyDoesNotStartWithoutFuel()
{
    State s; Frame f = Base(); f.canStart = false; OnKey(f);
    Press(s, f, kRight);
    bool started = false, startEvent = false;
    for (int d = 5; d <= 95; d += 5) {
        f.hand[kRight].rollDeg = -static_cast<float>(d);
        const Out o = s.Step(f);
        started |= o.startTimer; startEvent |= (o.events[kRight] & kStart) != 0;
    }
    CHECK(!started);
    CHECK(!startEvent);
}

static void KeyIgnoredWhenNotNeeded()
{
    State s; Frame f = Base(); f.engineOn = true; OnKey(f);
    const Out o = Press(s, f, kRight);
    CHECK(!(o.events[kRight] & kGrabKey));
    CHECK(!o.pinned[kRight]);
}

static void LeftHandCannotTurnKey()
{
    State s; Frame f = Base();
    for (int k = 0; k < 3; ++k) { f.hand[kLeft].pos[k] = f.keyPos[k]; }
    const Out o = Press(s, f, kLeft);
    CHECK(!(o.events[kLeft] & kGrabKey));
}

// BK3 (Jay: "the key was really hard to grab"): the key is grabbed by the FINGERTIPS (the pinch point), not the palm.
static void KeyGrabUsesFingertips()
{
    State s; Frame f = Base();
    for (int k = 0; k < 3; ++k) { f.hand[kRight].pinch[k] = f.keyPos[k]; f.hand[kRight].pos[k] = f.keyPos[k]; }
    f.hand[kRight].pos[0] += 0.30f;                         // the palm 30 cm away, the fingertips on the key
    CHECK(Press(s, f, kRight).events[kRight] & kGrabKey);
    State s2; Frame g = Base();
    for (int k = 0; k < 3; ++k) { g.hand[kRight].pos[k] = g.keyPos[k]; }   // the palm on the key, the fingertips far (Base)
    CHECK(!(Press(s2, g, kRight).events[kRight] & kGrabKey));
}

// BK3: while the engine is off, a grab near the key goes to the KEY even where the rim is within reach; once on, the rim.
static void KeyWinsOverRimWhileNeeded()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);       // the palm on the rim at (0.18 0 0)
    f.keyPos[0] = 0.32f; f.keyPos[1] = 0.0f; f.keyPos[2] = 0.0f;
    f.hand[kRight].pinch[0] = 0.18f; f.hand[kRight].pinch[1] = 0.0f; f.hand[kRight].pinch[2] = 0.0f;   // fingertips 14 cm from the key
    const Out o = Press(s, f, kRight);
    CHECK(o.events[kRight] & kGrabKey);
    CHECK(!(o.events[kRight] & kGrabWheel));
    State s2; Frame g = f; g.engineOn = true;                  // the key not needed: the same grab is the wheel
    CHECK(Press(s2, g, kRight).events[kRight] & kGrabWheel);
    State s3; Frame h = f;
    h.hand[kRight].pinch[0] = 0.10f;                           // fingertips 22 cm from the key: the wheel
    CHECK(Press(s3, h, kRight).events[kRight] & kGrabWheel);
}

// BK3 (Jay: "you had to place them roughly on the wheel, few cm of leeway where the hands could float"): a wheel grip lands ON the rim -
// the rim's plane (kRimAxialM along the axis, measured on the mesh) at the rim radius, at the angle the hand grabbed.
static void RimGrabSnapsOntoRim()
{
    float o[3];
    const float a[3] = { 0.07f, 0.10f, 0.0f };
    RimGrabLocal(a, 0.164f, o);
    CHECK(Near(o[0], kRimAxialM, 1e-6f) && Near(o[1], 0.164f, 1e-6f) && Near(o[2], 0.0f, 1e-6f));
    const float b[3] = { -0.02f, 0.0f, -0.25f };
    RimGrabLocal(b, 0.164f, o);
    CHECK(Near(o[0], kRimAxialM, 1e-6f) && Near(o[1], 0.0f, 1e-6f) && Near(o[2], -0.164f, 1e-6f));
    const float c[3] = { 0.03f, 0.06f, 0.08f };                // radial 0.10 at 53.13 deg: the angle is kept
    RimGrabLocal(c, 0.164f, o);
    CHECK(Near(o[1], 0.164f * 0.6f, 1e-5f) && Near(o[2], 0.164f * 0.8f, 1e-5f));
    const float d[3] = { 0.02f, 0.0f, 0.0f };                  // on the axis: no angle - the reference direction, never NaN
    RimGrabLocal(d, 0.164f, o);
    CHECK(Near(o[0], kRimAxialM, 1e-6f) && Near(o[1], 0.164f, 1e-6f) && Near(o[2], 0.0f, 1e-6f));
    CHECK(Near(kRimAxialM, 0.030f, 1e-6f));
}

// BK4 (Jay: "Use the hold you had in the render earlier"): the wheel hold = the clip's frame-5 hand relative to the buggy body.
static void RotAxisRightHanded()
{
    const float z[3] = { 0, 0, 2 };   // not unit: normalised inside
    float R[9];
    RotAxis(z, 90.0f, R);
    const float x[3] = { 1, 0, 0 };
    const float y0 = R[3] * x[0] + R[4] * x[1] + R[5] * x[2], x0 = R[0] * x[0] + R[1] * x[1] + R[2] * x[2];
    CHECK(Near(x0, 0.0f, 1e-6f) && Near(y0, 1.0f, 1e-6f));   // x -> +y about +z (the SignedAngleAbout / Rx sense)
}

// a hold: grip point (0.2 0 0.05), hand origin (0.25 0.02 0.08), hand rotation = identity
static const float kTestHold[15] = { 0.2f, 0.0f, 0.05f,   0.25f, 0.02f, 0.08f,   1, 0, 0, 0, 1, 0, 0, 0, 1 };

// Gripped AT the clip's grip point: no turn, the hold is exactly the body-placed clip (the render).
static void HoldAtClipSpotIsTheClip()
{
    const float B[12] = { 0, 0, 1, 10,   0, 1, 0, 20,   -1, 0, 0, 30 };   // rotated 90 about y, moved
    const float centre[3] = { 10, 20, 30 }, axis[3] = { 0, 1, 0 }, ref[3] = { 1, 0, 0 };
    float grip[3];
    BodyPoint(B, kTestHold, grip);
    CHECK(Near(HoldDeltaDeg(B, kTestHold, centre, axis, ref, grip), 0.0f, 1e-3f));
    float hh[3], hand[3], rot[9], wantHand[3];
    HoldWorld(B, kTestHold, centre, axis, 0.0f, hh, hand, rot);
    BodyPoint(B, kTestHold + 3, wantHand);
    CHECK(Near(hand[0], wantHand[0], 1e-5f) && Near(hand[1], wantHand[1], 1e-5f) && Near(hand[2], wantHand[2], 1e-5f));
    CHECK(Near(rot[0], 0.0f, 1e-6f) && Near(rot[2], 1.0f, 1e-6f) && Near(rot[6], -1.0f, 1e-6f));   // = B's rotation x identity
}

// Gripped 90 deg round the wheel (any radius / depth): the hold turns about the column to that angle - the hold's own radius and depth.
static void HoldTurnsToGrabAngle()
{
    const float B[12] = { 1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0 };
    const float centre[3] = { 0, 0, 0 }, axis[3] = { 0, 0, 1 }, ref[3] = { 1, 0, 0 };
    const float grip[3] = { 0.0f, 0.30f, -0.02f };
    const float d = HoldDeltaDeg(B, kTestHold, centre, axis, ref, grip);
    CHECK(Near(d, 90.0f, 1e-3f));                               // +90, not -90
    float hh[3], hand[3], rot[9];
    HoldWorld(B, kTestHold, centre, axis, d, hh, hand, rot);
    CHECK(Near(hh[0], 0.0f, 1e-5f) && Near(hh[1], 0.2f, 1e-5f) && Near(hh[2], 0.05f, 1e-5f));
    CHECK(Near(hand[0], -0.02f, 1e-5f) && Near(hand[1], 0.25f, 1e-5f) && Near(hand[2], 0.08f, 1e-5f));
    CHECK(Near(rot[0], 0.0f, 1e-6f) && Near(rot[3], 1.0f, 1e-6f) && Near(rot[1], -1.0f, 1e-6f));   // Rz(90)
}

// BK5 (Jay: "use that as the point of where the wheel is for alignment, not to actually force those specific spots"): the clip's grip
// point gives the rim's DEPTH along the column and its RADIUS; the hand lands at the angle it grabbed.
static void HoldAlignReadsTheClipRim()
{
    const float B[12] = { 1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0 };
    const float centre[3] = { 0, 0, 0 }, axis[3] = { 0, 0, 2 };   // not unit
    float axial = 0.0f, radial = 0.0f;
    HoldAlign(B, kTestHold, centre, axis, &axial, &radial);       // grip point (0.2 0 0.05)
    CHECK(Near(axial, 0.05f, 1e-6f) && Near(radial, 0.2f, 1e-6f));
    const float B2[12] = { 1, 0, 0, 5,   0, 1, 0, 6,   0, 0, 1, 7 };   // moved with its centre: the same rim
    const float c2[3] = { 5, 6, 7 };
    HoldAlign(B2, kTestHold, c2, axis, &axial, &radial);
    CHECK(Near(axial, 0.05f, 1e-5f) && Near(radial, 0.2f, 1e-5f));
}

static void RimGrabAtKeepsTheAngle()
{
    float o[3];
    const float a[3] = { -0.30f, 0.06f, 0.08f };                 // radial 0.10 at 53.13 deg, 30 cm off the plane
    RimGrabLocalAt(a, 0.1671f, 0.0836f, o);
    CHECK(Near(o[0], 0.0836f, 1e-6f) && Near(o[1], 0.1671f * 0.6f, 1e-5f) && Near(o[2], 0.1671f * 0.8f, 1e-5f));
    const float z[3] = { 0.01f, 0.0f, 0.0f };                    // on the axis: the reference direction
    RimGrabLocalAt(z, 0.17f, 0.08f, o);
    CHECK(Near(o[0], 0.08f, 1e-6f) && Near(o[1], 0.17f, 1e-6f) && Near(o[2], 0.0f, 1e-6f));
}

static void OneHandSteers()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    const Out g = Press(s, f, kRight);
    CHECK(g.events[kRight] & kGrabWheel);
    OnRim(f, kRight, 20.0f);
    const Out o = s.Step(f);
    CHECK(o.wheelGripped && o.writeTurn);
    CHECK(Near(o.wheelDeg, 20.0f, 1e-3f));
    CHECK(Near(o.rawTurn, RawSteerFromWheel(20.0f, 30.0f), 1e-5f));
}

// The wheel starts where the game shows it, so a grab never makes it jump.
static void GrabStartsFromGameAngle()
{
    State s; Frame f = Base(); f.gameWheelDeg = 12.0f; OnRim(f, kRight, 40.0f);
    const Out g = Press(s, f, kRight);
    CHECK(Near(g.wheelDeg, 12.0f, 1e-3f));
}

static void TwoHandsAverage()
{
    State s; Frame f = Base(); OnRim(f, kLeft, 180.0f); OnRim(f, kRight, 0.0f);
    f.hand[kLeft].squeeze = 0.0f; f.hand[kRight].squeeze = 0.0f;
    s.Step(f);
    f.hand[kLeft].squeeze = 1.0f; f.hand[kRight].squeeze = 1.0f;
    s.Step(f);
    OnRim(f, kLeft, 190.0f); OnRim(f, kRight, 30.0f);
    const Out o = s.Step(f);
    CHECK(Near(o.wheelDeg, 20.0f, 1e-3f));
}

// A second hand joining does not make the wheel jump.
static void SecondHandJoinsWithoutJump()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    Press(s, f, kRight);
    OnRim(f, kRight, 20.0f);
    s.Step(f);
    OnRim(f, kLeft, 180.0f);
    f.hand[kLeft].squeeze = 1.0f;   // primed open by the earlier Steps
    const Out o = s.Step(f);
    CHECK(o.events[kLeft] & kGrabWheel);
    CHECK(Near(o.wheelDeg, 20.0f, 1e-3f));
}

static void LimitHolds()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    Press(s, f, kRight);
    OnRim(f, kRight, 80.0f);
    const Out o = s.Step(f);
    CHECK(Near(o.wheelDeg, 30.0f, 1e-3f));
    CHECK(Near(o.rawTurn, 1.0f, 1e-5f));
}

static void ReleaseStopsWriting()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    Press(s, f, kRight);
    f.hand[kRight].squeeze = 0.0f;
    const Out o = s.Step(f);
    CHECK(!o.writeTurn && !o.wheelGripped && !o.pinned[kRight]);
}

static void TrackingLossReleases()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    Press(s, f, kRight);
    f.hand[kRight].tracked = false;
    const Out o = s.Step(f);
    CHECK(!o.writeTurn && !o.pinned[kRight]);
    f.hand[kRight].tracked = true;   // squeeze still held: not a new press
    CHECK(!s.Step(f).wheelGripped);
}

static void GripStateClearsWhenNotDriving()
{
    State s; Frame f = Base(); OnRim(f, kRight, 0.0f);
    Press(s, f, kRight);
    f.localDriver = false;
    s.Step(f);
    f.localDriver = true;            // same hand, squeeze still held
    CHECK(!s.Step(f).wheelGripped);
    f.hand[kRight].squeeze = 0.0f; s.Step(f);
    f.hand[kRight].squeeze = 1.0f;
    CHECK(s.Step(f).wheelGripped);   // a NEW press grips
}

static void OffRimDoesNotGrab()
{
    State s; Frame f = Base();
    f.hand[kRight].pos[0] = 0.0f; f.hand[kRight].pos[1] = 0.0f; f.hand[kRight].pos[2] = 0.0f;   // the hub
    CHECK(!(Press(s, f, kRight).events[kRight] & kGrabWheel));
    State s2; Frame g = Base(); OnRim(g, kRight, 0.0f); g.hand[kRight].pos[2] = 0.15f;          // 15 cm off the rim's plane
    CHECK(!(Press(s2, g, kRight).events[kRight] & kGrabWheel));
}

// The key point (Jay-approved 2026-11-10, renders/key_marked_*.png) is stored relative to the body element 24. Node 24's REST world
// (msh_extract nodes.json: rows (0,0,-1 | 0) (0,1,0 | 0) (1,0,0 | 0)) applied to the relative points must give back the model-space key
// and steering-wheel node 109 - the same 3x4 row-major layout GetElementWorldMatrix returns (translation at [3] [7] [11]).
static void BodyPointRoundTrip()
{
    const float W24[12] = { 0, 0, -1, 0,   0, 1, 0, 0,   1, 0, 0, 0 };
    float k[3], w[3];
    BodyPoint(W24, kKeyRel, k);
    BodyPoint(W24, kWheelRel, w);
    CHECK(Near(k[0], -0.149f, 1e-4f) && Near(k[1], 0.901f, 1e-4f) && Near(k[2], 0.456f, 1e-4f));
    CHECK(Near(w[0], 0.0f, 1e-4f) && Near(w[1], 0.880f, 1e-4f) && Near(w[2], 0.3656f, 1e-4f));
    const float moved[12] = { 1, 0, 0, 10,   0, 1, 0, 20,   0, 0, 1, 30 };   // translation lands in [3] [7] [11]
    float m[3];
    BodyPoint(moved, kKeyRel, m);
    CHECK(Near(m[0], 10.456f, 1e-4f) && Near(m[1], 20.901f, 1e-4f) && Near(m[2], 30.149f, 1e-4f));
}

int main()
{
    BodyPointRoundTrip();
    RawSteerInvertsGameCurve();
    SignedAngleRightHanded();
    OffWritesNothing();
    NotLocalDriverWritesNothing();
    KeyNeededHoldsInputs();
    NoKeyPointNoHold();
    KeyTurnStartsOnceAt30();
    KeyGrabUsesFingertips();
    KeyWinsOverRimWhileNeeded();
    RimGrabSnapsOntoRim();
    EarlyReleaseDoesNothing();
    KeyDoesNotStartWithoutFuel();
    KeyIgnoredWhenNotNeeded();
    LeftHandCannotTurnKey();
    RotAxisRightHanded();
    HoldAtClipSpotIsTheClip();
    HoldTurnsToGrabAngle();
    HoldAlignReadsTheClipRim();
    RimGrabAtKeepsTheAngle();
    OneHandSteers();
    GrabStartsFromGameAngle();
    TwoHandsAverage();
    SecondHandJoinsWithoutJump();
    LimitHolds();
    ReleaseStopsWriting();
    TrackingLossReleases();
    GripStateClearsWhenNotDriving();
    OffRimDoesNotGrab();
    std::printf("buggywheel_logic: %d failure(s)\n", g_fail);
    return g_fail;
}
