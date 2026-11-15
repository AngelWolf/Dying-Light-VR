// Standalone unit test for src/src/ovr2xr/fuel_logic.h (the chainsaw gun's pour + cord) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 6. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\fuel_logic_test.cpp
#include "fuel_logic.h"

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
    FMachine m;
    FInput in{};
    FOut o{};
    int adds = 0, events[static_cast<int>(FEvent::Count)] = {};
    explicit Rig(int rounds, int spare = 300)
    {
        in.manualOn = true;
        in.isChainsaw = true;
        in.rounds = rounds;
        in.spare = spare;
        in.capacity = 100;
        Step(0.011f);
    }
    void Step(float dt)
    {
        in.dtS = dt;
        o = m.Update(in);
        if (o.callAddBullet) { ++adds; in.rounds = in.capacity; }
        for (int e = 0; e < static_cast<int>(FEvent::Count); ++e) {
            if (o.Has(static_cast<FEvent>(e))) { ++events[e]; }
        }
    }
    int N(FEvent e) const { return events[static_cast<int>(e)]; }
    void Take() { in.gripDown = true; in.inHip = true; Step(0.011f); in.inHip = false; Step(0.011f); }
    void Pour(float s, float tilt = 120.0f, bool atFiller = true)
    {
        in.tiltDeg = tilt; in.spoutAtFiller = atFiller;
        for (float t = 0.0f; t < s; t += 0.011f) { Step(0.011f); }
        in.tiltDeg = 0.0f; in.spoutAtFiller = false;
    }
    void Cord(float pullM)
    {
        in.gripDown = false; Step(0.011f);
        in.nearCord = true; in.gripDown = true; Step(0.011f); in.nearCord = false;
        for (int k = 1; k <= 10; ++k) { in.cordPullM = pullM * static_cast<float>(k) / 10.0f; Step(0.011f); }
        in.gripDown = false; in.cordPullM = 0.0f; Step(0.011f);
    }
};

// Jay: "Pour the fuel by hand" + "Left hip pouch"
void TakeAtHip()
{
    Rig r(0);
    r.Take();
    CHECK(r.N(FEvent::Take) == 1 && r.o.bottleInHand && r.o.withholdR2);
}

void FullGunRefusesTake()
{
    Rig r(100);
    r.Take();
    CHECK(r.N(FEvent::TakeNone) == 1 && !r.o.bottleInHand);
    Rig s(0, 0);
    s.Take();
    CHECK(s.N(FEvent::TakeNone) == 1 && !s.o.bottleInHand);
}

// Review 3: tipped away from the filler / not tipped enough - no fill
void PourAwayFromFillerDoesNothing()
{
    Rig r(0);
    r.Take();
    r.Pour(2.0f, 120.0f, false);
    CHECK(r.adds == 0 && r.N(FEvent::Filled) == 0);
}

void ShallowTiltDoesNothing()
{
    Rig r(0);
    r.Take();
    r.Pour(2.0f, 80.0f, true);
    CHECK(r.adds == 0);
}

// 1.0 s tipped >= 90 at the filler = the gun fills (ONE add = the game's whole fill)
void PourOneSecondFills()
{
    Rig r(0);
    r.Take();
    r.Pour(0.6f);
    CHECK(r.adds == 0 && r.o.pourS > 0.5f);
    r.Pour(0.5f);
    CHECK(r.adds == 1 && r.N(FEvent::Filled) == 1 && r.in.rounds == 100);
}

// Review 3: let go mid-pour - the pour time does not carry to the next take
void LetGoMidPourResets()
{
    Rig r(0);
    r.Take();
    r.Pour(0.8f);
    r.in.gripDown = false; r.Step(0.011f);
    CHECK(r.N(FEvent::BottleAway) == 1 && !r.o.bottleInHand && r.o.pourS == 0.0f);
    r.Take();
    r.Pour(0.5f);
    CHECK(r.adds == 0);
}

// "then pull the cord once": after the fill the gun waits for the cord
void CordNeededAfterFill()
{
    Rig r(0);
    r.Take();
    r.Pour(1.1f);
    CHECK(r.o.withholdR2 && !r.o.ready);
    r.Cord(0.3f);
    CHECK(r.N(FEvent::CordGrab) == 1 && r.N(FEvent::CordPulled) == 1 && r.o.ready && !r.o.withholdR2);
}

void ShortCordDoesNotStart()
{
    Rig r(0);
    r.Take();
    r.Pour(1.1f);
    r.Cord(0.15f);
    CHECK(r.N(FEvent::CordShort) == 1 && !r.o.ready && r.o.withholdR2);
    r.Cord(0.3f);
    CHECK(r.o.ready);
}

void DrawnLoadedIsReady()
{
    Rig r(60);
    CHECK(r.o.ready && !r.o.withholdR2);
    Rig e(0);
    CHECK(!e.o.ready && e.o.withholdR2);   // drawn empty: needs the pour + the cord
}

void NotManualIsIdle()
{
    Rig r(0);
    r.in.manualOn = false; r.Step(0.011f);
    CHECK(!r.o.withholdR2 && !r.o.bottleInHand);
}

}  // namespace

int main()
{
    RUN(TakeAtHip);
    RUN(FullGunRefusesTake);
    RUN(PourAwayFromFillerDoesNothing);
    RUN(ShallowTiltDoesNothing);
    RUN(PourOneSecondFills);
    RUN(LetGoMidPourResets);
    RUN(CordNeededAfterFill);
    RUN(ShortCordDoesNotStart);
    RUN(DrawnLoadedIsReady);
    RUN(NotManualIsIdle);
    std::printf("fuel_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
