// Standalone unit test for src/src/ovr2xr/gate_logic.h (the Sixshooter / Walker loading gate) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 5. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\gate_logic_test.cpp
#include "gate_logic.h"

#include <cmath>
#include <cstdio>

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

struct Rig {
    GMachine m;
    GInput in{};
    GOut o{};
    GParams p{};
    int adds = 0, idx = 0, events[static_cast<int>(GEvent::Count)] = {};
    explicit Rig(int rounds, int spare = 20)
    {
        in.manualOn = true;
        in.rounds = rounds;
        in.spare = spare;
        in.farLoad = true;
        in.onSide = true;   // RG3: the gun held on its side, the gate up (the flicks count only then)
        Step(0.0);
    }
    void Step(double dt)
    {
        in.nowS += dt;
        o = m.Update(in, p);
        if (o.callAddBullet) { ++adds; ++in.rounds; --in.spare; }
        for (int e = 0; e < static_cast<int>(GEvent::Count); ++e) {
            if (o.Has(static_cast<GEvent>(e))) { ++events[e]; }
        }
    }
    int N(GEvent e) const { return events[static_cast<int>(e)]; }
    // RG5: a twist = the window's peak for one update (the driver clears its window on o.toggled)
    void Right(float deg) { in.twistRightDeg = deg; Step(0.011); in.twistRightDeg = 0.0f; Step(0.011); }
    void Left(float deg) { in.twistLeftDeg = deg; Step(0.011); in.twistLeftDeg = 0.0f; Step(0.011); }
    void Take() { in.gripDown = true; in.inHip = true; Step(0.011); in.inHip = false; Step(0.011); }
    void Wait() { for (int i = 0; i < 41; ++i) { Step(0.011); } }   // past the 0.4 s lockout after a toggle (RG4)
    void PushOne()
    {
        in.nearLoad = true; in.farLoad = false;
        Step(0.011);
        in.nearLoad = false;
        for (int i = 0; i < 120 && m.loading(); ++i) { Step(0.011); }
        in.farLoad = true;
        Step(0.011);
    }
};

int Count(const GOut& o, DChamber c)
{
    int n = 0;
    for (int s = 0; s < 6; ++s) { n += o.ch[s] == c ? 1 : 0; }
    return n;
}

// Jay: "Flick open the gate too, has to be a flick to the right"; RUN RG5 -> "Controller twist": open = a twist RIGHT of 45 deg within
// 0.15 s, close = LEFT 60 deg (the controller's own twist, twist_logic.h)
void RightTwistOpensGate()
{
    Rig r(0);
    r.Right(46.0f);
    CHECK(r.N(GEvent::GateOpen) == 1 && r.o.gateOpen && r.o.withholdR2);
    Rig s(0);
    s.Right(29.0f);
    CHECK(s.N(GEvent::GateOpen) == 0 && !s.o.gateOpen);
    s.Left(120.0f);   // a LEFT twist never opens it
    CHECK(!s.o.gateOpen);
    // RG6: close lowered 60 -> 45 (Jay's left twists read 47-55; the rebound guard is the 0.4 s lockout - "You can fix the gate now")
    // RG9 (Jay: "I'm saying it's difficult to get OPEN AND CLOSED"; his twists measured 30-41 deg): both at 30
    CHECK(std::fabs(kGateOpenTwistDeg - 30.0f) < 1e-6f && std::fabs(kGateCloseTwistDeg - 30.0f) < 1e-6f && kGateTwistWindowS == 0.15);
}

// the toggle is reported so the driver clears its twist window (one twist never counts twice)
void ToggleIsReported()
{
    Rig r(0);
    r.in.twistRightDeg = 50.0f;
    r.Step(0.011);
    CHECK(r.o.toggled && r.o.gateOpen);
    r.in.twistRightDeg = 0.0f;
    r.Step(0.011);
    CHECK(!r.o.toggled);
}

// RUN RG3, Jay: "way too easy to open/close accidentally (Sideways controller rotation, remember)" - only with the gun on its side
void OpenNeedsTheGunOnItsSide()
{
    Rig r(0);
    r.in.onSide = false;
    r.Right(120.0f);
    CHECK(!r.o.gateOpen && r.N(GEvent::GateOpen) == 0);
    r.in.onSide = true;
    r.Right(46.0f);
    CHECK(r.o.gateOpen);
}

