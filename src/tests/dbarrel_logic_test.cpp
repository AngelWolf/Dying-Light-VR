// Standalone unit test for src/src/ovr2xr/dbarrel_logic.h + dbarrel_hands.h - NOT part of the DLL build. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\dbarrel_logic_test.cpp
#include "dbarrel_logic.h"
#include "reload_guns.h"   // IsDBarrelMesh - the classifier's list vs the per-mesh table (2026-11-07)

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::reload;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

const DParams kP{};

struct Rig {
    DMachine m;
    DInput in{};
    DOut o{};
    DParams p = kP;
    int adds = 0, haptics = 0, events[static_cast<int>(DEvent::Count)] = {};
    int fxOpen = 0, fxSeat = 0, fxClose = 0;
    explicit Rig(int rounds, int spare = 10, int set = kDbSetA)
    {
        p.set = set;
        in.manualOn = true;
        in.rounds = rounds;
        in.spare = spare;
        in.farLoad = true;
        Step(0.0);   // the first update = the draw
    }
    bool lastGrip = false;
    DOut Step(double dt)
    {
        in.nowS += dt;
        in.gripEdge = in.gripDown && !lastGrip;   // the driver's own edge (DbDraw::gripWas)
        lastGrip = in.gripDown;
        o = m.Update(in, p);
        if (o.callAddBullet) { ++adds; ++in.rounds; --in.spare; }   // the game adds one round (seen from the NEXT update on)
        if (o.haptic) { ++haptics; }
        for (int e = 0; e < static_cast<int>(DEvent::Count); ++e) {
            if (o.Has(static_cast<DEvent>(e))) { ++events[e]; }
        }
        for (int k = 0; k < o.nFx; ++k) {
            if (o.fx[k] == DFx::Open) { ++fxOpen; }
            if (o.fx[k] == DFx::Seat) { ++fxSeat; }
            if (o.fx[k] == DFx::Close) { ++fxClose; }
        }
        return o;
    }
    int N(DEvent e) const { return events[static_cast<int>(e)]; }
    void Open()
    {
        in.gripDown = true; in.nearTop = true;
        Step(0.011);
        in.nearTop = false;
        in.rideValid = true;
        for (float a : { 10.0f, 30.0f, 46.0f }) { in.rideAngle = a; Step(0.011); }
        in.gripDown = false; in.rideValid = false;
        Step(0.011);
        Step(0.011);
    }
    void Pull()
    {
        in.gripDown = true; in.nearPull = true;
        Step(0.011);
        in.nearPull = false;
        for (int i = 0; i < 60 && m.phase() == DPhase::Pulling; ++i) { Step(0.011); }
    }
    void Drop() { in.gripDown = false; Step(0.011); }
    void Take() { in.gripDown = true; in.inHip = true; Step(0.011); in.inHip = false; Step(0.011); }
    // ONE PUSH per entry (Jay after RUN DB2): in to the breech, the push plays, back out (re-arms) - once per shell in the hand.
    void PushOne()
    {
        in.nearLoad = true; in.farLoad = false;
        Step(0.011);
        in.nearLoad = false;
        for (int i = 0; i < 120 && m.phase() == DPhase::Loading; ++i) { Step(0.011); }
        in.farLoad = true;
        Step(0.011);
    }
    void Load()
    {
        for (int k = 0; k < 2 && m.hand() != 0; ++k) { PushOne(); }
        in.gripDown = false;
        Step(0.011);
    }
};

void DrawSetsChambersFromRounds()
{
    Rig r0(0), r1(1), r2(2);
    CHECK(r0.o.ch[0] == DChamber::Spent && r0.o.ch[1] == DChamber::Spent);
    CHECK(r1.o.ch[0] == DChamber::Live && r1.o.ch[1] == DChamber::Spent);   // one round = the LEFT (right fires first)
    CHECK(r2.o.ch[0] == DChamber::Live && r2.o.ch[1] == DChamber::Live);
    CHECK(r2.o.phase == DPhase::Closed && !r2.o.withholdR2 && r2.o.angle == 0.0f);
}

void TopGripOpensTo45AndHangsOpen()
{
    Rig r(2);
    r.in.gripDown = true; r.in.nearTop = true;    r.Step(0.011);
    CHECK(r.N(DEvent::OpenGrab) == 1 && r.o.phase == DPhase::Riding && r.o.locked && r.o.anim == DbAnim::Open && r.o.withholdR2);
    r.in.rideValid = true;
    r.in.rideAngle = 20.0f; r.Step(0.011);
    CHECK(std::fabs(r.o.angle - 20.0f) < 1e-4f && r.N(DEvent::Opened) == 0 && r.o.seatMask == 3);
    r.in.rideAngle = 60.0f; r.Step(0.011);   // past the stop: clamped
    CHECK(r.o.angle == 45.0f && r.N(DEvent::Opened) == 1 && r.fxOpen == 1);
    r.in.rideAngle = 30.0f; r.Step(0.011);   // lifted part way, still held: follows
    CHECK(std::fabs(r.o.angle - 30.0f) < 1e-4f);
    r.in.gripDown = false; r.Step(0.011);     // let go: drops back to 45
    CHECK(r.o.phase == DPhase::Open && r.o.angle == 45.0f && r.o.withholdR2 && !r.o.locked && r.N(DEvent::Closed) == 0);
}

void ReleaseBeforeOpenShutsWithoutSound()
{
    Rig r(2);
    r.in.gripDown = true; r.in.nearTop = true; r.Step(0.011);
    r.in.rideValid = true; r.in.rideAngle = 25.0f; r.Step(0.011);
    r.in.gripDown = false; r.Step(0.011);
    CHECK(r.o.phase == DPhase::Closed && r.o.angle == 0.0f && r.N(DEvent::OpenCancel) == 1 && r.fxOpen == 0 && r.fxClose == 0);
    CHECK(!r.o.withholdR2);
}

void ForendHoldRefusesTheTopGrip()
{
    Rig r(2);
    r.in.gripDown = true; r.in.nearTop = true; r.in.forendHeld = true;    r.Step(0.011);
    CHECK(r.o.phase == DPhase::Closed && r.N(DEvent::OpenGrab) == 0);
}

// RUN DB1: a hand sliding onto the barrels with the grip ALREADY held grabbed (and let go 27 / 33 ms later). Now only a fresh squeeze.
void TopGripNeedsAFreshSqueeze()
{
    Rig r(2);
    r.in.gripDown = true; r.Step(0.011);           // squeezed away from the barrels
    r.in.nearTop = true; r.Step(0.011);             // slides on, still held: no grab
    r.Step(0.011);
    CHECK(r.N(DEvent::OpenGrab) == 0 && r.o.phase == DPhase::Closed);
    r.in.gripDown = false; r.Step(0.011);
    r.in.gripDown = true; r.Step(0.011);            // a fresh squeeze on top: grabs
    CHECK(r.N(DEvent::OpenGrab) == 1 && r.o.phase == DPhase::Riding);
    r.in.gripDown = false; r.Step(0.011);           // let go shut
    r.in.gripDown = true; r.Step(0.011);            // squeeze again in place: grabs again (no leave-and-return needed)
    CHECK(r.N(DEvent::OpenGrab) == 2);
}

