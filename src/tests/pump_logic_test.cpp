// Standalone unit test for src/src/ovr2xr/pump_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 1. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\pump_logic_test.cpp
#include "pump_logic.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dlvr::reload;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
// Runs one test and prints its name with PASS / FAIL (the failures of that test only).
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

const PParams kP{};   // capacity 6, handMax 4, rackDoneFrac 0.815, cycle 36, add 23

PInput Base(int rounds, int spare)
{
    PInput i{};
    i.manualOn = true;
    i.isPump = true;
    i.rounds = rounds;
    i.spare = spare;
    return i;
}

struct Driver {
    PMachine m;
    PParams p = kP;   // 2026-11-07: a test may set the row's racks / capacity (the semi-auto: false / 8)
    std::vector<PEvent> events;
    int haptics = 0;
    int addBullets = 0;
    POut last;
    POut Step(const PInput& in)
    {
        last = m.Update(in, p);
        if (last.ev != PEvent::None) { events.push_back(last.ev); }
        if (last.haptic) { ++haptics; }
        if (last.callAddBullet) { ++addBullets; }
        return last;
    }
    int Count(PEvent e) const
    {
        int n = 0;
        for (PEvent x : events) { if (x == e) { ++n; } }
        return n;
    }
};

// Draws the pump (freshDraw) with the given counts.
PInput Draw(Driver& d, int rounds, int spare)
{
    PInput i = Base(rounds, spare);
    i.freshDraw = true;
    d.Step(i);
    i.freshDraw = false;
    return i;
}

// From a drawn pump: grip + hip together -> Take. Returns the input with the grip still down, hip off.
PInput TakeHandful(Driver& d, PInput i)
{
    i.gripDown = true; i.inHip = true;
    d.Step(i);
    i.inHip = false;
    return i;
}

void TakeGivesUpToFour()
{
    {
        Driver d;
        PInput i = Draw(d, 0, 10);
        POut o;
        i.gripDown = true; i.inHip = true;
        o = d.Step(i);
        CHECK(o.ev == PEvent::Take && o.handShells == 4 && o.haptic && o.nFx == 1 && o.fx[0] == Fx::PouchTake);
    }
    {
        Driver d;
        PInput i = Draw(d, 0, 2);
        i.gripDown = true; i.inHip = true;
        POut o = d.Step(i);
        CHECK(o.ev == PEvent::Take && o.handShells == 2);
    }
    {
        Driver d;
        PInput i = Draw(d, 4, 10);
        i.gripDown = true; i.inHip = true;
        POut o = d.Step(i);
        CHECK(o.ev == PEvent::Take && o.handShells == 2);
        CHECK(d.m.handShells() == 2);
    }
}

void TakeNoShellRefused()
{
    Driver d;
    PInput i = Draw(d, 0, 0);   // no spare
    i.gripDown = true; i.inHip = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::TakeNoShell && o.handShells == 0 && !o.haptic && o.nFx == 0);
    for (int k = 0; k < 5; ++k) { d.Step(i); }
    CHECK(d.Count(PEvent::TakeNoShell) == 1);   // one per press
    Driver e;
    PInput j = Draw(e, 6, 5);   // full gun
    j.gripDown = true; j.inHip = true;
    o = e.Step(j);
    CHECK(o.ev == PEvent::TakeNoShell && o.handShells == 0 && !o.haptic);
}

void TakeOnLateHipFlag()
{
    // Ruling R1: the pouch flag rises AFTER the press (INV-673) - the take is the rising edge of (grip AND inHip).
    Driver d;
    PInput i = Draw(d, 0, 10);
    i.gripDown = true; i.inHip = false;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::None && o.handShells == 0);
    i.inHip = true;
    o = d.Step(i);
    CHECK(o.ev == PEvent::Take && o.handShells == 4);
    for (int k = 0; k < 5; ++k) { d.Step(i); }
    CHECK(d.Count(PEvent::Take) == 1);
    CHECK(d.m.handShells() == 4);
}

