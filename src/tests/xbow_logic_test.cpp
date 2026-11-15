// Standalone unit test for src/src/ovr2xr/xbow_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-03-manual-reload-crossbow.md Task 1. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\xbow_logic_test.cpp
#include "xbow_logic.h"

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

const XParams kP{};   // grab 0.08, done 0.177, catch 0.10, hold 0.15, seat 0.04, catchDeg 35

// A manual, empty crossbow with a spare bolt, everything far away.
XInput Base()
{
    XInput i{};
    i.manualOn = true;
    i.isCrossbow = true;
    i.rounds = 0;
    i.spare = 3;
    return i;
}

struct Driver {
    XMachine m;
    std::vector<XEvent> events;
    int haptics = 0;
    int addBullets = 0;
    std::vector<Fx> fxs;
    XOut last;
    XOut Step(const XInput& in)
    {
        last = m.Update(in, kP);
        if (last.ev != XEvent::None) { events.push_back(last.ev); }
        if (last.haptic) { ++haptics; }
        if (last.callAddBullet) { ++addBullets; }
        if (last.fx != Fx::None) { fxs.push_back(last.fx); }
        return last;
    }
    int Count(XEvent e) const
    {
        int n = 0;
        for (XEvent x : events) { if (x == e) { ++n; } }
        return n;
    }
};

// Presses at the rack and pulls it back to the catch; the grip is then UP again.
void RackToCocked(Driver& d)
{
    XInput i = Base();
    d.Step(i);                       // Idle -> Empty
    i.gripDown = true; i.handToRackM = 0.02f;
    d.Step(i);                       // RackGrab
    i.rackBackM = 0.18f;
    d.Step(i);                       // Catch
    i.gripDown = false; i.rackBackM = 0; i.handToRackM = 1e9f;
    d.Step(i);
}

// From Cocked: press at the shoulder -> Carry, grip stays down.
XInput TakeToCarry(Driver& d)
{
    XInput i = Base();
    i.gripDown = true; i.inShoulder = true;
    d.Step(i);
    i.inShoulder = false;
    return i;
}

void FullCycleRackTakeLaySeat()
{
    Driver d;
    XInput i = Base();
    d.Step(i);
    CHECK(d.m.step() == XStep::Empty);
    i.gripDown = true; i.handToRackM = 0.05f;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::RackGrab && o.fx == Fx::RackGrab && o.step == XStep::Racking && o.haptic);   // the rack grab pulses (Jay 2026-11-04)
    i.rackBackM = 0.10f;
    d.Step(i);
    i.rackBackM = 0.18f;
    o = d.Step(i);
    CHECK(o.ev == XEvent::Catch && o.fx == Fx::RackDone && o.haptic && o.step == XStep::Cocked);
    i.gripDown = false; i.rackBackM = 0; i.handToRackM = 1e9f;
    d.Step(i);
    i.gripDown = true; i.inShoulder = true;
    o = d.Step(i);
    CHECK(o.ev == XEvent::Take && o.fx == Fx::PouchTake && o.haptic && o.step == XStep::Carry);
    i.inShoulder = false; i.nockToLineM = 0.5f; i.dirDeg = 10;
    d.Step(i);
    i.nockToLineM = 0.05f;
    o = d.Step(i);
    CHECK(o.ev == XEvent::Lay && !o.haptic && o.step == XStep::Laid);
    i.nockToLineM = 0.02f; i.nockToSeatM = 0.03f;
    o = d.Step(i);
    CHECK(o.ev == XEvent::Seat && o.fx == Fx::Seat && o.haptic && o.callAddBullet && o.step == XStep::Idle);

    const XEvent want[] = { XEvent::RackGrab, XEvent::Catch, XEvent::Take, XEvent::Lay, XEvent::Seat };
    CHECK(d.events.size() == 5);
    for (size_t k = 0; k < 5 && k < d.events.size(); ++k) { CHECK(d.events[k] == want[k]); }
    CHECK(d.haptics == 4);   // rack grab, catch, take, seat
    CHECK(d.addBullets == 1);
    // fx order: RackGrab, RackDone (Catch), PouchTake, Seat
    CHECK(d.fxs.size() == 4);
}