// Open, spent shells in, the palm both at the breech and over the barrels (the zones do not overlap in game - this proves the order):
// the pull wins. And the pull needs a fresh squeeze too.
void PullBeatsTheTopGripAndNeedsAFreshSqueeze()
{
    Rig r(0);
    r.Open();
    r.in.gripDown = true; r.Step(0.011);
    r.in.nearPull = true; r.in.nearTop = true; r.Step(0.011);   // held, slid in: nothing
    CHECK(r.N(DEvent::PullStart) == 0 && r.N(DEvent::CloseGrab) == 0);
    r.in.gripDown = false; r.Step(0.011);
    r.in.gripDown = true; r.Step(0.011);
    CHECK(r.N(DEvent::PullStart) == 1 && r.N(DEvent::CloseGrab) == 0 && r.o.phase == DPhase::Pulling);
}

void ZoneOfThePalm()
{
    const float onTop[3] = { 0.0f, kDbBoreY + 0.05f, 0.30f };
    const float under[3] = { 0.02f, 0.015f, 0.389f };               // holderl's place (the forend hold's)
    const float breech[3] = { 0.0f, kDbBoreY + 0.03f, 0.05f };
    const float side[3] = { 0.20f, kDbBoreY + 0.05f, 0.30f };
    const float behindTop[3] = { 0.0f, kDbBoreY + 0.05f, 0.12f };     // above the bore but not along the barrels (z < 0.15)
    CHECK(DbZoneOf(onTop).top && !DbZoneOf(onTop).breech);
    CHECK(!DbZoneOf(under).top && DbZoneOf(under).aboveM < 0.0f);
    CHECK(DbZoneOf(breech).breech && !DbZoneOf(breech).top && DbZoneOf(breech).breechM < 0.06f);
    CHECK(!DbZoneOf(side).top && !DbZoneOf(behindTop).top);
    // the barrels' frame: a point turned WITH the barrels by 30 deg comes back to where it was
    float m[12], pm[3], pb[3];
    DbMeshHinge(30.0f, m);
    for (int k = 0; k < 3; ++k) { pm[k] = m[4 * k] * onTop[0] + m[4 * k + 1] * onTop[1] + m[4 * k + 2] * onTop[2] + m[4 * k + 3]; }
    DbToBarrels(30.0f, pm, pb);
    CHECK(std::fabs(pb[0] - onTop[0]) < 1e-5f && std::fabs(pb[1] - onTop[1]) < 1e-5f && std::fabs(pb[2] - onTop[2]) < 1e-5f);
    DbToBarrels(0.0f, pm, pb);   // NEGATIVE: not turned back
    CHECK(std::fabs(pb[1] - onTop[1]) > 0.01f || std::fabs(pb[2] - onTop[2]) > 0.01f);
}

void LiftClosesWithSoundAndPulse()
{
    Rig r(2);
    r.Open();
    const int h0 = r.haptics;
    r.in.gripDown = true; r.in.nearTop = true; r.Step(0.011);
    r.in.rideValid = true;
    for (float a : { 40.0f, 20.0f, 0.2f }) { r.in.rideAngle = a; r.Step(0.011); }
    CHECK(r.N(DEvent::Closed) == 1 && r.fxClose == 1 && r.o.angle == 0.0f && r.haptics == h0 + 2);   // the grab + the shut
    r.in.gripDown = false; r.in.rideValid = false; r.Step(0.011);
    CHECK(r.o.phase == DPhase::Closed && !r.o.withholdR2);
}

void FlickClosesAcceleratingThenStopsDead()
{
    Rig r(2);
    r.Open();
    r.in.flickUpDps = 500.0f; r.Step(0.011);
    CHECK(r.o.phase == DPhase::Open);                       // one update is not a flick
    r.in.flickUpDps = 500.0f; r.Step(0.011);
    CHECK(r.o.phase == DPhase::FlickClosing && r.N(DEvent::FlickClose) == 1 && r.fxClose == 0);
    r.in.flickUpDps = 0.0f;
    r.Step(0.075);
    CHECK(std::fabs(r.o.angle - 45.0f * 0.75f) < 0.01f);    // a = 45 (1 - u^2), u = 0.5
    r.Step(0.074);
    CHECK(r.o.phase == DPhase::FlickClosing && r.o.angle > 0.0f && r.o.angle < 1.0f);
    r.Step(0.002);
    CHECK(r.o.phase == DPhase::Closed && r.o.angle == 0.0f && r.fxClose == 1 && r.N(DEvent::Closed) == 1 && r.o.haptic);
}

void BelowThresholdNeverFlicks()
{
    Rig r(2);
    r.Open();
    r.in.flickUpDps = 399.0f;
    for (int i = 0; i < 20; ++i) { r.Step(0.011); }
    r.in.flickUpDps = 500.0f; r.Step(0.011);
    r.in.flickUpDps = 0.0f; r.Step(0.011);
    r.in.flickUpDps = 500.0f; r.Step(0.011);              // not consecutive
    CHECK(r.o.phase == DPhase::Open && r.N(DEvent::FlickClose) == 0);
}

void PullTakesBothSpentLockedThenDrops()
{
    Rig r(0);
    r.Open();
    CHECK(r.o.seatMask == 3);
    r.in.gripDown = true; r.in.nearPull = true; r.Step(0.011);
    CHECK(r.N(DEvent::PullStart) == 1 && r.o.phase == DPhase::Pulling && r.o.animMask == 3 && r.o.seatMask == 0 && r.o.locked);
    CHECK(r.o.pullLeadT == 0.0f && r.o.animFrame == 1.0f);   // the start update: no time has passed
    r.Step(0.05);
    CHECK(std::fabs(r.o.pullLeadT - 0.5f) < 1e-3f && r.o.animFrame == 1.0f);   // half the lead-in: the shells half way to frame 1
    r.in.gripDown = false;   // let go mid-pull: the pull is locked, it plays on
    for (int i = 0; i < 60 && r.m.phase() == DPhase::Pulling; ++i) { r.Step(0.011); }
    CHECK(r.N(DEvent::PullDone) == 1 && r.o.ch[0] == DChamber::Empty && r.o.ch[1] == DChamber::Empty);
    r.Step(0.011);           // the grip is up: the spent shells drop
    CHECK(r.N(DEvent::SpentDropped) == 1 && r.o.handMask == 0);
}

void PullTimingIsLeadPlusClip()
{
    Rig r(0);
    r.Open();
    r.in.gripDown = true; r.in.nearPull = true; r.Step(0.0);
    const double t0 = r.in.nowS;
    while (r.m.phase() == DPhase::Pulling) { r.Step(0.001); }
    const double took = r.in.nowS - t0;
    CHECK(std::fabs(took - (0.10 + DbTimeOfFrame(kDbPullFrames))) < 0.0025);
    CHECK(r.o.handMask == 3 && r.o.handSpent && r.o.anim == DbAnim::Pull && r.o.animFrame == static_cast<float>(kDbPullFrames) &&
          r.o.animHeld);
}