void PushPlaysOneCycleAndAddsOnce()
{
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 4);
    i.frameNow = 100; i.inPort = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush && o.animCycle == 1 && o.animFrame == 1 && !o.callAddBullet && o.handShells == 4);
    i.inPort = false;
    for (int f = 101; f <= 121; ++f) {
        i.frameNow = f;
        o = d.Step(i);
        CHECK(o.ev == PEvent::None && !o.callAddBullet && o.handShells == 4 && o.animCycle == 1 && o.animFrame == f - 99);
    }
    i.frameNow = 122;   // animFrame 23
    o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.haptic && o.HasFx(Fx::Seat) && o.handShells == 3 && o.animFrame == 23);
    for (int f = 123; f <= 135; ++f) {
        i.frameNow = f;
        o = d.Step(i);
        CHECK(!o.callAddBullet && o.handShells == 3 && o.animCycle == 1 && o.animFrame == f - 99);
    }
    CHECK(o.animFrame == 36);
    i.frameNow = 136;   // animFrame 37: the cycle is over
    o = d.Step(i);
    CHECK(o.animCycle == 0 && o.animFrame == 0 && o.handShells == 3);
    CHECK(d.addBullets == 1 && d.Count(PEvent::ShellIn) == 1);
    // a frame counter that jumps past 23 still fires exactly once
    Driver e;
    PInput j = Draw(e, 0, 10);
    j = TakeHandful(e, j);
    j.frameNow = 10; j.inPort = true;
    e.Step(j);
    j.inPort = false;
    j.frameNow = 60;   // animFrame 51: past the whole cycle in one jump
    o = e.Step(j);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.handShells == 3 && o.animCycle == 0);
    j.frameNow = 41;
    e.Step(j);
    j.frameNow = 80;
    e.Step(j);
    CHECK(e.addBullets == 1);
}

void PushNeedsExitBeforeNext()
{
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    // the hand lingers in the port through the whole cycle and after it: no second push
    for (int f = 101; f <= 150; ++f) { i.frameNow = f; d.Step(i); }
    CHECK(d.Count(PEvent::PortPush) == 1 && d.Count(PEvent::ShellIn) == 1);
    // an update with outOfPort re-arms; the next entry pushes cycle 2
    i.inPort = false; i.outOfPort = true; i.frameNow = 151;
    d.Step(i);
    i.outOfPort = false; i.inPort = true; i.frameNow = 160;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush && o.animCycle == 2 && o.animFrame == 1);
    // leaving the port only a little (neither in nor out) does not re-arm
    Driver e;
    PInput j = Draw(e, 0, 10);
    j = TakeHandful(e, j);
    j.frameNow = 100; j.inPort = true;
    e.Step(j);
    j.inPort = false;
    j.frameNow = 150;
    e.Step(j);
    j.inPort = true; j.frameNow = 151;
    e.Step(j);
    CHECK(e.Count(PEvent::PortPush) == 1);
}

void FullGunRefusesPush()
{
    Driver d;
    PInput i = Draw(d, 2, 10);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 4);
    i.rounds = 6;   // the gun filled with shells still in the hand
    i.frameNow = 100; i.inPort = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortFull && o.animCycle == 0 && o.handShells == 4 && !o.callAddBullet);
    for (int f = 101; f < 140; ++f) { i.frameNow = f; o = d.Step(i); CHECK(!o.callAddBullet && o.animCycle == 0); }
    CHECK(d.addBullets == 0 && d.Count(PEvent::PortPush) == 0);
    CHECK(d.m.handShells() == 4);   // kept until the grip opens
    i.gripDown = false;
    o = d.Step(i);
    CHECK(o.ev == PEvent::HandCleared && o.handShells == 0);
}

void GripUpClearsHand()
{
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 4);
    i.gripDown = false;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::HandCleared && o.handShells == 0 && !o.haptic);
    // no port push with an empty hand
    i.inPort = true; i.frameNow = 50;
    o = d.Step(i);
    CHECK(o.ev == PEvent::None && o.animCycle == 0);
    // grip up mid-cycle: the cycle runs on, the add-bullet still fires, the rest of the hand clears after the cycle
    Driver e;
    PInput j = Draw(e, 0, 10);
    j = TakeHandful(e, j);
    j.frameNow = 100; j.inPort = true;
    e.Step(j);
    j.inPort = false; j.gripDown = false;
    j.frameNow = 105;
    o = e.Step(j);
    CHECK(o.ev == PEvent::None && o.handShells == 4 && o.animCycle == 1);
    j.frameNow = 122;
    o = e.Step(j);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.handShells == 3);
    j.frameNow = 136;
    o = e.Step(j);
    CHECK(o.ev == PEvent::HandCleared && o.handShells == 0 && o.animCycle == 0);
    CHECK(e.addBullets == 1);
}