// RG6: Jay's real left twists were 47-55 deg - they close now
void LeftTwistOfFiftyCloses()
{
    Rig r(0);
    r.Right(46.0f);
    r.Wait();
    r.Left(50.0f);
    CHECK(!r.o.gateOpen && r.N(GEvent::GateClose) == 1);
}

void GateCloseNeedsFortyFive()
{
    Rig r(0);
    r.Right(46.0f);
    r.Wait();
    r.Left(29.0f);
    CHECK(r.o.gateOpen);
    r.Left(31.0f);
    CHECK(!r.o.gateOpen && r.N(GEvent::GateClose) == 1 && !r.o.withholdR2);
}

// RUN RG4: a hard swing's REBOUND 30-130 ms later toggled it back. After a toggle nothing counts for kGateLockoutS (0.4 s).
void ReboundAfterOpenDoesNotClose()
{
    Rig r(0);
    r.Right(46.0f);
    CHECK(r.o.gateOpen);
    r.Left(150.0f);   // the rebound, at once
    r.Left(150.0f);
    CHECK(r.o.gateOpen && r.N(GEvent::GateClose) == 0);
    r.Wait();
    r.Left(61.0f);
    CHECK(!r.o.gateOpen && r.N(GEvent::GateClose) == 1);
    CHECK(std::fabs(kGateLockoutS - 0.4f) < 1e-6f);
}

void ReboundAfterCloseDoesNotOpen()
{
    Rig r(0);
    r.Right(46.0f);
    r.Wait();
    r.Left(61.0f);
    CHECK(!r.o.gateOpen);
    r.Right(150.0f);   // the rebound
    CHECK(!r.o.gateOpen && r.N(GEvent::GateOpen) == 1);
}

// closing too: only on its side (righting the gun quickly never closes it)
void CloseNeedsTheGunOnItsSide()
{
    Rig r(0);
    r.Right(46.0f);
    r.Wait();
    r.in.onSide = false;
    r.Left(150.0f);
    CHECK(r.o.gateOpen);
    r.in.onSide = true;
    r.Left(61.0f);
    CHECK(!r.o.gateOpen);
}

// each spent case pops out by itself as its chamber comes to the gate
void SpentCaseOutAtGate()
{
    Rig r(0);
    r.Right(46.0f);
    CHECK(r.N(GEvent::CaseOut) == 1 && r.o.ch[r.o.cylinderIndex] == DChamber::Empty);
}

void PushOnlyWhileOpen()
{
    Rig r(0);
    r.in.gripDown = true; r.in.inHip = true; r.Step(0.011); r.in.inHip = false; r.Step(0.011);
    CHECK(r.m.hand() == 0);                // no take with the gate shut
    r.PushOne();
    CHECK(r.adds == 0);
}

// one push per round, the cylinder clicks one chamber on after each (Index), the next spent case popping at the gate
void EachRoundIndexesOnce()
{
    Rig r(0);
    r.Right(46.0f);
    r.Take();
    const int i0 = r.o.cylinderIndex;
    r.PushOne();
    CHECK(r.adds == 1 && r.N(GEvent::Index) == 1 && r.o.cylinderIndex == (i0 + 1) % 6);
    CHECK(r.N(GEvent::CaseOut) == 2);       // the next chamber's spent case popped as it came to the gate
}

void SixRoundsFill()
{
    Rig r(0);
    r.Right(46.0f);
    r.Take();
    CHECK(r.m.hand() == 6);
    for (int k = 0; k < 6; ++k) { r.PushOne(); }
    CHECK(r.adds == 6 && r.in.rounds == 6 && Count(r.o, DChamber::Live) == 6 && r.m.hand() == 0);
    r.in.gripDown = false; r.Step(0.011);
    r.Left(61.0f);
    CHECK(r.N(GEvent::ReloadDone) == 1);
}