void UnfiredBarrelKeepsItsLiveShell()
{
    Rig r(1);   // left live, right spent
    r.Open();
    r.Pull();
    CHECK(r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Empty && r.o.handMask == 2 && r.o.seatMask == 1);
}

void NoTakeWhileSpentShellsAreIn()
{
    Rig r(0);
    r.Open();
    r.Take();
    CHECK(r.N(DEvent::Take) == 0 && r.N(DEvent::TakeNoShell) == 1);
}

void TakeWhileShutDoesNothing()
{
    Rig r(0);
    r.Take();
    CHECK(r.N(DEvent::Take) == 0 && r.N(DEvent::TakeShut) == 1 && r.o.handMask == 0);
}

void FullReloadOfTwo()
{
    Rig r(0, 10);
    r.Open();
    r.Pull();
    r.Drop();
    r.Take();
    CHECK(r.o.handMask == 3 && !r.o.handSpent && r.o.anim == DbAnim::Load && r.o.animFrame == 1.0f);
    r.Load();
    CHECK(r.adds == 2 && r.fxSeat == 2 && r.N(DEvent::LoadStart) == 2 && r.N(DEvent::LoadDone) == 2);   // two pushes
    CHECK(r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Live && r.o.seatMask == 3);
    // close by flick, then the count rises by our two adds: no Shot
    r.in.flickUpDps = 600.0f; r.Step(0.011); r.Step(0.011);
    r.in.flickUpDps = 0.0f;
    for (int i = 0; i < 30; ++i) { r.Step(0.011); }
    CHECK(r.o.phase == DPhase::Closed && r.N(DEvent::Shot) == 0 && r.in.rounds == 2 && !r.o.withholdR2);
}

// One push per entry: the first push loads the LEFT only, then the hand is FREE holding the second shell at the right push's start pose;
// it must leave the breech and come back for the RIGHT push. Each push's seat lands at lead + its own frames.
void EachShellIsItsOwnPush()
{
    Rig r(0, 10);
    r.Open(); r.Pull(); r.Drop(); r.Take();
    double at[2] = { -1, -1 };
    int order[2] = { -1, -1 }, n = 0;
    for (int push = 0; push < 2; ++push) {
        r.in.nearLoad = true; r.in.farLoad = false; r.Step(0.0);
        CHECK(r.o.phase == DPhase::Loading && r.o.locked);
        const double t0 = r.in.nowS;
        while (r.m.phase() == DPhase::Loading) {
            r.Step(0.001);
            if (r.o.callAddBullet && n < 2) { at[n] = r.in.nowS - t0; order[n] = r.o.addChamber; ++n; }
        }
        if (push == 0) {
            // free, the second shell in the hand at the right push's start pose; staying at the breech does NOT push again
            CHECK(n == 1 && r.o.phase == DPhase::Open && !r.o.locked && r.o.handMask == 2 &&
                  r.o.animFrame == static_cast<float>(kDbLoadOneStart));
            for (int i = 0; i < 10; ++i) { r.Step(0.011); }
            CHECK(r.o.phase == DPhase::Open && n == 1);
            r.in.nearLoad = false; r.in.farLoad = true; r.Step(0.011);   // out of the breech ball: re-armed
        }
    }
    CHECK(n == 2 && order[0] == 0 && order[1] == 1 && r.N(DEvent::LoadStart) == 2);
    CHECK(std::fabs(at[0] - (0.10 + DbTimeOfFrame(kDbLoadSeatFrame[0]))) < 0.0025);
    CHECK(std::fabs(at[1] - (0.10 + DbTimeOfFrame(kDbLoadSeatFrame[1] - kDbLoadOneStart + 1))) < 0.0025);
}

void OneLiveStaysThenOneShellIntoTheRight()
{
    Rig r(1, 10);
    r.Open(); r.Pull(); r.Drop(); r.Take();
    CHECK(r.o.handMask == 2 && r.o.animFrame == static_cast<float>(kDbLoadOneStart));
    r.in.nearLoad = true; r.in.farLoad = false; r.Step(0.011);
    CHECK(r.o.anim == DbAnim::Load && r.o.animFrame == static_cast<float>(kDbLoadOneStart) && r.o.animMask == 2 && r.o.seatMask == 1);
    r.in.nearLoad = false;
    for (int i = 0; i < 120 && r.m.phase() == DPhase::Loading; ++i) { r.Step(0.011); }
    CHECK(r.adds == 1 && r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Live);
}

void OneSpareGoesLeftFirst()
{
    Rig r(0, 1);
    r.Open(); r.Pull(); r.Drop(); r.Take();
    CHECK(r.o.handMask == 1);
    r.Load();
    CHECK(r.adds == 1 && r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Empty);
}

// A 2 s hitch inside a push: that push's ONE shell goes in and the push ends - the other shell is never pushed without its own entry.
void HitchInsideAPushLoadsOnlyItsShell()
{
    Rig r(0, 10);
    r.Open(); r.Pull(); r.Drop(); r.Take();
    r.in.nearLoad = true; r.in.farLoad = false; r.Step(0.011);
    r.Step(2.0);
    CHECK(r.adds == 1 && r.o.callAddBullet && r.o.addChamber == 0 && r.N(DEvent::LoadDone) == 1);
    r.Step(0.011);
    CHECK(r.adds == 1 && r.o.handMask == 2);
}

void ShotsMarkRightFirst()
{
    Rig r(2);
    r.in.rounds = 1; r.Step(0.011);
    CHECK(r.N(DEvent::Shot) == 1 && r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Spent);
    r.in.rounds = 0; r.Step(0.011);
    CHECK(r.N(DEvent::Shot) == 2 && r.o.ch[0] == DChamber::Spent && r.o.ch[1] == DChamber::Spent);
}

void FaultedAddEmptiesTheChamber()
{
    DMachine m;
    DInput in{};
    in.manualOn = true; in.rounds = 0; in.spare = 10; in.farLoad = true;
    m.Update(in, kP);
    // open
    in.gripDown = true; in.gripEdge = true; in.nearTop = true; m.Update(in, kP);
    in.gripEdge = false;
    in.rideValid = true; in.rideAngle = 45.0f; m.Update(in, kP);
    in.gripDown = false; in.nearTop = false; in.rideValid = false; m.Update(in, kP);
    // pull, drop, take
    in.gripDown = true; in.gripEdge = true; in.nearPull = true; in.nowS = 1.0; m.Update(in, kP);
    in.gripEdge = false;
    in.nearPull = false; in.nowS = 2.0; m.Update(in, kP);
    in.gripDown = false; m.Update(in, kP);
    in.gripDown = true; in.inHip = true; m.Update(in, kP);
    in.inHip = false;
    in.nearLoad = true; in.farLoad = false; in.nowS = 3.0; m.Update(in, kP);
    in.nearLoad = false;
    in.nowS = 3.0 + 0.1 + DbTimeOfFrame(kDbLoadSeatFrame[0]) + 0.001;
    DOut o = m.Update(in, kP);
    CHECK(o.callAddBullet && o.addChamber == 0);
    m.AddFailed(0);
    CHECK(m.chamber(0) == DChamber::Empty);
}