void RackLetGoShortGoesBackToEmpty()
{
    Driver d;
    XInput i = Base();
    d.Step(i);
    i.gripDown = true; i.handToRackM = 0.02f;
    d.Step(i);
    i.rackBackM = 0.10f;
    d.Step(i);
    i.gripDown = false;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::RackLetGo && o.step == XStep::Empty && !o.haptic);
    CHECK(!o.rackFollowsHand && !o.stringAtCatch);
    // the next press at the rack starts again, the running max restarted
    i.gripDown = true; i.rackBackM = 0;
    o = d.Step(i);
    CHECK(o.ev == XEvent::RackGrab && o.step == XStep::Racking);
    i.rackBackM = 0.12f;
    o = d.Step(i);
    CHECK(o.step == XStep::Racking);   // 0.12 < 0.177: the old 0.10 max alone must not complete
}

void TakeBeforeCockRefused()
{
    Driver d;
    XInput i = Base();
    d.Step(i);
    i.gripDown = true; i.inShoulder = true;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::TakeNotCocked);
    CHECK(o.step == XStep::Empty);
    CHECK(!o.haptic && o.fx == Fx::None && d.haptics == 0);
}

void TakeWithNoSpareRefused()
{
    Driver d;
    RackToCocked(d);
    XInput i = Base();
    i.spare = 0; i.gripDown = true; i.inShoulder = true;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::TakeNoBolt);
    CHECK(o.step == XStep::Cocked);
    CHECK(!o.haptic && o.fx == Fx::None);
    CHECK(d.haptics == 2);   // the rack grab and the catch only
}

void EarlyReleaseInCarryKeepsCocked()
{
    Driver d;
    RackToCocked(d);
    XInput i = TakeToCarry(d);
    CHECK(d.m.step() == XStep::Carry);
    i.gripDown = false;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::EarlyRelease && o.step == XStep::Cocked && o.stringAtCatch && !o.boltInHand);
    CHECK(!o.haptic && !o.callAddBullet);
}

void EarlyReleaseWhenLaidKeepsCocked()
{
    Driver d;
    RackToCocked(d);
    XInput i = TakeToCarry(d);
    i.nockToLineM = 0.05f; i.dirDeg = 5;
    d.Step(i);
    CHECK(d.m.step() == XStep::Laid);
    i.gripDown = false;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::EarlyRelease && o.step == XStep::Cocked && !o.boltOnLine && !o.callAddBullet);
}

void BackwardsBoltNeverLays()
{
    Driver d;
    RackToCocked(d);
    XInput i = TakeToCarry(d);
    i.nockToLineM = 0; i.dirDeg = 180;
    for (int k = 0; k < 20; ++k) { d.Step(i); }
    CHECK(d.Count(XEvent::LayRefusedAngle) == 1);
    CHECK(d.Count(XEvent::Lay) == 0);
    CHECK(d.m.step() == XStep::Carry);
    // leaving the range and coming back re-arms the one message
    i.nockToLineM = 0.5f;
    d.Step(i);
    i.nockToLineM = 0;
    d.Step(i);
    CHECK(d.Count(XEvent::LayRefusedAngle) == 2);
}

void WanderOffLineUnlays()
{
    Driver d;
    RackToCocked(d);
    XInput i = TakeToCarry(d);
    i.nockToLineM = 0.05f; i.dirDeg = 5;
    d.Step(i);
    CHECK(d.m.step() == XStep::Laid);
    i.nockToLineM = 0.15f;   // exactly holdM: still laid
    d.Step(i);
    CHECK(d.m.step() == XStep::Laid);
    i.nockToLineM = kP.holdM + 0.01f;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::Unlay && o.step == XStep::Carry && o.boltInHand && !o.boltOnLine);
}

void LoadedIsIdle()
{
    Driver d;
    XInput i = Base();
    i.rounds = 1;
    i.gripDown = true; i.handToRackM = 0.02f; i.inShoulder = true;
    XOut o = d.Step(i);
    CHECK(o.step == XStep::Idle && !o.withholdR2 && o.ev == XEvent::None);
    CHECK(!o.gameReloadAllowed);
    i.gripDown = false;
    d.Step(i);
    i.gripDown = true;
    o = d.Step(i);
    CHECK(o.step == XStep::Idle && !o.withholdR2);
    // loading mid-reload (rounds became > 0) drops the step
    Driver e;
    RackToCocked(e);
    XInput j = Base();
    j.rounds = 1;
    o = e.Step(j);
    CHECK(o.step == XStep::Idle && !o.withholdR2);
}

