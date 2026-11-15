// Standalone unit test for src/src/ovr2xr/nock_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-17-bow-manual-nock.md Task 2. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\nock_logic_test.cpp
#include "nock_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::nock;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static const Params kP = { 0.20f, 0.25f, 0.08f, 60.0f, 0.75f };

// The fixture: the string's rest point at the origin, the bow pointing +X. A bow out, a readable controller.
static Input In(int state)
{
    Input i{};
    i.enabled = true;
    i.haveHand = true;
    i.gameState = state;
    i.trueCount = 12;
    i.lineOrigin = { 0.0f, 0.0f, 0.0f };
    i.lineTip = { 1.0f, 0.0f, 0.0f };
    i.notch = { 0.0f, 0.0f, -0.6f };   // the hand away from the bow (not in the capsule)
    i.tip = { 1.0f, 0.0f, 0.0f };
    return i;
}

static Input Held(int state, Vec3 notch, Vec3 tip)
{
    Input i = In(state);
    i.trigDown = true;
    i.notch = notch;
    i.tip = tip;
    return i;
}

// Empty -> Carry via a take at the shoulder; the game then enters 6.
static void ToCarry(Machine& m)
{
    Input i = In(-1);
    i.pressEdge = true;
    i.handInShoulder = true;
    i.trigDown = true;
    m.Step(i, kP);
}