void NotManualIsIdle()
{
    Rig r(2);
    r.in.manualOn = false;
    r.Step(0.011);
    CHECK(r.o.phase == DPhase::Closed && !r.o.withholdR2 && r.o.events == 0 && !r.o.locked);
}

void RedrawClearsTheHand()
{
    Rig r(0, 10);
    r.Open(); r.Pull(); r.Drop(); r.Take();
    CHECK(r.o.handMask == 3);
    r.in.freshDraw = true; r.Step(0.011); r.in.freshDraw = false;
    CHECK(r.o.handMask == 0 && r.o.phase == DPhase::Closed && r.N(DEvent::HandCleared) == 1 && r.adds == 0);
}

// ---- dbarrel_hands.h

void MuzzleUpRateSignAndSize()
{
    const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    const float a = 10.0f * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    // muzzle (+z) turned toward +y by 10 deg: a turn of -10 deg about +x
    const float up[12] = { 1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0 };
    const float dn[12] = { 1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0 };
    CHECK(std::fabs(DbMuzzleUpDps(I, up, 0.01f) - 1000.0f) < 1.0f);
    CHECK(std::fabs(DbMuzzleUpDps(I, dn, 0.01f) + 1000.0f) < 1.0f);
    float up2[12];
    for (int i = 0; i < 12; ++i) { up2[i] = up[i] * 0.4f; }   // a scaled world (INV-11)
    CHECK(std::fabs(DbMuzzleUpDps(I, up2, 0.01f) - 1000.0f) < 1.0f);
    // a yaw (about +y) is not an upward flick
    const float yaw[12] = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0 };
    CHECK(std::fabs(DbMuzzleUpDps(I, yaw, 0.01f)) < 1.0f);
    CHECK(DbMuzzleUpDps(I, up, 0.0f) == 0.0f);
}

void RideAngleFollowsTheHandDown()
{
    const float* h = kDbOpen[0].hand;
    const float g0[3] = { h[3], h[7], h[11] };
    float m[12], p[3];
    DbMeshHinge(30.0f, m);
    for (int r = 0; r < 3; ++r) { p[r] = m[4 * r] * g0[0] + m[4 * r + 1] * g0[1] + m[4 * r + 2] * g0[2] + m[4 * r + 3]; }
    CHECK(std::fabs(DbRideAngle(g0, p, 0.0f, 45.0f) - 30.0f) < 0.01f);   // the point turned with the barrels reads its own angle
    DbMeshHinge(-20.0f, m);
    for (int r = 0; r < 3; ++r) { p[r] = m[4 * r] * g0[0] + m[4 * r + 1] * g0[1] + m[4 * r + 2] * g0[2] + m[4 * r + 3]; }
    CHECK(DbRideAngle(g0, p, 0.0f, 45.0f) == 0.0f);                         // lifted above shut: clamped
    CHECK(std::fabs(DbRideAngle(g0, g0, 45.0f, 45.0f) - 45.0f) < 1e-4f);
}

void HingeFramesAgree()
{
    // bone_root = identity: the seat at 45 deg rebuilt from bone_reload_shotgun's local == the load's last frame (the generator's C7)
    float hl[12], w[12];
    DbHingeLocal(kDbOpenDeg, hl);
    for (int s = 0; s < 2; ++s) {
        DbMul34(hl, kDbSeat[s], w);
        float d = 0.0f;
        for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(w[i] - kDbLoad[kDbLoadFrames - 1].shellM[s][i])); }
        CHECK(d < 1e-5f);
    }
    // the mesh hinge == hinge local x [I | -pivot]
    float mh[12];
    DbMeshHinge(33.0f, mh);
    DbHingeLocal(33.0f, hl);
    const float q[3] = { 0.02f, 0.11f, 0.31f };
    for (int r = 0; r < 3; ++r) {
        const float a = mh[4 * r] * q[0] + mh[4 * r + 1] * q[1] + mh[4 * r + 2] * q[2] + mh[4 * r + 3];
        const float b = hl[4 * r] * (q[0] - kDbPivot[0]) + hl[4 * r + 1] * (q[1] - kDbPivot[1]) + hl[4 * r + 2] * (q[2] - kDbPivot[2]) + hl[4 * r + 3];
        CHECK(std::fabs(a - b) < 1e-6f);
    }
    // NEGATIVE: the seat with the barrels SHUT is not the load's open-gun place
    DbHingeLocal(0.0f, hl);
    DbMul34(hl, kDbSeat[0], w);
    CHECK(std::fabs(w[11] - kDbLoad[kDbLoadFrames - 1].shellM[0][11]) > 0.005f || std::fabs(w[7] - kDbLoad[kDbLoadFrames - 1].shellM[0][7]) > 0.005f);
}

void AnimWordAdvancesUnlessHeld()
{
    DbAnim a = DbAnim::None;
    float f = 0.0f;
    const long long w = DbAnimPack(1000, DbAnim::Pull, 3.0f, false);
    CHECK(DbAnimUnpack(w, 1000 + 4 * 50, &a, &f) && a == DbAnim::Pull && std::fabs(f - 6.0f) < 0.01f);   // 50 ms = 3 frames
    CHECK(DbAnimUnpack(w, 1000 + 4 * 100, &a, &f) && std::fabs(f - 9.0f) < 0.02f);                       // 100 ms = 6 frames
    const long long h = DbAnimPack(1000, DbAnim::Load, 8.0f, true);
    CHECK(DbAnimUnpack(h, 1000 + 4 * 100, &a, &f) && a == DbAnim::Load && f == 8.0f);                    // held: not advanced
    CHECK(!DbAnimUnpack(w, 1000 + 4 * 151, &a, &f));   // stale
    CHECK(!DbAnimUnpack(0, 1000, &a, &f));
    CHECK(DbAnimPack(1000, DbAnim::None, 1.0f, true) == 0);
    const long long e = DbAnimPack(1000, DbAnim::Pull, static_cast<float>(kDbPullFrames) - 0.5f, false);
    CHECK(DbAnimUnpack(e, 1000 + 4 * 100, &a, &f) && f == static_cast<float>(kDbPullFrames));             // held at the last frame
}

