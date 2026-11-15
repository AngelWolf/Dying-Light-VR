// Standalone unit test for src/src/ovr2xr/cock_logic.h (the revolvers' fan-cock) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 4. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\cock_logic_test.cpp
#include "cock_logic.h"

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
    CMachine m;
    CInput in{};
    COut o{};
    int needed = 0, cocks = 0, haptics = 0;
    explicit Rig(int rounds = 6)
    {
        in.manualOn = true;
        in.isRevolver = true;
        in.rounds = rounds;
        Step();
    }
    void Step()
    {
        o = m.Update(in);
        if (o.ev == CEvent::CockNeeded) { ++needed; }
        if (o.ev == CEvent::Cocked) { ++cocks; }
        if (o.haptic) { ++haptics; }
        in.reloadDone = false;
    }
    void Reload() { in.reloadDone = true; Step(); }
    // the palm enters the hammer zone, then travels back by `back` m in `n` steps, then leaves
    void Sweep(float back, int n = 5)
    {
        in.palmInZone = true; in.palmTravelM = 0.0f; Step();
        for (int k = 1; k <= n; ++k) { in.palmTravelM = back * static_cast<float>(k) / static_cast<float>(n); Step(); }
        in.palmInZone = false; in.palmTravelM = 0.0f; Step();
    }
};

// Jay: "I want the hammer to be cocked back manually" + "Once after reload, on all revolvers"
void ReloadRequiresOneCock()
{
    Rig r;
    CHECK(!r.o.withholdR2 && r.needed == 0);   // drawn loaded: no cock asked
    r.Reload();
    CHECK(r.needed == 1 && r.o.withholdR2 && !r.o.cocked);
}

// "Left hand, like if you were to fan fire": the palm sweeps BACK over the hammer
void SweepBackCocks()
{
    Rig r;
    r.Reload();
    r.Sweep(0.035f);
    CHECK(r.cocks == 1 && r.o.cocked && !r.o.withholdR2 && r.haptics >= 1 && r.o.fx == Fx::None);
}

void ShortSweepDoesNot()
{
    Rig r;
    r.Reload();
    r.Sweep(0.02f);
    CHECK(r.cocks == 0 && r.o.withholdR2);
}

// 2026-11-10: the travel is any direction (the driver passes a distance), but two short passes never add up - each counts from its entry
void ShortPassesDoNotAddUp()
{
    Rig r;
    r.Reload();
    r.Sweep(0.02f);
    r.Sweep(0.02f);
    CHECK(r.cocks == 0 && r.o.withholdR2);
}

// a sweep that starts OUTSIDE the zone and only ends inside it counts from the entry, not from outside
void TravelCountsFromTheEntry()
{
    Rig r;
    r.Reload();
    r.in.palmInZone = false; r.in.palmTravelM = 0.05f; r.Step();
    r.in.palmInZone = true; r.in.palmTravelM = 0.0f; r.Step();
    r.in.palmTravelM = 0.01f; r.Step();
    CHECK(r.cocks == 0);
}

// once per reload: after the cock, shots never ask again until the next reload
void ShotsAfterCockNeedNoMore()
{
    Rig r;
    r.Reload();
    r.Sweep(0.04f);
    for (int k = 5; k >= 0; --k) { r.in.rounds = k; r.Step(); CHECK(!r.o.withholdR2); }
    CHECK(r.needed == 1);
    r.Reload();
    CHECK(r.needed == 2 && r.o.withholdR2);
}

// Review 1: reloaded, not cocked, swapped away and back - still uncocked, still asked
void CockSurvivesRedraw()
{
    Rig r;
    r.Reload();
    r.in.freshDraw = true; r.Step(); r.in.freshDraw = false; r.Step();
    CHECK(r.o.withholdR2 && !r.o.cocked);
    r.Sweep(0.04f);
    CHECK(!r.o.withholdR2);
}

void NotManualIsIdle()
{
    Rig r;
    r.Reload();
    r.in.manualOn = false; r.Step();
    CHECK(!r.o.withholdR2 && r.o.ev == CEvent::None);
    r.in.manualOn = true; r.in.isRevolver = false; r.Step();
    CHECK(!r.o.withholdR2);
}

}  // namespace

int main()
{
    RUN(ReloadRequiresOneCock);
    RUN(SweepBackCocks);
    RUN(ShortSweepDoesNot);
    RUN(ShortPassesDoNotAddUp);
    RUN(TravelCountsFromTheEntry);
    RUN(ShotsAfterCockNeedNoMore);
    RUN(CockSurvivesRedraw);
    RUN(NotManualIsIdle);
    std::printf("cock_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