void SwitchMidCycleCancelsCleanly()
{
    // Review 1
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    i.inPort = false;
    i.frameNow = 110;
    d.Step(i);
    i.freshDraw = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::HandCleared && o.handShells == 0 && o.animCycle == 0 && !o.callAddBullet);
    i.freshDraw = false;
    for (int f = 111; f <= 160; ++f) { i.frameNow = f; o = d.Step(i); CHECK(o.ev != PEvent::ShellIn && !o.callAddBullet && o.animCycle == 0); }
    CHECK(d.Count(PEvent::ShellIn) == 0 && d.addBullets == 0 && d.m.handShells() == 0);
}

void ShotUnchambers()
{
    Driver d;
    PInput i = Draw(d, 3, 5);
    CHECK(d.last.chambered && !d.last.withholdR2);
    i.rounds = 2;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::Shot && !o.chambered && o.withholdR2 && !d.m.chambered());
    d.Step(i);
    CHECK(d.Count(PEvent::Shot) == 1);
}

void FullStrokeChambers()
{
    Driver d;
    PInput i = Draw(d, 0, 5);
    CHECK(!d.last.chambered && d.last.withholdR2);   // empty at the draw
    i.rounds = 1;                                    // a shell arrived (add-bullet)
    d.Step(i);
    CHECK(d.last.withholdR2);
    i.grippedPump = true; i.strokeFrac = 0.3f;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::None && o.pumpFollowsHand);
    i.strokeFrac = 0.9f;   // back past 0.815
    o = d.Step(i);
    CHECK(o.ev == PEvent::RackBack && o.haptic && o.HasFx(Fx::RackGrab) && o.withholdR2);
    i.strokeFrac = 0.95f;
    o = d.Step(i);
    CHECK(o.ev == PEvent::None);   // RackBack once per stroke
    i.strokeFrac = 0.5f;
    o = d.Step(i);
    CHECK(o.ev == PEvent::None && o.withholdR2);
    i.strokeFrac = 0.1f;   // forward below 0.185
    o = d.Step(i);
    CHECK(o.ev == PEvent::Chambered && o.haptic && o.HasFx(Fx::RackDone) && o.chambered && !o.withholdR2);
    i.grippedPump = false; i.strokeFrac = 0;
    o = d.Step(i);
    CHECK(o.ev == PEvent::None && !o.withholdR2 && !o.pumpFollowsHand);
    CHECK(d.Count(PEvent::RackShort) == 0);
}

void HalfStrokeDoesNotChamber()
{
    // Review 3
    Driver d;
    PInput i = Draw(d, 2, 5);
    i.rounds = 2;
    // unchambered gun: draw it empty, then a shell
    Driver e;
    PInput j = Draw(e, 0, 5);
    j.rounds = 2;
    j.grippedPump = true; j.strokeFrac = 0.6f;
    e.Step(j);
    j.strokeFrac = 0.1f;
    e.Step(j);
    j.grippedPump = false; j.strokeFrac = 0;
    POut o = e.Step(j);
    CHECK(o.ev == PEvent::RackShort);
    CHECK(!o.chambered && o.withholdR2);
    CHECK(e.Count(PEvent::RackBack) == 0 && e.Count(PEvent::Chambered) == 0);
    // a grab with no travel at all stays silent
    Driver f;
    PInput k = Draw(f, 0, 5);
    k.grippedPump = true;
    f.Step(k);
    k.grippedPump = false;
    f.Step(k);
    CHECK(f.Count(PEvent::RackShort) == 0);
}

void RackEmptyTubeStaysUnchambered()
{
    // Review 4
    Driver d;
    PInput i = Draw(d, 0, 5);
    i.grippedPump = true; i.strokeFrac = 0.95f;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::RackBack);
    i.strokeFrac = 0.05f;
    o = d.Step(i);
    CHECK(o.ev == PEvent::RackForward && o.haptic && o.HasFx(Fx::RackDone));
    CHECK(!o.chambered && o.withholdR2);
    i.grippedPump = false; i.strokeFrac = 0;
    o = d.Step(i);
    CHECK(!o.chambered && o.withholdR2 && d.Count(PEvent::Chambered) == 0);
}