void TablesAreShaped()
{
    volatile int pf = kDbPullFrames, lf = kDbLoadFrames, s0 = kDbLoadSeatFrame[0], s1 = kDbLoadSeatFrame[1], one = kDbLoadOneStart;
    CHECK(pf == 11 && lf == 14);                 // after RUN DB3: the bore slide; two 7-frame pushes
    CHECK(s0 == 7 && s1 == 14 && one == 8);
    CHECK(kDbLoad[kDbLoadFrames - 1].shellOn[0] && kDbLoad[kDbLoadFrames - 1].shellOn[1]);
    CHECK(kDbLoad[kDbLoadFrames - 1].shellM[0][3] > 0.0f && kDbLoad[kDbLoadFrames - 1].shellM[1][3] < 0.0f);   // left +x, right -x
    CHECK(&DbFrameAt(DbAnim::Pull, 0) == &kDbPull[0] && &DbFrameAt(DbAnim::Pull, 99) == &kDbPull[kDbPullFrames - 1]);
    CHECK(&DbFrameAt(DbAnim::Open, 5) == &kDbOpen[0]);
    int k = 0, k2 = 0;
    float t = 0.0f;
    DbFrameSplit(DbAnim::Load, 13.5f, &k, &k2, &t);
    CHECK(k == 13 && k2 == 14 && std::fabs(t - 0.5f) < 1e-6f);
    DbFrameSplit(DbAnim::Load, 80.0f, &k, &k2, &t);
    CHECK(k == 14 && k2 == 14 && t == 0.0f);
}

// ---- 2026-11-07 THE FAMILY (Jay: d / e / g "Ship ON with a's"; "D/G are fine. Leave it as is. E is done now.")

// The mesh list the classifier accepts == the per-mesh table's rows; e alone plays the one-shell set; a's row is a's old constants.
void GunTableMatchesTheClassifier()
{
    int rows = 0;
    for (const DbGunRow& r : kDbGuns) {
        ++rows;
        CHECK(IsDBarrelMesh(r.mesh));
        CHECK(DbGunFor(r.mesh) == &r);
        CHECK((r.set == kDbSetE) == (std::strcmp(r.mesh, "wn_shotgun_e") == 0));
    }
    CHECK(rows == 4);
    for (const char* m : { "wn_shotgun_a", "wn_shotgun_d", "wn_shotgun_e", "wn_shotgun_g" }) { CHECK(DbGunFor(m) != nullptr); }
    CHECK(DbGunFor("WN_SHOTGUN_G") == DbGunFor("wn_shotgun_g"));
    CHECK(DbGunFor("wn_shotgun_b") == nullptr && DbGunFor("wn_shotgun_a_hq") == nullptr && DbGunFor(nullptr) == nullptr);   // NEGATIVE
    const DbGunRow* a = DbGunFor("wn_shotgun_a");
    float d = 0.0f;
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(a->ammoRest[s][i] - kDbAmmoRest[s][i])); }
    }
    CHECK(d < 1e-6f && std::fabs(a->topZ1 - kDbTopZ1) < 5e-5f);
    CHECK(DbGunFor("wn_shotgun_g")->topZ1 < 0.56f && DbGunFor("wn_shotgun_e")->topZ1 < 0.62f);   // the shorter barrels
}

// The band over the barrels ends at THIS gun's muzzle: a palm at z 0.60 is over a's barrels but past g's.
void TopBandEndsAtEachMuzzle()
{
    const float p[3] = { 0.0f, kDbBoreY + 0.05f, 0.60f };
    CHECK(DbZoneOf(p).top);
    CHECK(DbZoneOf(p, DbGunFor("wn_shotgun_a")->topZ1).top && DbZoneOf(p, DbGunFor("wn_shotgun_d")->topZ1).top);
    CHECK(!DbZoneOf(p, DbGunFor("wn_shotgun_g")->topZ1).top);
    const float q[3] = { 0.0f, kDbBoreY + 0.05f, 0.40f };   // the middle of every gun's barrels
    for (const DbGunRow& r : kDbGuns) { CHECK(DbZoneOf(q, r.topZ1).top); }
}

// The Ox e: ONE chamber. Drawn with 1 round = live, 0 = spent; the second chamber is always empty and never drawn.
void OxDrawHasOneChamber()
{
    Rig r1(1, 10, kDbSetE), r0(0, 10, kDbSetE), r9(2, 10, kDbSetE);
    CHECK(r1.o.ch[0] == DChamber::Live && r1.o.ch[1] == DChamber::Empty);
    CHECK(r0.o.ch[0] == DChamber::Spent && r0.o.ch[1] == DChamber::Empty);
    CHECK(r9.o.ch[0] == DChamber::Live && r9.o.ch[1] == DChamber::Empty);   // a count above its capacity never fills a second chamber
}

// The Ox's whole reload: shoot, open (its seat drawn), pull ONE spent shell (e's pull), take ONE from the hip (spare 10), ONE push (e's
// load, its seat frame) = one round, shut. Element 1 is never drawn at any step.
void OxFullReloadIsOneShell()
{
    Rig r(1, 10, kDbSetE);
    int seat1 = 0, anim1 = 0, hand1 = 0;
    auto watch = [&]() {
        if (r.o.seatMask & 2) { ++seat1; }
        if (r.o.animMask & 2) { ++anim1; }
        if (r.o.handMask & 2) { ++hand1; }
    };
    r.in.rounds = 0; r.Step(0.011); watch();
    CHECK(r.N(DEvent::Shot) == 1 && r.o.ch[0] == DChamber::Spent);
    r.Open(); watch();
    CHECK(r.o.phase == DPhase::Open && r.o.seatMask == 1);
    r.in.gripDown = true; r.in.nearPull = true; r.Step(0.011); watch();
    CHECK(r.N(DEvent::PullStart) == 1 && r.o.anim == DbAnim::PullE && r.o.animMask == 1);
    r.in.nearPull = false;
    double took = 0.0;
    for (int i = 0; i < 1000 && r.m.phase() == DPhase::Pulling; ++i) { r.Step(0.001); took += 0.001; watch(); }
    CHECK(std::fabs(took - (0.10 + DbTimeOfFrame(kDbEPullFrames))) < 0.0025);
    CHECK(r.o.handMask == 1 && r.o.handSpent && r.o.anim == DbAnim::PullE && r.o.ch[0] == DChamber::Empty);
    r.Drop(); watch();
    CHECK(r.N(DEvent::SpentDropped) == 1 && r.o.handMask == 0);
    r.Take(); watch();
    CHECK(r.N(DEvent::Take) == 1 && r.m.hand() == 1 && r.o.anim == DbAnim::LoadE && r.o.animFrame == 1.0f);   // ONE shell, spare 10
    r.in.nearLoad = true; r.in.farLoad = false; r.Step(0.011); watch();
    CHECK(r.N(DEvent::LoadStart) == 1 && r.o.anim == DbAnim::LoadE && r.o.animMask == 1);
    r.in.nearLoad = false;
    double at = -1.0, t = 0.0;
    for (int i = 0; i < 400 && r.m.phase() == DPhase::Loading; ++i) {
        r.Step(0.001); t += 0.001; watch();
        if (r.o.Has(DEvent::ShellIn) && at < 0.0) { at = t; }
    }
    CHECK(r.adds == 1 && r.N(DEvent::ShellIn) == 1 && r.N(DEvent::LoadDone) == 1);
    CHECK(std::fabs(at - (0.10 + DbTimeOfFrame(kDbELoadSeatFrame))) < 0.0035);
    CHECK(r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Empty && r.m.hand() == 0 && r.in.rounds == 1);
    r.in.gripDown = false; r.Step(0.011); watch();
    r.in.gripDown = true; r.in.inHip = true; r.Step(0.011); watch(); r.in.inHip = false; r.in.gripDown = false; r.Step(0.011);
    CHECK(r.N(DEvent::TakeNoShell) == 1 && r.m.hand() == 0);   // full: no empty chamber, nothing taken
    CHECK(seat1 == 0 && anim1 == 0 && hand1 == 0);
}

