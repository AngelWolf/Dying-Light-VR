// Standalone unit test for src/src/ovr2xr/break6_logic.h (the Schofield's six-chamber break-top) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 3. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\break6_logic_test.cpp
#include "break6_logic.h"

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
    SMachine m;
    SInput in{};
    SOut o{};
    SParams p{};
    int adds = 0, haptics = 0, events[static_cast<int>(SEvent::Count)] = {};
    explicit Rig(int rounds, int spare = 20)
    {
        in.manualOn = true;
        in.rounds = rounds;
        in.spare = spare;
        in.farLoad = true;
        Step(0.0);
    }
    void Step(double dt)
    {
        in.nowS += dt;
        o = m.Update(in, p);
        if (o.callAddBullet) { ++adds; ++in.rounds; --in.spare; }
        if (o.haptic) { ++haptics; }
        for (int e = 0; e < static_cast<int>(SEvent::Count); ++e) {
            if (o.Has(static_cast<SEvent>(e))) { ++events[e]; }
        }
    }
    int N(SEvent e) const { return events[static_cast<int>(e)]; }
    void FlickDown(float dps)
    {
        in.flickDownDps = dps; Step(0.011); Step(0.011);
        in.flickDownDps = 0.0f;
        for (int i = 0; i < 60 && m.phase() == SPhase::Opening; ++i) { Step(0.011); }
    }
    void FlickUp(float dps)
    {
        in.flickUpDps = dps; Step(0.011); Step(0.011);
        in.flickUpDps = 0.0f;
        for (int i = 0; i < 60 && m.phase() == SPhase::Closing; ++i) { Step(0.011); }
    }
    void Take() { in.gripDown = true; in.inHip = true; Step(0.011); in.inHip = false; Step(0.011); }
    void PushOne()
    {
        in.nearLoad = true; in.farLoad = false;
        Step(0.011);
        in.nearLoad = false;
        for (int i = 0; i < 120 && m.phase() == SPhase::Loading; ++i) { Step(0.011); }
        in.farLoad = true;
        Step(0.011);
    }
};

int Count(const SOut& o, DChamber c)
{
    int n = 0;
    for (int s = 0; s < 6; ++s) { n += o.ch[s] == c ? 1 : 0; }
    return n;
}

void DrawFillsLiveFromRounds()
{
    Rig r(4);
    CHECK(Count(r.o, DChamber::Live) == 4 && Count(r.o, DChamber::Spent) == 2 && r.o.phase == SPhase::Closed && !r.o.withholdR2);
}

// Jay: "Flick open for the revolvers", "Downward flick" - the muzzle snapped DOWN at the flick threshold for 2 updates opens it to 80
void SchofieldFlickDownOpens80()
{
    Rig r(0);
    r.FlickDown(450.0f);
    CHECK(r.N(SEvent::FlickOpen) == 1 && r.N(SEvent::Opened) == 1);
    CHECK(r.o.phase == SPhase::Open && std::fabs(r.o.angle - 80.0f) < 1e-4f && r.o.withholdR2);
}

void SchofieldSlowTurnDoesNotOpen()
{
    Rig r(0);
    r.FlickDown(350.0f);
    CHECK(r.N(SEvent::FlickOpen) == 0 && r.o.phase == SPhase::Closed);
    r.in.flickUpDps = 900.0f; r.Step(0.011); r.Step(0.011);   // an UP flick never opens it
    CHECK(r.o.phase == SPhase::Closed);
}

// Jay: "all 6 spent cases drop out on opening" - the spent ones go, a live one stays
void SchofieldSpentDropOnOpen()
{
    Rig r(2);
    r.FlickDown(450.0f);
    CHECK(r.N(SEvent::CasesOut) == 1);
    CHECK(Count(r.o, DChamber::Spent) == 0 && Count(r.o, DChamber::Live) == 2 && Count(r.o, DChamber::Empty) == 4);
}

// Jay: "all 6 bullets in one hand" - a take holds min(empty chambers, spare)
void SchofieldTakesMinSix()
{
    Rig a(0, 20);
    a.FlickDown(450.0f);
    a.Take();
    CHECK(a.N(SEvent::Take) == 1 && a.m.hand() == 6);
    Rig b(0, 4);
    b.FlickDown(450.0f);
    b.Take();
    CHECK(b.m.hand() == 4);
    Rig c(5, 20);
    c.FlickDown(450.0f);
    c.Take();
    CHECK(c.m.hand() == 1);
    Rig d(0, 0);
    d.FlickDown(450.0f);
    d.Take();
    CHECK(d.N(SEvent::TakeNoRound) == 1 && d.m.hand() == 0);
}

// "feed them in one at a time" - one push per entry, each into the next EMPTY chamber, one add each
void SchofieldOnePushPerChamber()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.Take();
    for (int k = 0; k < 6; ++k) { r.PushOne(); }
    CHECK(r.adds == 6 && r.in.rounds == 6 && r.m.hand() == 0 && Count(r.o, DChamber::Live) == 6 && r.N(SEvent::RoundIn) == 6);
    r.PushOne();   // nothing in the hand: nothing happens
    CHECK(r.adds == 6 && r.N(SEvent::LoadStart) == 6);
    // a push held IN the breech does not repeat - it needs the hand out (farLoad) first
    Rig s(0);
    s.FlickDown(450.0f);
    s.Take();
    s.in.nearLoad = true; s.in.farLoad = false;
    for (int i = 0; i < 100; ++i) { s.Step(0.011); }
    CHECK(s.adds == 1);
}