void RackChamberedLosesNothing()
{
    Driver d;
    PInput i = Draw(d, 3, 5);
    CHECK(d.last.chambered);
    i.grippedPump = true; i.strokeFrac = 0.95f;
    d.Step(i);
    CHECK(d.last.chambered);
    i.strokeFrac = 0.05f;
    POut o = d.Step(i);
    CHECK(o.chambered && !o.withholdR2 && o.ev == PEvent::Chambered);
    CHECK(d.Count(PEvent::Shot) == 0);
    // pumping again on the same hold counts afresh
    i.strokeFrac = 0.95f;
    o = d.Step(i);
    CHECK(o.ev == PEvent::RackBack && o.chambered);
    i.strokeFrac = 0.05f;
    o = d.Step(i);
    CHECK(o.ev == PEvent::Chambered && o.chambered);
}

void ShotDuringCycleUnchambers()
{
    // Review 5
    Driver d;
    PInput i = Draw(d, 3, 10);
    CHECK(d.last.chambered);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 3);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    i.inPort = false;
    i.frameNow = 110;
    d.Step(i);
    CHECK(d.last.animCycle == 2 && !d.last.withholdR2);   // a handful of 3 starts at cycle 2 (P4); a chambered pump fires while it plays
    i.rounds = 2;   // the shot is the game's
    i.frameNow = 111;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::Shot && !o.chambered && o.withholdR2 && o.animCycle == 2 && o.handShells == 3);
    i.frameNow = 122;
    o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.handShells == 2 && !o.chambered);
    CHECK(d.Count(PEvent::Shot) == 1 && d.addBullets == 1);
}

// Review I1: several things in ONE update keep every pulse / sound / event.
void ShotAndShellInSameUpdateKeepsBoth()
{
    Driver d;
    PInput i = Draw(d, 3, 10);
    i = TakeHandful(d, i);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    i.inPort = false;
    i.frameNow = 121;
    d.Step(i);
    i.rounds = 2;       // the shot lands on the very update of the add-bullet frame (animFrame 23)
    i.frameNow = 122;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::Shot);   // the first raised, for logging
    CHECK(o.Has(PEvent::Shot) && o.Has(PEvent::ShellIn));
    CHECK(o.haptic && o.HasFx(Fx::Seat) && o.callAddBullet);
    CHECK(!o.chambered && o.handShells == 2);
}

void TakeAndShotSameUpdateKeepsTakePulse()
{
    Driver d;
    PInput i = Draw(d, 3, 10);
    CHECK(d.last.chambered);
    i.rounds = 2;
    i.gripDown = true; i.inHip = true;
    POut o = d.Step(i);
    CHECK(o.Has(PEvent::Shot) && o.Has(PEvent::Take));
    CHECK(o.haptic && o.HasFx(Fx::PouchTake) && o.handShells == 4 && !o.chambered);
}

void ForwardEndWithShotKeepsRackDone()
{
    Driver d;
    PInput i = Draw(d, 3, 5);
    i.grippedPump = true; i.strokeFrac = 0.95f;
    d.Step(i);
    i.strokeFrac = 0.05f;
    i.rounds = 2;       // fired by the right hand on the update the stroke ends
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::Shot);
    CHECK(o.Has(PEvent::Shot) && o.Has(PEvent::Chambered));
    CHECK(o.haptic && o.HasFx(Fx::RackDone) && o.chambered && !o.withholdR2);
}

// Ruling R7 / review M2: our own add-bullet (+1) and a shot (-1) between two updates net to 0 - the shot is still seen.
void ShotHiddenByOwnAddIsSeen()
{
    Driver d;
    PInput i = Draw(d, 3, 10);
    i = TakeHandful(d, i);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    i.inPort = false;
    i.frameNow = 122;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.chambered);
    i.frameNow = 123;   // add landed (+1) and a shot happened (-1): rounds back at 3
    o = d.Step(i);
    CHECK(o.ev == PEvent::Shot && !o.chambered && o.withholdR2);
    // control: the add alone (rounds 4) is not a shot
    Driver e;
    PInput j = Draw(e, 3, 10);
    j = TakeHandful(e, j);
    j.frameNow = 100; j.inPort = true;
    e.Step(j);
    j.inPort = false;
    j.frameNow = 122;
    e.Step(j);
    j.rounds = 4; j.frameNow = 123;
    o = e.Step(j);
    CHECK(o.ev == PEvent::None && o.chambered && !o.withholdR2);
    CHECK(e.Count(PEvent::Shot) == 0);
}