// e's tables: element 0 on, element 1 never; the seat rebuilt at 45 deg == the push's last frame; the hand rows == a's (Jay moved only
// the shell); the finger word carries e's anims; DbFrameAt clamps into e's tables.
void OxTablesAreShaped()
{
    for (int k = 0; k < kDbEPullFrames; ++k) { CHECK(kDbEPull[k].shellOn[0] && !kDbEPull[k].shellOn[1]); }
    for (int k = 0; k < kDbELoadFrames; ++k) { CHECK(kDbELoad[k].shellOn[0] && !kDbELoad[k].shellOn[1]); }
    float hl[12], w[12], d = 0.0f;
    DbHingeLocal(kDbOpenDeg, hl);
    DbMul34(hl, kDbESeat, w);
    for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(w[i] - kDbELoad[kDbELoadFrames - 1].shellM[0][i])); }
    CHECK(d < 1e-5f);
    CHECK(std::fabs(w[3]) < 0.005f);   // e's seat is on the centre line (Jay's), not a's left chamber at +11.5 mm
    float dh = 0.0f;
    for (int k = 0; k < kDbEPullFrames; ++k) {
        for (int i = 0; i < 12; ++i) { dh = std::fmax(dh, std::fabs(kDbEPull[k].hand[i] - kDbPull[k].hand[i])); }
    }
    for (int k = 0; k < kDbELoadFrames; ++k) {
        for (int i = 0; i < 12; ++i) { dh = std::fmax(dh, std::fabs(kDbELoad[k].hand[i] - kDbLoad[k].hand[i])); }
    }
    CHECK(dh == 0.0f);
    // NEGATIVE: e's shell path is not a's
    CHECK(std::fabs(kDbELoad[kDbELoadFrames - 1].shellM[0][3] - kDbLoad[kDbELoadFrames - 1].shellM[0][3]) > 0.005f);
    CHECK(&DbFrameAt(DbAnim::PullE, 0) == &kDbEPull[0] && &DbFrameAt(DbAnim::PullE, 99) == &kDbEPull[kDbEPullFrames - 1]);
    CHECK(&DbFrameAt(DbAnim::LoadE, 99) == &kDbELoad[kDbELoadFrames - 1]);
    DbAnim a = DbAnim::None;
    float f = 0.0f;
    CHECK(DbAnimUnpack(DbAnimPack(1000, DbAnim::LoadE, 3.0f, true), 1000, &a, &f) && a == DbAnim::LoadE && f == 3.0f);
    CHECK(DbAnimUnpack(DbAnimPack(1000, DbAnim::PullE, 2.0f, false), 1000, &a, &f) && a == DbAnim::PullE);
    // NEGATIVE: no table, no word (2026-11-09: ids 6..8 are the single-shot's now - the first id past the tables is kDbAnimMaxId + 1)
    CHECK(DbAnimPack(1000, static_cast<DbAnim>(kDbAnimMaxId + 1), 1.0f, true) == 0);
    CHECK(DbChambers(kDbSetE) == 1 && DbChambers(kDbSetA) == 2 && DbSeatOf(kDbSetE, 0) == kDbESeat && DbSeatOf(kDbSetA, 1) == kDbSeat[1]);
}

// a's flow is unchanged by the family: set A explicitly == the default.
void SetAIsTheDefault()
{
    Rig r(2, 10, kDbSetA);
    CHECK(r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Live);
    r.in.rounds = 0; r.Step(0.011);
    r.Open();
    r.Pull();
    CHECK(r.o.handMask == 3 && r.o.anim == DbAnim::Pull);
}

// ---- The SINGLE-SHOT wn_pistol_e (plan 2026-11-09-manual-reload-remaining-guns Task 3; Jay: "Break the barrel by hand, like the OX
// shotgun", open "68") ----
void Pt(const float m[12], const float q[3], float p[3])
{
    for (int r = 0; r < 3; ++r) { p[r] = m[4 * r] * q[0] + m[4 * r + 1] * q[1] + m[4 * r + 2] * q[2] + m[4 * r + 3]; }
}

// the per-gun geometry: wn_shotgun_a's row reproduces every fixed function bit for bit (the verified double-barrels never move)
void GeomAMatchesTheFixedFunctions()
{
    const float q[3] = { 0.02f, 0.13f, 0.41f }, g0[3] = { 0.0f, 0.15f, 0.30f };
    for (float a : { 0.0f, 22.0f, 45.0f, -20.0f }) {
        float m1[12], m2[12];
        DbMeshHinge(a, m1);
        DbMeshHingeG(kDbGeomA, a, m2);
        float d = 0.0f;
        for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(m1[i] - m2[i])); }
        CHECK(d < 1e-6f);
        float b1[3], b2[3];
        DbToBarrels(a, q, b1);
        DbToBarrelsG(kDbGeomA, a, q, b2);
        CHECK(std::fabs(b1[0] - b2[0]) < 1e-6f && std::fabs(b1[1] - b2[1]) < 1e-6f && std::fabs(b1[2] - b2[2]) < 1e-6f);
        float p[3];
        Pt(m1, g0, p);
        CHECK(std::fabs(DbRideAngle(g0, p, 0.0f, 45.0f) - DbRideAngleG(kDbGeomA, g0, p, 0.0f, 45.0f)) < 1e-4f);
    }
    const DbZone z1 = DbZoneOf(q, 0.6834f), z2 = DbZoneOfG(kDbGeomA, q);
    CHECK(z1.top == z2.top && z1.breech == z2.breech && std::fabs(z1.breechM - z2.breechM) < 1e-6f);
}

// the single-shot's hinge: its pivot stays put, a point turned by +a about ITS axis reads a back, clamped to 0 .. 68
void PeHingeKeepsItsPivotAndAxis()
{
    float m[12], p[3];
    DbMeshHingeG(kDbGeomPe, kDbPeOpenDeg, m);
    Pt(m, kDbPePivot, p);
    CHECK(std::fabs(p[0] - kDbPePivot[0]) < 1e-6f && std::fabs(p[1] - kDbPePivot[1]) < 1e-6f && std::fabs(p[2] - kDbPePivot[2]) < 1e-6f);
    const float g0[3] = { 0.0f, 0.09f, 0.28f };   // on the barrel, ahead of the pivot
    DbMeshHingeG(kDbGeomPe, 40.0f, m);
    Pt(m, g0, p);
    CHECK(std::fabs(DbRideAngleG(kDbGeomPe, g0, p, 0.0f, kDbPeOpenDeg) - 40.0f) < 0.01f);
    DbMeshHingeG(kDbGeomPe, 90.0f, m);
    Pt(m, g0, p);
    CHECK(std::fabs(DbRideAngleG(kDbGeomPe, g0, p, 0.0f, kDbPeOpenDeg) - kDbPeOpenDeg) < 1e-4f);   // past open: clamped
    float back[3];
    DbToBarrelsG(kDbGeomPe, 90.0f, p, back);
    CHECK(std::fabs(back[0] - g0[0]) < 1e-5f && std::fabs(back[1] - g0[1]) < 1e-5f && std::fabs(back[2] - g0[2]) < 1e-5f);
}