// Jay after RUN XB1 ("Add all 3 fixes"): a redraw of the same crossbow keeps a cocked string cocked; a carried / laid bolt is
// dropped back (nothing consumed) and a new take works at once; a rack mid-pull goes forward.
void FreshDrawKeepsTheCock()
{
    for (int from = 0; from < 3; ++from) {
        Driver d;
        RackToCocked(d);
        XInput i = Base();
        if (from >= 1) { i = TakeToCarry(d); }
        if (from >= 2) { i.nockToLineM = 0.05f; i.dirDeg = 5; d.Step(i); CHECK(d.m.step() == XStep::Laid); }
        else if (from == 1) { CHECK(d.m.step() == XStep::Carry); }
        else { CHECK(d.m.step() == XStep::Cocked); }
        i.freshDraw = true;
        XOut o = d.Step(i);
        CHECK(o.step == XStep::Cocked);
        CHECK(o.stringAtCatch && !o.boltInHand && !o.boltOnLine && o.withholdR2);
        CHECK(!o.callAddBullet && !o.haptic);
        // still cocked: a shoulder press takes a bolt
        i.freshDraw = false; i.gripDown = false; i.nockToLineM = 1e9f; i.inShoulder = false;
        d.Step(i);
        i.gripDown = true; i.inShoulder = true;
        o = d.Step(i);
        CHECK(o.ev == XEvent::Take && o.step == XStep::Carry);
    }
}

void FreshDrawMidRackGoesForward()
{
    Driver d;
    XInput i = Base();
    i.handToRackM = 0.01f; i.gripDown = false;
    d.Step(i);
    i.gripDown = true;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::RackGrab && o.step == XStep::Racking);
    i.rackBackM = 0.05f;
    d.Step(i);
    i.freshDraw = true;
    o = d.Step(i);
    CHECK(o.step == XStep::Empty && !o.stringAtCatch);
}

void NotManualIsIdle()
{
    Driver d;
    RackToCocked(d);
    CHECK(d.m.step() == XStep::Cocked);
    XInput i = Base();
    i.manualOn = false;
    XOut o = d.Step(i);
    CHECK(o.step == XStep::Idle && !o.withholdR2 && o.gameReloadAllowed && !o.stringAtCatch);
    // not a crossbow: the same
    Driver e;
    RackToCocked(e);
    XInput j = Base();
    j.isCrossbow = false;
    o = e.Step(j);
    CHECK(o.step == XStep::Idle && !o.withholdR2 && o.gameReloadAllowed);
    // and the state was reset: back on, it starts at Empty, not Cocked
    o = e.Step(Base());
    CHECK(o.step == XStep::Empty);
}

void GameReloadRefusedWhileManual()
{
    Driver d;
    XOut o = d.Step(Base());
    CHECK(!o.gameReloadAllowed);
    RackToCocked(d);
    CHECK(!d.last.gameReloadAllowed);
    XInput i = Base();
    i.rounds = 2;
    CHECK(!d.Step(i).gameReloadAllowed);
    i.isCrossbow = false;
    CHECK(d.Step(i).gameReloadAllowed);
    i.isCrossbow = true; i.manualOn = false;
    CHECK(d.Step(i).gameReloadAllowed);
}

void OnePressOneTake()
{
    Driver d;
    RackToCocked(d);
    XInput i = Base();
    i.gripDown = true; i.inShoulder = true;
    for (int k = 0; k < 10; ++k) { d.Step(i); }
    CHECK(d.Count(XEvent::Take) == 1);
    // let go early, keep the zone: no new take until a NEW press
    i.gripDown = false;
    d.Step(i);
    CHECK(d.m.step() == XStep::Cocked);
    for (int k = 0; k < 10; ++k) { d.Step(i); }
    CHECK(d.Count(XEvent::Take) == 1);
    i.gripDown = true;
    d.Step(i);
    CHECK(d.Count(XEvent::Take) == 2);
    // a grip still held from the rack, with the hand carried into the zone, is not a press either
    Driver e;
    XInput j = Base();
    e.Step(j);
    j.gripDown = true; j.handToRackM = 0.02f;
    e.Step(j);
    j.rackBackM = 0.18f;
    e.Step(j);
    CHECK(e.m.step() == XStep::Cocked);
    j.inShoulder = true; j.handToRackM = 1e9f;
    for (int k = 0; k < 5; ++k) { e.Step(j); }
    CHECK(e.Count(XEvent::Take) == 0 && e.Count(XEvent::TakeNoBolt) == 0);
}