// Task 4 review M6 / ruling R15: the driver's add-bullet call FAULTED (the game added nothing) - the machine is told at once
// (AddFailed), so the unchanged round count on the next update is NOT a shot; the gun stays chambered. The control is the identical
// sequence without AddFailed (ShotHiddenByOwnAddIsSeen above: the same inputs read as a Shot).
void FaultedAddIsNotCounted()
{
    Driver d;
    PInput i = Draw(d, 3, 10);
    i = TakeHandful(d, i);
    i.frameNow = 100; i.inPort = true;
    d.Step(i);
    i.inPort = false;
    i.frameNow = 122;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.callAddBullet && o.chambered);
    d.m.AddFailed();      // CallAddBullet faulted: rounds stay 3
    i.frameNow = 123;
    o = d.Step(i);
    CHECK(o.ev == PEvent::None && o.chambered && !o.withholdR2);
    CHECK(d.Count(PEvent::Shot) == 0);
    // a real shot after that is still seen (nothing left counted)
    i.rounds = 2; i.frameNow = 124;
    o = d.Step(i);
    CHECK(o.ev == PEvent::Shot && !o.chambered);
    // AddFailed with nothing requested changes nothing (never below zero): a later add still counts
    Driver e;
    PInput j = Draw(e, 3, 10);
    e.m.AddFailed();
    j.rounds = 2;
    o = e.Step(j);
    CHECK(o.ev == PEvent::Shot);
    // control: without AddFailed the same faulted sequence reads as a (false) shot
    Driver c;
    PInput k = Draw(c, 3, 10);
    k = TakeHandful(c, k);
    k.frameNow = 100; k.inPort = true;
    c.Step(k);
    k.inPort = false;
    k.frameNow = 122;
    c.Step(k);
    k.frameNow = 123;
    o = c.Step(k);
    CHECK(o.ev == PEvent::Shot && !o.chambered);
}

// Final fix wave (#19, ruling R17): a stale controller sample HOLDS the last stroke - never 0, never full - so a stale stretch mid-rack
// raises no RACK BACK / CHAMBERED; the stroke is relative to the grip's first fresh sample (a moved-together pair = 0).
void StaleHoldsStroke()
{
    const float kTravel = 0.0574f;
    PStroke s;
    CHECK(s.Update(true, false, 0.30f, kTravel) == 0.0f);          // gripped, no fresh sample yet: nothing latched, 0
    CHECK(s.Update(true, true, 0.30f, kTravel) == 0.0f);           // the latch: s0 = 0.30
    CHECK(std::fabs(s.Update(true, true, 0.32f, kTravel) - 0.02f) < 1e-6f);
    CHECK(std::fabs(s.Update(true, false, 0.0f, kTravel) - 0.02f) < 1e-6f);    // stale with a far-off value: held
    CHECK(std::fabs(s.Update(true, false, 1.0f, kTravel) - 0.02f) < 1e-6f);    // stale the other way: held
    CHECK(std::fabs(s.Update(true, true, 0.31f, kTravel) - 0.01f) < 1e-6f);    // fresh again: follows
    CHECK(s.Update(true, true, 0.20f, kTravel) == 0.0f);           // forward of the latch clamps at 0
    CHECK(std::fabs(s.Update(true, true, 0.90f, kTravel) - kTravel) < 1e-6f); // past the travel clamps at travel
    CHECK(s.Update(false, true, 0.90f, kTravel) == 0.0f);          // released: 0
    CHECK(s.Update(true, true, 0.50f, kTravel) == 0.0f);           // a new grip latches afresh
    // Fed to the machine: back to 0.05 m (RACK BACK), then a stale stretch reading "0" must NOT be the forward end (no CHAMBERED);
    // the control: the same pull with a FRESH forward sample chambers.
    Driver d;
    PInput i = Draw(d, 3, 0);
    i.rounds = 2;   // a shot: not chambered
    d.Step(i);
    PStroke st;
    i.grippedPump = true;
    i.strokeFrac = st.Update(true, true, 0.40f, kTravel) / kTravel;
    d.Step(i);
    i.strokeFrac = st.Update(true, true, 0.45f, kTravel) / kTravel;
    POut o = d.Step(i);
    CHECK(o.Has(PEvent::RackBack));
    for (int k = 0; k < 10; ++k) {
        i.strokeFrac = st.Update(true, false, 0.0f, kTravel) / kTravel;   // stale, garbage value
        o = d.Step(i);
        CHECK(o.events == 0 && !o.chambered && o.withholdR2);
    }
    i.strokeFrac = st.Update(true, true, 0.40f, kTravel) / kTravel;      // fresh, forward: the real forward end
    o = d.Step(i);
    CHECK(o.Has(PEvent::Chambered) && o.chambered);
    CHECK(d.Count(PEvent::Shot) == 1);
}