// a top-up: 4 live, 2 spent -> the take holds 2, both go in, the cylinder skips the live ones
void TopUpTwoOfSix()
{
    Rig r(4);
    r.Right(46.0f);
    CHECK(r.o.ch[r.o.cylinderIndex] == DChamber::Empty);
    r.Take();
    CHECK(r.m.hand() == 2);
    r.PushOne();
    r.PushOne();
    CHECK(r.adds == 2 && Count(r.o, DChamber::Live) == 6);
}

// open and shut with nothing loaded: no reload, no cock (N4: a full gun cannot open, so 5 rounds + a spare)
void OpenCloseWithoutLoadingIsNoReload()
{
    Rig r(5);
    r.Right(46.0f);
    r.Wait();
    r.Left(61.0f);
    CHECK(r.N(GEvent::GateClose) == 1 && r.N(GEvent::ReloadDone) == 0);
}

// Review N4 (2026-11-10; Jay: "we'll go with 1"): the gate opens only when the gun can load = it is not full (rounds < 6). The close is
// unchanged (a gun filled with the gate open still closes). RUN SK1: the spare half refused 52 twists (the driver's spare is -1 until a
// hip take) -> Jay: "Drop the spare check" - the spare count never decides the open.
void FullGunNeverOpens()
{
    Rig full(6);
    full.Right(120.0f);
    full.Wait();
    full.Right(46.0f);
    CHECK(!full.o.gateOpen && full.N(GEvent::GateOpen) == 0 && full.N(GEvent::CaseOut) == 0 && !full.o.toggled);
    Rig unknown(0, -1);   // SK1's case: an empty gun, the spare not read yet (-1) - it OPENS
    unknown.Right(46.0f);
    CHECK(unknown.o.gateOpen && unknown.N(GEvent::GateOpen) == 1);
    Rig none(0, 0);       // no spare: it opens too (nothing to take - the take says TakeNoRound)
    none.Right(46.0f);
    CHECK(none.o.gateOpen && none.N(GEvent::GateOpen) == 1);
    Rig last(5, 1);       // one empty chamber
    last.Right(46.0f);
    CHECK(last.o.gateOpen && last.N(GEvent::GateOpen) == 1);
    CHECK(GateCanLoad(5) && GateCanLoad(0) && !GateCanLoad(6));
    // filled to six with the gate open: it still closes, then will not open again
    Rig fill(0);
    fill.Right(46.0f);
    fill.Take();
    for (int k = 0; k < 6; ++k) { fill.PushOne(); }
    fill.in.gripDown = false; fill.Step(0.011);
    fill.Left(61.0f);
    CHECK(!fill.o.gateOpen && fill.N(GEvent::GateClose) == 1 && fill.N(GEvent::ReloadDone) == 1);
    fill.Wait();
    fill.Right(120.0f);
    CHECK(!fill.o.gateOpen && fill.N(GEvent::GateOpen) == 1);
}

void ShotsSpendAndRedrawClears()
{
    Rig r(6);
    r.in.rounds = 5; r.Step(0.011);
    CHECK(r.N(GEvent::Shot) == 1 && Count(r.o, DChamber::Spent) == 1);
    r.Right(46.0f);
    r.Take();
    r.in.freshDraw = true; r.Step(0.011); r.in.freshDraw = false;
    CHECK(r.m.hand() == 0 && !r.o.gateOpen && r.N(GEvent::HandCleared) == 1);
}

}  // namespace

int main()
{
    RUN(RightTwistOpensGate);
    RUN(ToggleIsReported);
    RUN(OpenNeedsTheGunOnItsSide);
    RUN(LeftTwistOfFiftyCloses);
    RUN(GateCloseNeedsFortyFive);
    RUN(CloseNeedsTheGunOnItsSide);
    RUN(ReboundAfterOpenDoesNotClose);
    RUN(ReboundAfterCloseDoesNotOpen);
    RUN(SpentCaseOutAtGate);
    RUN(PushOnlyWhileOpen);
    RUN(EachRoundIndexesOnce);
    RUN(SixRoundsFill);
    RUN(TopUpTwoOfSix);
    RUN(OpenCloseWithoutLoadingIsNoReload);
    RUN(FullGunNeverOpens);
    RUN(ShotsSpendAndRedrawClears);
    std::printf("gate_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