void StringHeldAtCatchUntilSeat()
{
    Driver d;
    XInput i = Base();
    d.Step(i);
    CHECK(!d.last.stringAtCatch);
    i.gripDown = true; i.handToRackM = 0.02f;
    d.Step(i);
    CHECK(d.last.rackFollowsHand && d.last.rackPose && !d.last.stringAtCatch && !d.last.carryPose);
    i.rackBackM = 0.18f;
    d.Step(i);
    CHECK(d.last.stringAtCatch && !d.last.rackFollowsHand && !d.last.rackPose);
    i.gripDown = false; i.rackBackM = 0; i.handToRackM = 1e9f;
    d.Step(i);
    CHECK(d.last.stringAtCatch && !d.last.boltInHand && !d.last.carryPose);
    i.gripDown = true; i.inShoulder = true;
    d.Step(i);
    CHECK(d.last.stringAtCatch && d.last.boltInHand && d.last.carryPose && !d.last.boltOnLine);
    i.inShoulder = false; i.nockToLineM = 0.05f; i.dirDeg = 5;
    d.Step(i);
    CHECK(d.last.stringAtCatch && d.last.carryPose && d.last.boltOnLine && !d.last.boltInHand);
    CHECK(d.last.withholdR2);
    i.nockToSeatM = 0.02f;
    d.Step(i);
    CHECK(d.last.step == XStep::Idle && !d.last.stringAtCatch && !d.last.withholdR2);
}

void SettleAfterSeat()
{
    Driver d;
    RackToCocked(d);
    XInput i = TakeToCarry(d);
    i.nockToLineM = 0.05f; i.dirDeg = 5;
    d.Step(i);
    i.nockToSeatM = 0.02f;
    XOut o = d.Step(i);
    CHECK(o.ev == XEvent::Seat && o.stringSettle);
    int trues = 1;
    // the game adds the bolt: rounds 1 from the next update on
    XInput k = Base();
    k.rounds = 1;
    for (int n = 0; n < kSettleUpdates + 10; ++n) {
        o = d.Step(k);
        if (o.stringSettle) { ++trues; }
        if (n == kSettleUpdates - 2) { CHECK(o.stringSettle); }
        if (n == kSettleUpdates - 1) { CHECK(!o.stringSettle); }
    }
    CHECK(trues == kSettleUpdates);
    // a seat with rounds still 0 (add-bullet not yet visible) stays Idle through the settle, no early Empty
    Driver e;
    RackToCocked(e);
    XInput j = TakeToCarry(e);
    j.nockToLineM = 0.05f; j.dirDeg = 5;
    e.Step(j);
    j.nockToSeatM = 0.02f;
    e.Step(j);
    XInput z = Base();
    for (int n = 0; n < 5; ++n) { CHECK(e.Step(z).step == XStep::Idle); }
}

// Jay 2026-11-05 ("The crosshair sits off and shakes" -> "Yes"; INV-676 addendum): while empty the game idles on Shoot 9-10, which
// ANIMATES bolt_pose (the aim point) 5.8 / 8.4 deg off the loaded aim. Empty and Racking hold it at rest; Laid never (the groove write);
// Cocked / Carry are the stringAtCatch rest write; loaded / not manual: the game's own.
void EmptyHoldsTheAimAtRest()
{
    Driver d;
    XInput i = Base();
    XOut o = d.Step(i);                                  // Idle -> Empty
    CHECK(o.step == XStep::Empty && o.boltAtRest);
    i.gripDown = true; i.handToRackM = 0.02f;
    o = d.Step(i);                                       // RackGrab
    CHECK(o.step == XStep::Racking && o.boltAtRest);
    i.rackBackM = 0.18f;
    o = d.Step(i);                                       // Catch -> Cocked
    CHECK(o.step == XStep::Cocked && !o.boltAtRest);     // Cocked: the stringAtCatch branch writes the rest
    XInput k = Base();
    k.rounds = 1;
    CHECK(!d.Step(k).boltAtRest);                        // loaded: the game's own pose
    XInput n = Base();
    n.manualOn = false;
    Driver e;
    CHECK(!e.Step(n).boltAtRest);                        // manual reload off: never
}

}  // namespace

int main()
{
    RUN(EmptyHoldsTheAimAtRest);
    RUN(FullCycleRackTakeLaySeat);
    RUN(RackLetGoShortGoesBackToEmpty);
    RUN(TakeBeforeCockRefused);
    RUN(TakeWithNoSpareRefused);
    RUN(EarlyReleaseInCarryKeepsCocked);
    RUN(EarlyReleaseWhenLaidKeepsCocked);
    RUN(BackwardsBoltNeverLays);
    RUN(WanderOffLineUnlays);
    RUN(LoadedIsIdle);
    RUN(FreshDrawKeepsTheCock);
    RUN(FreshDrawMidRackGoesForward);
    RUN(NotManualIsIdle);
    RUN(GameReloadRefusedWhileManual);
    RUN(OnePressOneTake);
    RUN(StringHeldAtCatchUntilSeat);
    RUN(SettleAfterSeat);
    std::printf("xbow_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