// PS1 fixes (Jay: "0.3 s per shell"): 36 frames in 300 ms; the add-bullet frame 23 (machine frame >= start + 22) at 183 ms; the old
// 30 fps clock (1.2 s) would put it at 733 ms - the control that this test can fail.
void FrameClockAtPointThreeSeconds()
{
    CHECK(PumpFrameAt(0, kPumpCycleS, 36) == 0);
    CHECK(PumpFrameAt(300, kPumpCycleS, 36) == 36);
    CHECK(PumpFrameAt(150, kPumpCycleS, 36) == 18);
    // the first ms at which 22 frames have elapsed (the add at cycle frame 23): 22 x 300 / 36 = 183.3 -> 184 ms
    CHECK(PumpFrameAt(183, kPumpCycleS, 36) == 21 && PumpFrameAt(184, kPumpCycleS, 36) == 22);
    CHECK(PumpFrameAt(184, 1.2f, 36) == 5);   // control: the old 1.2 s clock is far slower
    CHECK(PumpFrameAt(100, 0.0f, 36) == 0);
    // through the machine: a push at t0, the add exactly once at >= 184 ms, the cycle over by 300 ms
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    const unsigned long long t0 = 5000;
    i.frameNow = PumpFrameAt(t0, kPumpCycleS, 36); i.inPort = true;
    d.Step(i);
    i.inPort = false;
    int addAt = -1;
    for (unsigned long long ms = 11; ms <= 330; ms += 11) {   // ~90 Hz updates
        i.frameNow = PumpFrameAt(t0 + ms, kPumpCycleS, 36);
        const POut o = d.Step(i);
        if (o.callAddBullet) { CHECK(addAt < 0); addAt = static_cast<int>(ms); }
    }
    CHECK(addAt >= 176 && addAt <= 198);   // the first ~90 Hz update at / after 184 ms (176 when the push update's own rounding helps)
    CHECK(d.addBullets == 1 && d.last.animCycle == 0);
}

// PS2 (Jay: "No snap until ready"): CanPush is true exactly when an entry now would PortPush - armed, shells in the hand, no cycle
// playing, room in the tube. The driver gates the snap (and the entry) on it, so a lock + buzz always loads a shell.
void CanPushOnlyWhenAPushWouldLoad()
{
    Driver d;
    PInput i = Draw(d, 0, 10);
    CHECK(!d.m.CanPush(0));                     // empty hand
    i = TakeHandful(d, i);
    CHECK(d.m.CanPush(0));                      // 4 in the hand, tube empty, armed
    CHECK(!d.m.CanPush(6));                     // tube full (capacity 6)
    i.frameNow = 100; i.inPort = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush);
    i.inPort = false;
    i.frameNow = 110; d.Step(i);
    CHECK(!d.m.CanPush(0));                     // a cycle plays (and disarmed until out of the port)
    i.outOfPort = true; i.frameNow = 120; d.Step(i); i.outOfPort = false;
    CHECK(!d.m.CanPush(1));                     // re-armed, but the cycle still plays
    for (int f = 121; f <= 140; ++f) { i.frameNow = f; if (d.Step(i).callAddBullet) { i.rounds = 1; } }   // the game's add lands
    CHECK(d.addBullets == 1 && d.last.animCycle == 0 && d.Count(PEvent::Shot) == 0);
    CHECK(d.m.CanPush(1));                      // cycle over, armed, 3 in the hand, room -> an entry now pushes
    i.inPort = true; i.frameNow = 141;
    o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush);            // and it did
}