// the close: the upward flick swings it shut (the single-shot's flick); closing after loading = the reload is done (the cock is next)
void FlickUpClosesAndReloadDone()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.Take();
    r.PushOne();
    r.in.gripDown = false; r.Step(0.011);
    r.FlickUp(450.0f);
    CHECK(r.N(SEvent::FlickClose) == 1 && r.N(SEvent::Closed) == 1 && r.o.phase == SPhase::Closed && !r.o.withholdR2);
    CHECK(r.N(SEvent::ReloadDone) == 1);
}

// a half-loaded Schofield closes, counts what it holds, and still needs the cock (Review 4)
void PartialLoadClosesAndCounts()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.Take();
    for (int k = 0; k < 3; ++k) { r.PushOne(); }
    r.in.gripDown = false; r.Step(0.011);
    CHECK(r.N(SEvent::HandCleared) == 1 && r.m.hand() == 0);   // the 3 left in the hand go back (never consumed)
    r.FlickUp(450.0f);
    CHECK(r.in.rounds == 3 && Count(r.o, DChamber::Live) == 3 && Count(r.o, DChamber::Empty) == 3 && r.N(SEvent::ReloadDone) == 1);
}

// open and closed again with nothing loaded: no reload, no cock asked
void OpenCloseWithoutLoadingIsNoReload()
{
    Rig r(6);
    r.FlickDown(450.0f);
    r.FlickUp(450.0f);
    CHECK(r.N(SEvent::Closed) == 1 && r.N(SEvent::ReloadDone) == 0);
}

// "close by hand": a grip on top rides the barrel shut
void HandRidesItShut()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.in.gripDown = true; r.in.nearTop = true; r.Step(0.011);
    CHECK(r.o.phase == SPhase::Riding);
    r.in.nearTop = false; r.in.rideValid = true;
    for (float a : { 60.0f, 20.0f, 0.0f }) { r.in.rideAngle = a; r.Step(0.011); }
    CHECK(r.N(SEvent::Closed) == 1 && r.o.phase == SPhase::Closed);
}

// the push's frame for the driver: held at 1 through the lead-in, then 60 fps, clamped at the last frame; the chamber it plays at
void LoadFrameAdvances()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.Take();
    CHECK(r.o.loadChamber == 0);
    r.in.nearLoad = true; r.in.farLoad = false; r.Step(0.011);
    CHECK(r.o.phase == SPhase::Loading && r.o.loadChamber == 0 && r.o.loadFrame == 1.0f && r.o.locked);
    r.Step(0.111);   // the 0.10 s lead-in over, ~0.011 s into the clip
    CHECK(r.o.loadFrame > 1.0f && r.o.loadFrame < 2.0f);
    r.Step(0.05);
    CHECK(r.o.loadFrame > 3.5f);
}

// the Schofield's push tables (reload_break_sch.inc): in the barrel's CLOSED frame the seat frame's round IS chamber 0's rest round; the
// hinge geometry opens 80 about +x; chamber j's mouth = chamber 0's + its delta
void SchTablesAreShaped()
{
    float d = 0.0f;
    for (int i = 0; i < 12; ++i) { d = std::fmax(d, std::fabs(kSchLoad[kSchLoadSeatFrame - 1].shellM[0][i] - kSchChamberRound[0][i])); }
    CHECK(d < 1e-4f);
    CHECK(std::fabs(kSchOpenDeg - 80.0f) < 0.01f && std::fabs(kDbGeomSch.axis[0] - 1.0f) < 1e-4f);
    for (int j = 0; j < 6; ++j) {
        CHECK(std::fabs(kSchChamberRound[0][3] + kSchChamberDelta[j][0] - kSchChamberRound[j][3]) < 1e-6f);
        CHECK(std::fabs(kSchChamberRound[0][7] + kSchChamberDelta[j][1] - kSchChamberRound[j][7]) < 1e-6f);
    }
    CHECK(static_cast<int>(sizeof(kSchLoad) / sizeof(kSchLoad[0])) == SParams{}.loadFrames && kSchLoadSeatFrame == SParams{}.seatFrame);
}

void ShotsSpendLiveChambers()
{
    Rig r(6);
    r.in.rounds = 4; r.Step(0.011);
    CHECK(r.N(SEvent::Shot) == 1 && Count(r.o, DChamber::Live) == 4 && Count(r.o, DChamber::Spent) == 2);
}

void RedrawClearsTheHandAndNotManualIsIdle()
{
    Rig r(0);
    r.FlickDown(450.0f);
    r.Take();
    r.in.freshDraw = true; r.Step(0.011); r.in.freshDraw = false;
    CHECK(r.m.hand() == 0 && r.o.phase == SPhase::Closed && r.N(SEvent::HandCleared) == 1);
    r.in.manualOn = false; r.Step(0.011);
    CHECK(r.o.phase == SPhase::Closed && !r.o.withholdR2 && r.o.events == 0);
}

}  // namespace

int main()
{
    RUN(DrawFillsLiveFromRounds);
    RUN(SchofieldFlickDownOpens80);
    RUN(SchofieldSlowTurnDoesNotOpen);
    RUN(SchofieldSpentDropOnOpen);
    RUN(SchofieldTakesMinSix);
    RUN(SchofieldOnePushPerChamber);
    RUN(FlickUpClosesAndReloadDone);
    RUN(PartialLoadClosesAndCounts);
    RUN(OpenCloseWithoutLoadingIsNoReload);
    RUN(HandRidesItShut);
    RUN(LoadFrameAdvances);
    RUN(SchTablesAreShaped);
    RUN(ShotsSpendLiveChambers);
    RUN(RedrawClearsTheHandAndNotManualIsIdle);
    std::printf("break6_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