// the seat rebuilt at 68 deg from the hinge's rest == the load's seat frame (the generator's C1, here through the runtime helpers)
void PeSeatAtOpenIsTheLoadsSeatFrame()
{
    float h[12], w1[12], w[12];
    DbMeshHingeG(kDbGeomPe, kDbPeOpenDeg, h);
    DbMul34(h, kDbPeHingeRest, w1);
    DbMul34(w1, kDbPeSeat, w);
    float d = 0.0f;
    for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(w[i] - kDbPeLoad[kDbPeLoadSeatFrame - 1].shellM[0][i])); }
    CHECK(d < 1e-4f);
}

void SingleShotSetIsOneChamber()
{
    CHECK(DbChambers(kDbSetPe) == 1);
    CHECK(DbPullAnim(kDbSetPe) == DbAnim::PullPe && DbLoadAnim(kDbSetPe) == DbAnim::LoadPe && DbOpenAnim(kDbSetPe) == DbAnim::OpenPe);
    CHECK(DbOpenAnim(kDbSetA) == DbAnim::Open && DbOpenAnim(kDbSetE) == DbAnim::Open);
    CHECK(DbIsPull(DbAnim::PullPe) && DbIsLoad(DbAnim::LoadPe));
    CHECK(DbPullFramesOf(kDbSetPe) == 11 && DbLoadFramesOf(kDbSetPe) == 7 && DbSeatFrameOf(kDbSetPe, 0) == kDbPeLoadSeatFrame);
    CHECK(DbSeatOf(kDbSetPe, 0) == kDbPeSeat && &DbGeomOf(kDbSetPe) == &kDbGeomPe && &DbGeomOf(kDbSetE) == &kDbGeomA);
    CHECK(&DbFrameAt(DbAnim::OpenPe, 1) == &kDbPeOpen[0] && &DbFrameAt(DbAnim::Open, 1) == &kDbOpen[0]);
    CHECK(&DbFrameAt(DbAnim::LoadPe, 99) == &kDbPeLoad[6]);
    long long w = DbAnimPack(4000, DbAnim::LoadPe, 3.0f, false);
    DbAnim a = DbAnim::None;
    float f = 0.0f;
    CHECK(DbAnimUnpack(w, 4000, &a, &f) && a == DbAnim::LoadPe && std::fabs(f - 3.0f) < 1e-3f);
    w = DbAnimPack(4000, DbAnim::OpenPe, 1.0f, true);
    CHECK(DbAnimUnpack(w, 4000, &a, &f) && a == DbAnim::OpenPe);
}

// the whole single-shot reload: shot, open by hand to 68 (its own open grip), pull the spent round (its pull), take one, one push (its
// load, the add at its seat frame), close by the upward flick
void SingleShotFullReload()
{
    Rig r(1, 10, kDbSetPe);
    r.p.openDeg = kDbPeOpenDeg;
    r.in.rounds = 0; r.Step(0.011);
    CHECK(r.N(DEvent::Shot) == 1 && r.o.ch[0] == DChamber::Spent && r.o.ch[1] == DChamber::Empty);
    r.in.gripDown = true; r.in.nearTop = true; r.Step(0.011);
    CHECK(r.o.anim == DbAnim::OpenPe && r.o.locked);
    r.in.nearTop = false; r.in.rideValid = true;
    for (float a : { 20.0f, 50.0f, 69.0f }) { r.in.rideAngle = a; r.Step(0.011); }
    CHECK(r.N(DEvent::Opened) == 1 && std::fabs(r.o.angle - kDbPeOpenDeg) < 1e-4f);
    r.in.gripDown = false; r.in.rideValid = false; r.Step(0.011);
    CHECK(r.o.phase == DPhase::Open && std::fabs(r.o.angle - kDbPeOpenDeg) < 1e-4f && r.o.seatMask == 1);
    r.Pull();
    CHECK(r.o.handMask == 1 && r.o.handSpent && r.o.anim == DbAnim::PullPe && r.o.ch[0] == DChamber::Empty);
    r.Drop();
    r.Take();
    CHECK(r.m.hand() == 1 && r.o.anim == DbAnim::LoadPe);
    r.PushOne();
    CHECK(r.adds == 1 && r.in.rounds == 1 && r.o.ch[0] == DChamber::Live && r.o.ch[1] == DChamber::Empty);
    r.in.gripDown = false; r.Step(0.011);
    r.in.flickUpDps = 450.0f; r.Step(0.011); r.Step(0.011);
    r.in.flickUpDps = 0.0f;
    for (int i = 0; i < 40 && r.m.phase() != DPhase::Closed; ++i) { r.Step(0.011); }
    CHECK(r.N(DEvent::FlickClose) == 1 && r.N(DEvent::Closed) == 1 && r.o.phase == DPhase::Closed && !r.o.withholdR2);
}

// Task 8 (the driver's per-set facts): the single-shot's row and element names; bone_root's rest (identity on a, rotated on the pistol);
// the hinge's world from the MESH frame == a's old local path on a
void PerSetDriverFacts()
{
    const DbGunRow* r = DbGunFor("WN_PISTOL_E");
    CHECK(r != nullptr && r->set == kDbSetPe && std::fabs(r->topZ1 - 0.3247f) < 1e-4f);
    CHECK(DbGunFor("wn_shotgun_a") != nullptr && DbGunFor("wn_shotgun_a")->set == kDbSetA);
    CHECK(std::strcmp(DbNamesOf(kDbSetPe).hinge, "bone_reload_schofield") == 0 && DbNamesOf(kDbSetPe).ammo[1] == nullptr);
    CHECK(std::strcmp(DbNamesOf(kDbSetPe).ammo[0], "bullet_pistol_vancrane") == 0 && std::strcmp(DbNamesOf(kDbSetPe).holder[0], "holder_bullet") == 0);
    CHECK(std::strcmp(DbNamesOf(kDbSetA).hinge, "bone_reload_shotgun") == 0 && std::strcmp(DbNamesOf(kDbSetE).ammo[1], "ammo_bullet_b") == 0);
    CHECK(DbOpenDegOf(kDbSetPe) == kDbPeOpenDeg && DbOpenDegOf(kDbSetA) == kDbOpenDeg && DbOpenDegOf(kDbSetE) == kDbOpenDeg);
    const float* ra = DbRootRestOf(kDbSetA);
    for (int i = 0; i < 12; ++i) { CHECK(std::fabs(ra[i] - ((i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f)) < 1e-7f); }
    CHECK(DbRootRestOf(kDbSetPe) == kDbPeRootRest && DbAmmoRestOf(kDbSetPe, r, 0) == kDbPeAmmoRest);
    // a: the hinge world from the mesh frame (bone_root = identity) == the old bone_root x DbHingeLocal path
    const float rootW[12] = { 0.0f, -1.0f, 0.0f, 0.3f, 1.0f, 0.0f, 0.0f, 1.2f, 0.0f, 0.0f, 1.0f, -0.4f };
    float a1[12], a2[12], hl[12];
    DbHingeLocal(30.0f, hl);
    DbMul34(rootW, hl, a1);
    DbHingeWorld(kDbSetA, rootW, 30.0f, a2);
    float d = 0.0f;
    for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(a1[i] - a2[i])); }
    CHECK(d < 1e-5f);
    // the pistol at 0 deg: the hinge bone's own rest world
    const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    DbHingeWorld(kDbSetPe, I, 0.0f, a2);
    d = 0.0f;
    for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(a2[i] - kDbPeHingeRest[i])); }
    CHECK(d < 1e-6f);
}