void DrawnLoadedIsChambered()
{
    Driver d;
    Draw(d, 5, 0);
    CHECK(d.last.chambered && !d.last.withholdR2 && d.m.chambered());
    Driver e;
    Draw(e, 0, 0);
    CHECK(!e.last.chambered && e.last.withholdR2);
}

void NotManualIsIdle()
{
    Driver d;
    PInput i = Draw(d, 0, 10);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 4);
    i.manualOn = false;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::None && !o.withholdR2 && o.gameReloadAllowed && o.handShells == 0 && o.animCycle == 0 && !o.chambered);
    CHECK(d.m.handShells() == 0 && !d.m.chambered());
    // not a pump: the same
    Driver e;
    PInput j = Draw(e, 3, 10);
    j.isPump = false;
    o = e.Step(j);
    CHECK(!o.withholdR2 && o.gameReloadAllowed && !o.pumpFollowsHand);
    // and the state was reset: back on without a fresh draw, nothing is chambered
    j.isPump = true;
    o = e.Step(j);
    CHECK(!o.chambered && o.withholdR2);
}

void GameReloadRefusedWhileManual()
{
    Driver d;
    PInput i = Draw(d, 3, 5);
    CHECK(!d.last.gameReloadAllowed);
    CHECK(!d.Step(i).gameReloadAllowed);
    i.rounds = 0;
    CHECK(!d.Step(i).gameReloadAllowed);
    i.isPump = false;
    CHECK(d.Step(i).gameReloadAllowed);
    i.isPump = true; i.manualOn = false;
    CHECK(d.Step(i).gameReloadAllowed);
}

void HandfulOfTwoStartsAtCycleThree()
{
    // P4
    Driver d;
    PInput i = Draw(d, 0, 2);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 2);
    i.frameNow = 100; i.inPort = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush && o.animCycle == 3);
    i.inPort = false;
    i.frameNow = 122;
    o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.handShells == 1 && o.animCycle == 3);
    i.frameNow = 136;
    d.Step(i);
    i.outOfPort = true; i.frameNow = 137;
    d.Step(i);
    i.outOfPort = false; i.inPort = true; i.frameNow = 150;
    o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush && o.animCycle == 4);
    i.inPort = false;
    i.frameNow = 172;
    o = d.Step(i);
    CHECK(o.ev == PEvent::ShellIn && o.handShells == 0);
}

// ---- 2026-11-07 THE SEMI-AUTO wn_shotgun_c (Jay: "No rack"): PParams racks false, capacity 8 (its row).
void SemiParams(Driver& d) { d.p.racks = false; d.p.capacity = 8; }

// An empty semi-auto is not chambered (R2 withheld); the first shell in chambers it on the SAME update as the add-bullet (the count
// rises only by the next update), with no rack step; it stays chambered once the game's count shows the round.
void NoRackFirstShellChambers()
{
    Driver d;
    SemiParams(d);
    PInput i = Draw(d, 0, 10);
    CHECK(!d.last.chambered && d.last.withholdR2);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 4);
    i.frameNow = 100; i.inPort = true;
    POut o = d.Step(i);
    CHECK(o.ev == PEvent::PortPush && o.withholdR2);
    i.inPort = false;
    i.frameNow = 122;   // cycle frame 23 = the add-bullet
    o = d.Step(i);
    CHECK(o.Has(PEvent::ShellIn) && o.callAddBullet && o.chambered && !o.withholdR2);
    i.rounds = 1; i.frameNow = 123;
    o = d.Step(i);
    CHECK(o.chambered && !o.withholdR2 && !o.Has(PEvent::Shot));
    CHECK(d.Count(PEvent::RackBack) == 0 && d.Count(PEvent::RackForward) == 0 && d.Count(PEvent::Chambered) == 0);
}