static void ToCaught(Machine& m)
{
    ToCarry(m);
    m.Step(Held(6, { 0.40f, 0.10f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
}

static Vec3 Tilt(float deg)   // a unit vector `deg` off +X, in the XY plane
{
    const float r = deg * 3.14159265f / 180.0f;
    return { std::cos(r), std::sin(r), 0.0f };
}

static void StartsEmptyFakesCount()
{
    Machine m(false);
    const Output o = m.Step(In(-1), kP);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
}

static void StartsSeatedWhenRemembered()
{
    Machine m(true);
    const Output o = m.Step(In(0), kP);
    CHECK(o.phase == Phase::Seated);
    CHECK(!o.fakeEmpty);
}

static void TakeAtShoulder()
{
    Machine m(false);
    Input i = In(-1);
    i.pressEdge = true;
    i.handInShoulder = true;
    i.trigDown = true;
    const Output o = m.Step(i, kP);
    CHECK(o.ev == Event::Take);
    CHECK(o.phase == Phase::Carry);
    CHECK(!o.fakeEmpty);
    CHECK(o.writeArrow);
    CHECK(!o.onLine);
}

static void TakeRefusedWithNoArrows()
{
    Machine m(false);
    Input i = In(-1);
    i.pressEdge = true;
    i.handInShoulder = true;
    i.trigDown = true;
    i.trueCount = 0;
    const Output o = m.Step(i, kP);
    CHECK(o.ev == Event::TakeNoArrows);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
    CHECK(!o.writeArrow);
}

static void PressOutsideShoulderDoesNothing()
{
    Machine m(false);
    Input i = In(-1);
    i.pressEdge = true;
    i.trigDown = true;
    const Output o = m.Step(i, kP);
    CHECK(o.ev == Event::None);
    CHECK(o.phase == Phase::Empty);
}

static void CarryHoldsPutArrow()
{
    Machine m(false);
    ToCarry(m);
    Output o = m.Step(Held(-1, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP);   // the update before the game enters 6
    CHECK(o.phase == Phase::Carry);
    CHECK(o.ev == Event::None);
    o = m.Step(Held(6, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.holdPutArrow);
    CHECK(o.writeArrow);
    CHECK(!o.onLine);
}

static void EarlyReleaseInCarry()
{
    Machine m(false);
    ToCarry(m);
    const Output o = m.Step(In(6), kP);   // trigDown false
    CHECK(o.ev == Event::EarlyRelease);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
    CHECK(!o.writeArrow);
}

static void EarlyReleaseWhenCaught()
{
    Machine m(false);
    ToCaught(m);
    CHECK(m.phase() == Phase::Caught);
    const Output o = m.Step(In(6), kP);
    CHECK(o.ev == Event::EarlyRelease);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
}

static void CatchInsideCapsuleRightWay()
{
    Machine m(false);
    ToCarry(m);
    const Output o = m.Step(Held(6, { 0.40f, 0.10f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.ev == Event::Catch);
    CHECK(o.phase == Phase::Caught);
    CHECK(o.onLine);
    CHECK(o.writeArrow);
    CHECK(o.holdPutArrow);
    CHECK(Near(o.along, 0.40f, 1e-4f));
}

static void ReversedArrowNeverCatches()
{
    const Vec3 spots[3] = { { 0.10f, 0.0f, 0.0f }, { 0.40f, 0.0f, 0.0f }, { 0.02f, 0.0f, 0.0f } };
    for (const Vec3& s : spots) {
        Machine m(false);
        ToCarry(m);
        Output o = m.Step(Held(6, s, { -1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.phase == Phase::Carry);
        CHECK(o.ev == Event::CatchWrongWay);
        CHECK(Near(o.evValue, 180.0f, 0.5f));
        for (int k = 0; k < 3; ++k) {
            o = m.Step(Held(6, s, { -1.0f, 0.0f, 0.0f }), kP);
            CHECK(o.phase == Phase::Carry);
            CHECK(o.ev == Event::None);
        }
    }
}

static void CatchAngleTolerance()
{
    {
        Machine m(false);
        ToCarry(m);
        const Output o = m.Step(Held(6, { 0.40f, 0.0f, 0.0f }, Tilt(59.0f)), kP);
        CHECK(o.ev == Event::Catch);
    }
    {
        Machine m(false);
        ToCarry(m);
        const Output o = m.Step(Held(6, { 0.40f, 0.0f, 0.0f }, Tilt(61.0f)), kP);
        CHECK(o.ev == Event::CatchWrongWay);
        CHECK(Near(o.evValue, 61.0f, 0.5f));
        CHECK(o.phase == Phase::Carry);
    }
}

static void CatchOffLineTooFar()
{
    {
        Machine m(false);
        ToCarry(m);
        const Output o = m.Step(Held(6, { 0.40f, 0.21f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.phase == Phase::Carry);
    }
    {
        Machine m(false);
        ToCarry(m);
        const Output o = m.Step(Held(6, { 0.40f, 0.19f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.phase == Phase::Caught);
    }
}

static void BehindStringNoCatch()
{
    Machine m(false);
    ToCarry(m);
    const Output o = m.Step(Held(6, { -0.10f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.phase == Phase::Carry);
}

static void CaughtWandersWithinHold()
{
    Machine m(false);
    ToCaught(m);
    Output o = m.Step(Held(6, { 0.30f, 0.24f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.phase == Phase::Caught);
    CHECK(Near(o.along, 0.30f, 1e-4f));
    o = m.Step(Held(6, { 0.30f, 0.26f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.ev == Event::DropToCarry);
    CHECK(o.phase == Phase::Carry);
}

static void SeatAtString()
{
    {
        Machine m(false);
        ToCaught(m);
        const Output o = m.Step(Held(6, { 0.09f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.phase == Phase::Caught);
    }
    {
        Machine m(false);
        ToCaught(m);
        const Output o = m.Step(Held(6, { 0.07f, 0.05f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.ev == Event::Seat);
        CHECK(o.phase == Phase::Seated);
        CHECK(o.endPutArrow);
        CHECK(!o.writeArrow);
        CHECK(!o.fakeEmpty);
    }
}

static void ShotEntersEmptyOnlyAfterSpawn()
{
    Machine m(true);
    const int states[3] = { 1, 3, 4 };
    for (int s : states) {
        Input i = In(s);
        i.spawned = false;
        const Output o = m.Step(i, kP);
        CHECK(o.phase == Phase::Seated);
        CHECK(!o.fakeEmpty);
    }
    Input i = In(4);
    i.spawned = true;
    const Output o = m.Step(i, kP);
    CHECK(o.ev == Event::Shot);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
}

static void CancelKeepsArrow()
{
    Machine m(true);
    CHECK(m.Step(In(2), kP).phase == Phase::Seated);
    CHECK(m.Step(In(0), kP).phase == Phase::Seated);
}

static void GameEmptiedSeatedBow()
{
    Machine m(true);
    const Output o = m.Step(In(-1), kP);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
}

static void GameMovesStateDuringCarryIsEarlyRelease()
{
    {
        Machine m(false);
        ToCarry(m);
        const Output o = m.Step(Held(0, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.ev == Event::GameMoved);
        CHECK(o.phase == Phase::Empty);
        CHECK(o.fakeEmpty);
        CHECK(Near(o.evValue, 0.0f, 1e-6f));
    }
    {
        Machine m(false);
        ToCaught(m);
        const Output o = m.Step(Held(5, { 0.40f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
        CHECK(o.ev == Event::GameMoved);
        CHECK(o.phase == Phase::Empty);
    }
}

static void DisableReleasesEverything()
{
    for (int start = 0; start < 3; ++start) {
        Machine m(false);
        if (start == 1) { ToCarry(m); }
        if (start == 2) { ToCaught(m); }
        Input i = Held(6, { 0.40f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
        i.enabled = false;
        const Output o = m.Step(i, kP);
        CHECK(o.ev == Event::Disabled);
        CHECK(o.phase == Phase::Off);
        CHECK(!o.fakeEmpty);
        CHECK(!o.holdPutArrow);
        CHECK(!o.endPutArrow);
        CHECK(!o.writeArrow);
        const Output o2 = m.Step(In(-1), kP);
        CHECK(o2.phase == Phase::Empty);
    }
}

static void AngleDegBasics()
{
    CHECK(Near(AngleDeg({ 1, 0, 0 }, { 1, 0, 0 }), 0.0f, 1e-3f));
    CHECK(Near(AngleDeg({ 1, 0, 0 }, { -1, 0, 0 }), 180.0f, 1e-3f));
    CHECK(Near(AngleDeg({ 1, 0, 0 }, { 0, 1, 0 }), 90.0f, 1e-3f));
}

static void ItemChangeDuringCarryForgetsCarry()
{
    Memory mem;
    mem.Store(0x1000, Phase::Carry);
    CHECK(!mem.Recall(0x1000));
    mem.Store(0x1000, Phase::Seated);
    CHECK(mem.Recall(0x1000));
    CHECK(!mem.Recall(0x2000));
    // 9 distinct seated bows: the least recently used (0x1000, touched first above) is evicted.
    for (uintptr_t k = 1; k <= 8; ++k) { mem.Store(0x9000 + k, Phase::Seated); }
    CHECK(!mem.Recall(0x1000));
    CHECK(mem.Recall(0x9001));
    CHECK(mem.Recall(0x9008));
}

// Final review 2: a missed tail during state 4 must not let the game's own Reload (5) re-nock the bow silently.
static void SeatedReloadIsShot()
{
    Machine m(true);
    const Output o = m.Step(In(5), kP);
    CHECK(o.ev == Event::Shot);
    CHECK(o.phase == Phase::Empty);
    CHECK(o.fakeEmpty);
}

// Final review 4: no published hand = hold the phase - no catch, no drop, no seat from a stand-in point.
static void NoHandHoldsPhase()
{
    {
        Machine m(false);
        ToCaught(m);
        Input i = Held(6, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });   // a point AT the string: would seat with a hand
        i.haveHand = false;
        Output o = m.Step(i, kP);
        CHECK(o.phase == Phase::Caught);
        CHECK(o.ev == Event::None);
        CHECK(!o.endPutArrow);
        CHECK(o.holdPutArrow);
        i.notch = { 0.40f, 5.0f, 0.0f };                                  // far off the line: would drop with a hand
        o = m.Step(i, kP);
        CHECK(o.phase == Phase::Caught);
        i.trigDown = false;                                               // the trigger still decides
        o = m.Step(i, kP);
        CHECK(o.ev == Event::EarlyRelease);
    }
    {
        Machine m(false);
        ToCarry(m);
        Input i = Held(6, { 0.40f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });  // in the capsule, right way: would catch with a hand
        i.haveHand = false;
        const Output o = m.Step(i, kP);
        CHECK(o.phase == Phase::Carry);
        CHECK(o.ev == Event::None);
        CHECK(!o.writeArrow);   // nothing to write the carried arrow from
    }
}

// Final review 1: the count is faked only for OUR controller and only while BOTH switches are on.
static void ShouldFakeNeedsBothSwitches()
{
    CHECK(ShouldFake(true, true, true, true));
    CHECK(!ShouldFake(true, true, false, true));
    CHECK(!ShouldFake(true, true, true, false));
    CHECK(!ShouldFake(true, false, true, true));
    CHECK(!ShouldFake(false, true, true, true));
}

// Jay 2026-10-17 ("Hold the bones ourselves"): the string / limbs / root are held at the Stand pose ONLY while the nock holds the
// bow empty in the game's Empty state (-1) - never in Seated, Carry, Caught, Off, or while the shot's state 4 is still playing.
static void HoldRestPoseOnlyWhileHeldEmpty()
{
    {
        Machine m(false);
        CHECK(m.Step(In(-1), kP).holdRestPose);
        CHECK(!m.Step(In(6), kP).holdRestPose);   // Empty but the game is in PutArrow (an ammo-type change) - leave it
    }
    {
        Machine m(true);
        CHECK(!m.Step(In(0), kP).holdRestPose);
        Input i = In(4);
        i.spawned = true;
        const Output o = m.Step(i, kP);   // the shot: Empty from here, but state 4 is still the game's Shoot clip
        CHECK(o.phase == Phase::Empty);
        CHECK(!o.holdRestPose);
        CHECK(m.Step(In(-1), kP).holdRestPose);
    }
    {
        Machine m(false);
        Input i = In(-1);
        i.enabled = false;
        CHECK(!m.Step(i, kP).holdRestPose);
    }
}

// Jay 2026-10-17 run 2: "Upon pulling the arrow out, the bow changes position & rotates slightly to the left in the hand. Changes
// position/rotation back upon arrow being notched" - PutArrow's bone_root is 8.55 deg off Stand. Held through the carry.
static void HoldsThroughCarryAndCatch()
{
    Machine m(false);
    ToCarry(m);
    CHECK(m.Step(Held(-1, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP).holdRestPose);   // the update before 6
    CHECK(m.Step(Held(6, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP).holdRestPose);
    const Output o = m.Step(Held(6, { 0.40f, 0.10f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);
    CHECK(o.phase == Phase::Caught);
    CHECK(o.holdRestPose);
    CHECK(!o.holdArrowAtRest);   // caught: the arrow is written on the line, not at rest
}

// ... and for the game's 1.0 s PutArrow -> Stand blend after the seat (m_fpp_bow.scr), with the seated arrow kept on the held bow.
static void HoldsForTheBlendAfterTheSeat()
{
    Machine m(false);
    ToCaught(m);
    Input s = Held(6, { 0.05f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    s.tMs = 1000.0;
    Output o = m.Step(s, kP);
    CHECK(o.ev == Event::Seat);
    CHECK(o.holdRestPose);
    Input i = In(0);
    i.tMs = 1500.0;
    o = m.Step(i, kP);
    CHECK(o.phase == Phase::Seated);
    CHECK(o.holdRestPose);
    CHECK(o.holdArrowAtRest);
    i.tMs = 2100.0;
    o = m.Step(i, kP);
    CHECK(!o.holdRestPose);
    CHECK(!o.holdArrowAtRest);
    // a draw inside the window: the draw owns the bow
    Machine m2(false);
    ToCaught(m2);
    m2.Step(s, kP);
    Input d = In(1);
    d.tMs = 1200.0;
    o = m2.Step(d, kP);
    CHECK(!o.holdRestPose);
    CHECK(!o.holdArrowAtRest);
}

// A bow remembered nocked comes back out in the game's PutArrow (activation enters 6, INV-589 f): end it at once and hold the
// pose through the blend - Jay: "pulling it out would have an arrow ready".
static void RememberedBowSkipsPutArrow()
{
    Machine m(true);
    Input i = In(6);
    i.tMs = 5000.0;
    Output o = m.Step(i, kP);
    CHECK(o.phase == Phase::Seated);
    CHECK(o.endPutArrow);
    CHECK(o.holdRestPose);
    CHECK(o.holdArrowAtRest);
    i.tMs = 5016.0;
    o = m.Step(i, kP);
    CHECK(!o.endPutArrow);   // once
    // remembered nocked but the game came out in Stand (player +3328 == 30): nothing to end
    Machine m2(true);
    Input j = In(0);
    j.tMs = 100.0;
    CHECK(!m2.Step(j, kP).endPutArrow);
}

// Run 2: the bow comes back as a NEW item object on every equip, its controller stays - the memory is keyed on the controller.
static void MemoryKeyIsTheController()
{
    CHECK(MemoryKey(0x10, 0x20) == 0x20);
    CHECK(MemoryKey(0x10, 0) == 0x10);
}

// INV-590 (Jay 2026-10-17 "Build it, ride bow_manual_nock"): the LEFT bow rides ONE fixed grip rel (the draw's) instead of the
// per-frame animated socket - only with the nock and the draw on, the detour live, the left hand, and the item a bow.
static void FixedBowRelGate()
{
    CHECK(UseFixedBowRel(true, true, true, true, true));
    CHECK(!UseFixedBowRel(false, true, true, true, true));    // bow_manual_nock off
    CHECK(!UseFixedBowRel(true, false, true, true, true));    // bow_manual_draw off
    CHECK(!UseFixedBowRel(true, true, false, true, true));    // the detour refused / not installed
    CHECK(!UseFixedBowRel(true, true, true, false, true));    // the right hand
    CHECK(!UseFixedBowRel(true, true, true, true, false));    // not a bow
}

// Jay 2026-10-17: "set it to 0.3m" - the latch reaches at most 0.30 m in front of the string along the firing path; with the tighter
// catch values (0.10 / 0.15 / 0.04 / 35 deg). A PIN of existing behaviour at Jay's numbers (the parameter already existed).
static void CatchLengthAtJaysValues()
{
    const Params p = { 0.10f, 0.15f, 0.04f, 35.0f, 0.30f };
    {
        Machine m(false);
        ToCarry(m);
        CHECK(m.Step(Held(6, { 0.35f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), p).phase == Phase::Carry);    // beyond 0.30
        CHECK(m.Step(Held(6, { 0.25f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), p).ev == Event::Catch);      // inside
    }
    {
        Machine m(false);
        ToCarry(m);
        CHECK(m.Step(Held(6, { 0.25f, 0.11f, 0.0f }, { 1.0f, 0.0f, 0.0f }), p).phase == Phase::Carry);   // 0.11 off the line
        CHECK(m.Step(Held(6, { 0.25f, 0.0f, 0.0f }, Tilt(36.0f)), p).ev == Event::CatchWrongWay);        // 36 deg
    }
}

// Jay 2026-10-17: "Use the game's reload pose, build it" - the right hand takes the Reload clip's finger pose while it holds the arrow
// (carrying or caught), and not otherwise.
static void HoldingFingersWhileCarrying()
{
    Machine m(false);
    CHECK(!m.Step(In(-1), kP).holdFingers);   // empty
    ToCarry(m);
    CHECK(m.Step(Held(6, { 0.0f, 0.0f, -0.6f }, { 1.0f, 0.0f, 0.0f }), kP).holdFingers);   // carrying
    CHECK(m.Step(Held(6, { 0.40f, 0.10f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP).holdFingers);  // caught
    const Output s = m.Step(Held(6, { 0.05f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), kP);     // seated
    CHECK(s.ev == Event::Seat);
    CHECK(!s.holdFingers);
    Machine m2(false);
    ToCarry(m2);
    CHECK(!m2.Step(In(6), kP).holdFingers);   // early release
    Input off = In(-1);
    off.enabled = false;
    CHECK(!m2.Step(off, kP).holdFingers);
}

int main()
{
    HoldingFingersWhileCarrying();
    CatchLengthAtJaysValues();
    FixedBowRelGate();
    HoldRestPoseOnlyWhileHeldEmpty();
    HoldsThroughCarryAndCatch();
    HoldsForTheBlendAfterTheSeat();
    MemoryKeyIsTheController();
    RememberedBowSkipsPutArrow();
    SeatedReloadIsShot();
    NoHandHoldsPhase();
    ShouldFakeNeedsBothSwitches();
    StartsEmptyFakesCount();
    StartsSeatedWhenRemembered();
    TakeAtShoulder();
    TakeRefusedWithNoArrows();
    PressOutsideShoulderDoesNothing();
    CarryHoldsPutArrow();
    EarlyReleaseInCarry();
    EarlyReleaseWhenCaught();
    CatchInsideCapsuleRightWay();
    ReversedArrowNeverCatches();
    CatchAngleTolerance();
    CatchOffLineTooFar();
    BehindStringNoCatch();
    CaughtWandersWithinHold();
    SeatAtString();
    ShotEntersEmptyOnlyAfterSpawn();
    CancelKeepsArrow();
    GameEmptiedSeatedBow();
    GameMovesStateDuringCarryIsEarlyRelease();
    DisableReleasesEverything();
    AngleDegBasics();
    ItemChangeDuringCarryForgetsCarry();
    std::printf("nock_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