// Task 8: the finger pass's word carries the Schofield's push and Jay's static poses (fan-cock x3, bottle, cord) - DbFrameAt finds each
// table, the word packs and unpacks every id, nothing past kDbAnimMaxId packs
void ExtraAnimIds()
{
    CHECK(DbAnimFrames(DbAnim::LoadSch) == 7 && &DbFrameAt(DbAnim::LoadSch, 3) == &kSchLoad[2] && &DbFrameAt(DbAnim::LoadSch, 99) == &kSchLoad[6]);
    CHECK(&DbFrameAt(DbAnim::FanSch, 1) == &kFanSch[0] && &DbFrameAt(DbAnim::FanSix, 1) == &kFanSix[0]);
    CHECK(&DbFrameAt(DbAnim::FanWalker, 1) == &kFanWalker[0] && &DbFrameAt(DbAnim::Bottle, 1) == &kBottle[0] && &DbFrameAt(DbAnim::Cord, 1) == &kCord[0]);
    CHECK(DbAnimFrames(DbAnim::Bottle) == 1 && kBottle[0].shellOn[0] && kCord[0].shellOn[0] && !kFanSch[0].shellOn[0]);
    for (int id = 1; id <= kDbAnimMaxId; ++id) {
        DbAnim a = DbAnim::None;
        float f = 0.0f;
        CHECK(DbAnimUnpack(DbAnimPack(5000, static_cast<DbAnim>(id), 1.0f, true), 5000, &a, &f) && static_cast<int>(a) == id);
    }
    static_assert(kDbAnimMaxId == 18, "the finger word's ids: 1..18 (2026-11-09: + the gate pushes 15 / 16, RG6: + their holds 17 / 18)");
    // RG6 (Jay: "the initial hand pose, with the shell inbetween the thumb & index finger isn't happening" -> "Yes, frame 1 pinch"): the
    // held pose is ONE frame (his scene's frame 1), whatever frame is asked
    CHECK(&DbFrameAt(DbAnim::HoldSix, 1) == &kSixHold[0] && &DbFrameAt(DbAnim::HoldWalker, 7) == &kWalkerHold[0]);
    CHECK(DbAnimFrames(DbAnim::HoldWalker) == 1 && DbStaticPose(DbAnim::HoldSix));
    CHECK(&DbFrameAt(DbAnim::LoadSix, 2) == &kSixLoad[1] && &DbFrameAt(DbAnim::LoadWalker, 99) == &kWalkerLoad[6]);
    float dd = 0.0f;   // each gate gun's seat frame IS its seated round
    for (int i = 0; i < 12; ++i) {
        dd = std::fmax(dd, std::fabs(kSixLoad[kGateLoadSeatFrame - 1].shellM[0][i] - kSixSeatRound[i]));
        dd = std::fmax(dd, std::fabs(kWalkerLoad[kGateLoadSeatFrame - 1].shellM[0][i] - kWalkerSeatRound[i]));
    }
    CHECK(dd < 1e-4f);
    // the extra rows are built on the SAME hand as the double-barrel tables (the finger pass checks the rests once against kDbOpen)
    float worst = 0.0f;
    for (int k = 0; k < 16; ++k) {
        for (int i = 0; i < 9; ++i) { worst = std::fmax(worst, std::fabs(kFanSch[0].fingers[k].rest[i] - kDbOpen[0].fingers[k].rest[i])); }
    }
    CHECK(worst < 0.02f);
}

}  // namespace

int main()
{
    RUN(ExtraAnimIds);
    RUN(PerSetDriverFacts);
    RUN(GeomAMatchesTheFixedFunctions);
    RUN(PeHingeKeepsItsPivotAndAxis);
    RUN(PeSeatAtOpenIsTheLoadsSeatFrame);
    RUN(SingleShotSetIsOneChamber);
    RUN(SingleShotFullReload);
    RUN(GunTableMatchesTheClassifier);
    RUN(TopBandEndsAtEachMuzzle);
    RUN(OxDrawHasOneChamber);
    RUN(OxFullReloadIsOneShell);
    RUN(OxTablesAreShaped);
    RUN(SetAIsTheDefault);
    RUN(DrawSetsChambersFromRounds);
    RUN(TopGripOpensTo45AndHangsOpen);
    RUN(ReleaseBeforeOpenShutsWithoutSound);
    RUN(ForendHoldRefusesTheTopGrip);
    RUN(TopGripNeedsAFreshSqueeze);
    RUN(PullBeatsTheTopGripAndNeedsAFreshSqueeze);
    RUN(ZoneOfThePalm);
    RUN(LiftClosesWithSoundAndPulse);
    RUN(FlickClosesAcceleratingThenStopsDead);
    RUN(BelowThresholdNeverFlicks);
    RUN(PullTakesBothSpentLockedThenDrops);
    RUN(PullTimingIsLeadPlusClip);
    RUN(UnfiredBarrelKeepsItsLiveShell);
    RUN(NoTakeWhileSpentShellsAreIn);
    RUN(TakeWhileShutDoesNothing);
    RUN(FullReloadOfTwo);
    RUN(EachShellIsItsOwnPush);
    RUN(OneLiveStaysThenOneShellIntoTheRight);
    RUN(OneSpareGoesLeftFirst);
    RUN(HitchInsideAPushLoadsOnlyItsShell);
    RUN(ShotsMarkRightFirst);
    RUN(FaultedAddEmptiesTheChamber);
    RUN(NotManualIsIdle);
    RUN(RedrawClearsTheHand);
    RUN(MuzzleUpRateSignAndSize);
    RUN(RideAngleFollowsTheHandDown);
    RUN(HingeFramesAgree);
    RUN(AnimWordAdvancesUnlessHeld);
    RUN(TablesAreShaped);
    std::printf("dbarrel_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