// A shot on the semi-auto is seen (the count drops) but leaves it chambered while rounds remain; the last shot leaves it unchambered.
void NoRackShotStaysChamberedWhileRoundsRemain()
{
    Driver d;
    SemiParams(d);
    PInput i = Draw(d, 2, 0);
    CHECK(d.last.chambered && !d.last.withholdR2);
    i.rounds = 1;
    POut o = d.Step(i);
    CHECK(o.Has(PEvent::Shot) && o.chambered && !o.withholdR2);
    i.rounds = 0;
    o = d.Step(i);
    CHECK(o.Has(PEvent::Shot) && !o.chambered && o.withholdR2);
    CHECK(d.Count(PEvent::Shot) == 2);
}

// The stroke is never read on a gun that does not rack: a full "rack" with the grip raises nothing and chambers nothing.
void NoRackIgnoresStroke()
{
    Driver d;
    SemiParams(d);
    PInput i = Draw(d, 0, 0);
    i.grippedPump = true;
    for (float s : { 0.0f, 0.5f, 1.0f, 0.5f, 0.0f }) {
        i.strokeFrac = s;
        const POut o = d.Step(i);
        CHECK(o.ev == PEvent::None && !o.chambered && o.withholdR2);
    }
    i.grippedPump = false;
    CHECK(d.Step(i).ev == PEvent::None);   // no RackShort either
    // the control: the SAME inputs on a pump rack (RackBack, then RackForward on an empty tube)
    Driver e;
    PInput j = Draw(e, 0, 0);
    j.grippedPump = true;
    for (float s : { 0.0f, 0.5f, 1.0f, 0.5f, 0.0f }) { j.strokeFrac = s; e.Step(j); }
    CHECK(e.Count(PEvent::RackBack) == 1 && e.Count(PEvent::RackForward) == 1);
}

// The tube's capacity is the row's: the semi-auto takes and pushes up to 8 (the pumps 6). CanPush must be given the row's params - its
// default is the pumps' 6 (the driver passes them).
void CapacityIsTheRows()
{
    Driver d;
    SemiParams(d);
    PInput i = Draw(d, 6, 10);
    i = TakeHandful(d, i);
    CHECK(d.m.handShells() == 2);                  // 8 - 6
    CHECK(d.m.CanPush(6, d.p) && d.m.CanPush(7, d.p) && !d.m.CanPush(8, d.p));
    CHECK(!d.m.CanPush(6));                        // the default (the pumps' 6) would refuse it - why the driver passes p
    i.frameNow = 100; i.inPort = true; i.rounds = 7;
    CHECK(d.Step(i).ev == PEvent::PortPush);       // 7 of 8: room
    Driver e;                                      // the control: a pump at 6 of 6 takes nothing
    PInput j = Draw(e, 6, 10);
    j.gripDown = true; j.inHip = true;
    CHECK(e.Step(j).ev == PEvent::TakeNoShell);
}

}  // namespace

int main()
{
    RUN(NoRackFirstShellChambers);
    RUN(NoRackShotStaysChamberedWhileRoundsRemain);
    RUN(NoRackIgnoresStroke);
    RUN(CapacityIsTheRows);
    RUN(TakeGivesUpToFour);
    RUN(TakeNoShellRefused);
    RUN(TakeOnLateHipFlag);
    RUN(PushPlaysOneCycleAndAddsOnce);
    RUN(PushNeedsExitBeforeNext);
    RUN(FullGunRefusesPush);
    RUN(GripUpClearsHand);
    RUN(SwitchMidCycleCancelsCleanly);
    RUN(ShotUnchambers);
    RUN(FullStrokeChambers);
    RUN(HalfStrokeDoesNotChamber);
    RUN(RackEmptyTubeStaysUnchambered);
    RUN(RackChamberedLosesNothing);
    RUN(ShotDuringCycleUnchambers);
    RUN(ShotAndShellInSameUpdateKeepsBoth);
    RUN(TakeAndShotSameUpdateKeepsTakePulse);
    RUN(ForwardEndWithShotKeepsRackDone);
    RUN(ShotHiddenByOwnAddIsSeen);
    RUN(FaultedAddIsNotCounted);
    RUN(StaleHoldsStroke);
    RUN(FrameClockAtPointThreeSeconds);
    RUN(CanPushOnlyWhenAPushWouldLoad);
    RUN(DrawnLoadedIsChambered);
    RUN(NotManualIsIdle);
    RUN(GameReloadRefusedWhileManual);
    RUN(HandfulOfTwoStartsAtCycleThree);
    std::printf("pump_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
