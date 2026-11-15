// Standalone unit test for src/src/ovr2xr/reload_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-01-manual-reload-pistol.md Task 2 (+ Task 1 rulings: a fired-empty reload adds its rounds at the
// RACK, a tactical one at the seat). Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\reload_logic_test.cpp
#include "reload_logic.h"

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

namespace {

const Params kP{};   // snap 0.05, align 45, pull-out 0.06, rack 0.03, grab 0.08

Input Pistol(int rounds, int maxRounds, int spare)
{
    Input i{};
    i.family = Family::Pistol;
    i.manualOn = true;
    i.rounds = rounds;
    i.maxRounds = maxRounds;
    i.spare = spare;
    return i;
}

// Drives one Machine and records every non-None event and every update that asked for +DAF160.
struct Driver {
    Machine m;
    std::vector<Event> events;
    std::vector<Event> addBulletOn;   // the event of each update with callAddBullet
    Out last;
    Out Step(const Input& in)
    {
        last = m.Update(in, kP);
        if (last.ev != Event::None) { events.push_back(last.ev); }
        if (last.callAddBullet) { addBulletOn.push_back(last.ev); }
        return last;
    }
};

// grab the seated mag, pull it out by `out` m, let go (it drops), take a fresh one from the pouch
void PullDropTake(Driver& d, Input in, float out)
{
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);                       // Grab
    in.pullFromGrabM = out;
    d.Step(in);                       // PullOut (if out >= pull-out)
    in.gripDown = false;
    d.Step(in);                       // Drop
    in.leftInPouch = true;
    in.gripDown = true;
    d.Step(in);                       // PouchTake
    in.leftInPouch = false;
}

Input Seating(Input in)
{
    in.gripDown = true;
    in.handToWellM = 0.03f;
    in.alignDeg = 20.0f;
    return in;
}

bool Same(const std::vector<Event>& a, const std::vector<Event>& b) { return a == b; }

Input Smg(int rounds, int maxRounds, int spare)
{
    Input i = Pistol(rounds, maxRounds, spare);
    i.family = Family::Smg;
    i.racks = true;   // the SMG rows rack (reload_fx_table.inc) - racking is the row's, never the family's
    return i;
}

// Up to Racking with the handle grabbed (fired empty) - an SMG: the pistol has no rack step (Jay 2026-11-02).
void ToRackingSmg(Driver& d, Input in)
{
    in.family = Family::Smg;
    in.racks = true;
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));                    // Seat -> NeedsRack
    in.gripDown = false;
    d.Step(in);
    in.handToSlideM = 0.04f;
    in.gripDown = true;
    d.Step(in);                             // RackPull
}

// Jay 2026-11-02 (after RUN HP5, his reference video e5xA-XflXmw 2:33-2:37: "Slide is automatic on modern pistols" -> "Yes, no
// pistol rack"): a FIRED-EMPTY pistol reload completes at the SEAT - the rounds go in there and the game draws the slide forward;
// the pistol never enters NeedsRack / Racking.
void FullPistolReloadFromEmpty()
{
    Driver d;
    Input in = Pistol(0, 15, 30);
    CHECK(!d.Step(in).gameReloadAllowed);   // manual pistol: the game's own reload is refused even while idle
    CHECK(!d.last.withholdR2);              // ... but it may fire (it is empty anyway) - Idle never withholds
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));                    // Done - no rack
    CHECK(d.last.ev == Event::Done);
    CHECK(d.last.callAddBullet);
    CHECK(d.last.haptic);
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.withholdR2);
    CHECK(!d.m.firedEmpty());
    const std::vector<Event> want{ Event::Grab, Event::PullOut, Event::Drop, Event::PouchTake, Event::Done };
    CHECK(Same(d.events, want));
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::Done }));
    // a grip at the slide afterwards starts nothing
    Input s = in;
    s.gripDown = false;
    d.Step(s);
    s.handToSlideM = 0.01f;
    s.gripDown = true;
    d.Step(s);
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.addBulletOn.size() == 1);
}

// The SMG keeps the rack: fired empty -> Seat -> NeedsRack -> RackPull -> RackDone (grip held).
void FullSmgReloadFromEmpty()
{
    Driver d;
    Input in = Smg(0, 32, 64);
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));                    // Seat -> NeedsRack (fired empty)
    CHECK(d.m.step() == Step::NeedsRack);
    CHECK(d.last.withholdR2);
    Input r = in;
    r.gripDown = false;
    d.Step(r);                              // let go of the seated mag
    r.handToSlideM = 0.04f;
    r.gripDown = true;
    d.Step(r);                              // RackPull
    CHECK(d.last.slideFollowsHand);
    r.slideBackM = 0.04f;
    d.Step(r);                              // RackDone - at the rack-done distance, the grip still held (Jay after HP2)
    CHECK(d.last.ev == Event::RackDone);
    CHECK(d.last.haptic);
    r.gripDown = false;
    d.Step(r);                              // letting go afterwards adds nothing
    const std::vector<Event> want{ Event::Grab, Event::PullOut, Event::Drop, Event::PouchTake, Event::Seat, Event::RackPull,
                                   Event::RackDone };
    CHECK(Same(d.events, want));
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::RackDone }));   // Task 1 ruling: the slide goes forward with the rounds
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.withholdR2);
}

// Jay after RUN HP2 ("feels very restrictive" -> "Lets just try it with the free release on the rack first"): the rack completes as
// soon as the pull reaches the rack-done distance, the grip still held - the hand is free at once (Idle), and holding on does not
// start anything else (a new grab needs a new press).
void RackDoneWithoutRelease()
{
    Driver d;
    Input in = Smg(0, 32, 64);
    ToRackingSmg(d, in);
    Input r = in;
    r.gripDown = true;
    r.handToSlideM = 0.04f;
    r.slideBackM = 0.0299f;                 // just short of 0.03 (kP.rackPullM)
    d.Step(r);
    CHECK(d.m.step() == Step::Racking);
    CHECK(d.addBulletOn.empty());
    r.slideBackM = 0.03f;                   // boundary inclusive
    d.Step(r);
    CHECK(d.last.ev == Event::RackDone);
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.slideFollowsHand);
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::RackDone }));
    r.handToMagM = 0.03f;                   // still held, now at the mag: no grab without a new press
    for (int i = 0; i < 5; ++i) { d.Step(r); }
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.last.ev == Event::None);
    CHECK(d.addBulletOn.size() == 1);
}

// The slide / handle grab has its OWN radius (Params::rackGrabM - reload_guns.h RackGrabFor); the mag grab keeps grabM.
void RackGrabUsesItsOwnRadius()
{
    Params p12 = kP;
    p12.rackGrabM = 0.12f;
    Machine m;
    Input in = Smg(0, 32, 64);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    m.Update(in, p12);                      // Grab
    in.pullFromGrabM = 0.07f;
    m.Update(in, p12);                      // PullOut
    in.gripDown = false;
    m.Update(in, p12);                      // Drop
    in.leftInPouch = true;
    in.gripDown = true;
    m.Update(in, p12);                      // PouchTake
    in.leftInPouch = false;
    m.Update(Seating(in), p12);             // Seat -> NeedsRack
    CHECK(m.step() == Step::NeedsRack);
    Input r = in;
    r.gripDown = false;
    m.Update(r, p12);
    r.handToSlideM = 0.12f;                 // boundary inclusive, beyond the mag's 0.08
    r.gripDown = true;
    CHECK(m.Update(r, p12).ev == Event::RackPull);
    // the default radius (0.08) refuses the same hand
    Driver d;
    ToRackingSmg(d, Smg(0, 32, 64));        // (a grab at 0.04 - inside both)
    CHECK(d.m.step() == Step::Racking);
    Machine m2;
    Input j = Smg(0, 32, 64);
    j.handToMagM = 0.03f; j.gripDown = true; m2.Update(j, kP);
    j.pullFromGrabM = 0.07f; m2.Update(j, kP);
    j.gripDown = false; m2.Update(j, kP);
    j.leftInPouch = true; j.gripDown = true; m2.Update(j, kP);
    j.leftInPouch = false; m2.Update(Seating(j), kP);
    j.gripDown = false; m2.Update(j, kP);
    j.handToSlideM = 0.12f; j.gripDown = true;
    CHECK(m2.Update(j, kP).ev == Event::None);
    CHECK(m2.step() == Step::NeedsRack);
}

// The pull-out reads the hand's distance from where it grabbed, in ANY direction (Jay after HP2: the mag "In hand at the grip").
void PullOutAtTheDistanceInclusive()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);                             // Grab
    in.pullFromGrabM = 0.0599f;
    d.Step(in);
    CHECK(d.m.step() == Step::Pulling);
    in.pullFromGrabM = 0.06f;               // boundary inclusive
    d.Step(in);
    CHECK(d.last.ev == Event::PullOut);
    CHECK(d.m.step() == Step::MagInHand);
    CHECK(d.last.magFollowsHand);
}

void WithholdsR2FromGrabToRackDone()
{
    Driver d;
    Input in = Pistol(0, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    CHECK(d.Step(in).withholdR2);           // Grab
    in.pullFromGrabM = 0.07f;
    CHECK(d.Step(in).withholdR2);           // PullOut
    in.gripDown = false;
    CHECK(d.Step(in).withholdR2);           // Drop - Empty
    CHECK(d.last.magHidden);
    CHECK(!d.last.magFollowsHand);
}

void TacticalReloadSkipsRack()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.last.ev == Event::Done);
    CHECK(d.last.callAddBullet);
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::Done }));
    CHECK(!d.m.firedEmpty());
}

void PullTooShortSnapsBack()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.05f;            // < 0.06
    d.Step(in);
    CHECK(d.m.step() == Step::Pulling);
    in.gripDown = false;
    d.Step(in);
    CHECK(d.last.ev == Event::SnapBack);
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.magHidden);
    CHECK(!d.last.magFollowsHand);
}

void GrabNeedsTheHandAtTheMag()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.09f;                  // > grab 0.08
    in.gripDown = true;
    d.Step(in);
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.events.empty());
    in.gripDown = false;
    d.Step(in);
    in.handToMagM = 0.08f;                  // boundary inclusive
    in.gripDown = true;
    d.Step(in);
    CHECK(d.last.ev == Event::Grab);
}

void GripHeldBeforeReachingIsNoGrab()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.gripDown = true;                     // squeezed away from the gun ...
    in.handToMagM = 0.5f;
    d.Step(in);
    in.handToMagM = 0.02f;                  // ... then moved onto the mag, still squeezed
    d.Step(in);
    CHECK(d.m.step() == Step::Idle);        // a grab needs the press edge at the mag
}

Driver ToFresh(Input in)
{
    Driver d;
    PullDropTake(d, in, 0.07f);
    return d;
}

void SeatNeedsBothDistanceAndAngle()
{
    const Input in = Pistol(5, 15, 30);
    {
        Driver d = ToFresh(in);
        Input s = Seating(in);
        s.handToWellM = 0.04f;
        s.alignDeg = 60.0f;
        d.Step(s);
        CHECK(d.m.step() == Step::FreshInHand);
    }
    {
        Driver d = ToFresh(in);
        Input s = Seating(in);
        s.handToWellM = 0.06f;
        s.alignDeg = 10.0f;
        d.Step(s);
        CHECK(d.m.step() == Step::FreshInHand);
    }
    {
        Driver d = ToFresh(in);
        Input s = Seating(in);
        s.handToWellM = 0.05f;              // boundaries inclusive
        s.alignDeg = 45.0f;
        d.Step(s);
        CHECK(d.last.ev == Event::Done);
    }
}

void PouchEmptyGivesNothing()
{
    Driver d;
    Input in = Pistol(0, 15, 0);            // no spare AND no rounds left in the gun (RL2B: rounds left = the mag comes back)
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.07f;
    d.Step(in);
    in.gripDown = false;
    d.Step(in);                             // Empty
    in.leftInPouch = true;
    in.gripDown = true;
    d.Step(in);
    CHECK(d.last.ev == Event::PouchEmpty);
    CHECK(d.last.haptic);
    CHECK(d.m.step() == Step::Empty);
    CHECK(!d.last.magFollowsHand);
    CHECK(!d.last.callAddBullet);
}

// RUN RL2 (2026-11-01): the holster raises the pouch flag only AFTER its hip yield (8-9 ms) - i.e. on a LATER update than the
// grip's own press edge. The pouch must still give the mag.
void PouchFlagAfterTheGripEdgeTakes()
{
    Driver d;
    Input in = Pistol(0, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.07f;
    d.Step(in);
    in.gripDown = false;
    d.Step(in);                             // Empty
    in.gripDown = true;                     // the grip's press edge - the pouch has not answered yet
    in.leftInPouch = false;
    d.Step(in);
    CHECK(d.m.step() == Step::Empty);
    in.leftInPouch = true;                  // the holster's pouch press arrives one update later, grip still held
    d.Step(in);
    CHECK(d.last.ev == Event::PouchTake);
    CHECK(d.m.step() == Step::FreshInHand);
    CHECK(d.last.haptic);
}

// One pouch press = one take / one buzz: holding the grip in the pouch does not repeat it (PouchEmpty would buzz every update).
void OnePouchPressOneBuzz()
{
    Driver d;
    Input in = Pistol(0, 15, 0);            // no spare, no rounds - PouchEmpty, the machine stays Empty
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.07f;
    d.Step(in);
    in.gripDown = false;
    d.Step(in);                             // Empty
    in.gripDown = true;
    in.leftInPouch = true;
    d.Step(in);
    CHECK(d.last.ev == Event::PouchEmpty);
    d.Step(in);                             // still held in the pouch
    CHECK(d.last.ev == Event::None);
    CHECK(!d.last.haptic);
    in.gripDown = false;
    in.leftInPouch = false;
    d.Step(in);
    in.gripDown = true;
    in.leftInPouch = true;
    d.Step(in);                             // a NEW press
    CHECK(d.last.ev == Event::PouchEmpty);
}

// RUN RL2B (Jay: "Pulling the mag out without emptying the entire mag first can also soft lock"; picked "Give back if rounds
// left"): 12 rounds in the gun, 0 spare - the pouch gives the mag back (the game still counts the 12), it seats, the gun is usable.
void TacticalNoSpareGivesTheMagBack()
{
    Driver d;
    Input in = Pistol(12, 15, 0);
    PullDropTake(d, in, 0.07f);
    CHECK(d.last.ev == Event::PouchTake);
    CHECK(d.m.step() == Step::FreshInHand);
    d.Step(Seating(in));
    CHECK(d.last.ev == Event::Done);
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.withholdR2);
}

int SettleCount(Driver& d, Input in, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i) {
        if (d.Step(in).magAtSeat) { ++c; }
    }
    return c;
}

// RUN RL2B: after a seat the mag stayed drawn where the hand left it (5.4 cm out, 6 s) - the game does not always re-pose
// bone_magazine. The machine asks the driver to draw the SEAT for kSettleUpdates updates after every step that drew the mag ends
// in the gun (Done / Seat / SnapBack) - never after a Drop (the mag is out) and never from a plain Idle.
void SeatSettlesTheMag()
{
    {   // tactical: Done
        Driver d = ToFresh(Pistol(5, 15, 30));
        Input s = Seating(Pistol(5, 15, 30));
        CHECK(d.Step(s).magAtSeat);                     // the Done update itself
        s.gripDown = false;
        CHECK(SettleCount(d, s, kSettleUpdates + 5) == kSettleUpdates - 1);
    }
    {   // fired empty SMG: Seat -> NeedsRack
        Driver d = ToFresh(Smg(0, 32, 64));
        Input s = Seating(Smg(0, 32, 64));
        CHECK(d.Step(s).magAtSeat);
        CHECK(d.m.step() == Step::NeedsRack);
    }
    {   // fired empty PISTOL: Done at the seat (no rack) - settles too
        Driver d = ToFresh(Pistol(0, 15, 30));
        Input s = Seating(Pistol(0, 15, 30));
        CHECK(d.Step(s).magAtSeat);
        CHECK(d.last.ev == Event::Done);
        CHECK(d.m.step() == Step::Idle);
    }
    {   // a short pull let go: SnapBack
        Driver d;
        Input in = Pistol(5, 15, 30);
        in.handToMagM = 0.03f;
        in.gripDown = true;
        d.Step(in);
        in.pullFromGrabM = 0.02f;
        d.Step(in);
        in.gripDown = false;
        CHECK(d.Step(in).magAtSeat);
        CHECK(d.last.ev == Event::SnapBack);
    }
    {   // a Drop never settles; a plain Idle never does
        Driver d;
        Input in = Pistol(5, 15, 30);
        CHECK(SettleCount(d, in, 10) == 0);
        in.handToMagM = 0.03f;
        in.gripDown = true;
        d.Step(in);
        in.pullFromGrabM = 0.07f;
        d.Step(in);
        in.gripDown = false;
        CHECK(!d.Step(in).magAtSeat);                   // Drop
        CHECK(d.last.magHidden);
    }
}

void FullClipSeatAddsNothing()
{
    Driver d;
    Input in = Pistol(15, 15, 30);          // already full at the pull
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));
    CHECK(d.m.step() == Step::Idle);        // not NeedsRack - it was not fired empty
    CHECK(d.last.ev == Event::Done);
    CHECK(d.last.callAddBullet);            // the game's own +DAF160 adds max - current = 0
}

void FreshMagLetGoDrops()
{
    Driver d = ToFresh(Pistol(5, 15, 30));
    Input in = Pistol(5, 15, 30);
    in.gripDown = false;
    d.Step(in);
    CHECK(d.last.ev == Event::Drop);
    CHECK(d.m.step() == Step::Empty);
    CHECK(d.last.magHidden);
}

void GripOutsidePouchDoesNothing()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.07f;
    d.Step(in);
    in.gripDown = false;
    d.Step(in);                             // Empty
    const size_t n = d.events.size();
    in.leftInPouch = false;
    in.handToMagM = 0.01f;                  // even right at the (empty) well
    in.gripDown = true;
    d.Step(in);
    CHECK(d.m.step() == Step::Empty);
    CHECK(d.events.size() == n);
}

void ShortRackDoesNotFinish()
{
    Driver d;
    Input in = Smg(0, 32, 64);
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));
    Input r = in;
    r.gripDown = false;
    d.Step(r);
    r.handToSlideM = 0.04f;
    r.gripDown = true;
    d.Step(r);                              // RackPull
    r.slideBackM = 0.02f;                   // < 0.03
    d.Step(r);
    r.gripDown = false;
    d.Step(r);
    CHECK(d.m.step() == Step::NeedsRack);
    CHECK(d.addBulletOn.empty());
    CHECK(d.last.withholdR2);
}

void UnbuiltFamilyKeepsGameReload()
{
    Driver d;
    Input in = Pistol(0, 2, 10);
    in.family = Family::None;
    in.handToMagM = 0.01f;
    in.gripDown = true;
    d.Step(in);
    CHECK(d.last.gameReloadAllowed);
    CHECK(!d.last.withholdR2);
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.events.empty());
}

void ManualOffIsInert()
{
    Driver d;
    Input in = Pistol(0, 15, 30);
    in.manualOn = false;
    in.handToMagM = 0.01f;
    in.gripDown = true;
    const Out o = d.Step(in);
    const Out def{};
    CHECK(o.step == def.step);
    CHECK(o.ev == def.ev);
    CHECK(o.withholdR2 == def.withholdR2);
    CHECK(o.gameReloadAllowed == def.gameReloadAllowed);
    CHECK(o.magFollowsHand == def.magFollowsHand);
    CHECK(o.magHidden == def.magHidden);
    CHECK(o.slideFollowsHand == def.slideFollowsHand);
    CHECK(o.callAddBullet == def.callAddBullet);
    CHECK(o.haptic == def.haptic);
}

void StatePerItemSurvivesSwitch()
{
    Driver a, b;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    a.Step(in);
    in.pullFromGrabM = 0.07f;
    a.Step(in);
    in.gripDown = false;
    a.Step(in);                             // A: Empty
    PullDropTake(b, Pistol(5, 15, 30), 0.07f);
    b.Step(Seating(Pistol(5, 15, 30)));     // B: a whole tactical reload
    CHECK(b.m.step() == Step::Idle);
    Input idle = Pistol(5, 15, 30);
    a.Step(idle);                           // A updated again, nothing happening
    CHECK(a.m.step() == Step::Empty);
    CHECK(a.last.withholdR2);
    CHECK(!a.last.gameReloadAllowed);
}

void ManualOffMidReloadReleasesTheGun()
{
    Driver d;
    Input in = Pistol(5, 15, 30);
    in.handToMagM = 0.03f;
    in.gripDown = true;
    d.Step(in);
    in.pullFromGrabM = 0.07f;
    d.Step(in);
    in.gripDown = false;
    d.Step(in);                             // Empty
    in.manualOn = false;
    d.Step(in);
    CHECK(d.m.step() == Step::Idle);
    CHECK(!d.last.withholdR2);
    CHECK(d.last.gameReloadAllowed);
    CHECK(!d.last.magHidden);               // the gun shows its mag again
}

// ---- SMG plan (docs/superpowers/plans/2026-11-01-manual-reload-smg.md Task 2) ----

// Up to Racking with the slide grabbed (fired empty), for the bolt settle tests.
void ToRacking(Driver& d, Input in)
{
    PullDropTake(d, in, 0.07f);
    d.Step(Seating(in));                    // Seat -> NeedsRack
    in.gripDown = false;
    d.Step(in);
    in.handToSlideM = 0.04f;
    in.gripDown = true;
    d.Step(in);                             // RackPull
}

void SmgRunsThePistolSteps()
{
    Driver d;
    Input in = Pistol(0, 32, 64);
    in.family = Family::Smg;
    in.racks = true;
    CHECK(!d.Step(in).gameReloadAllowed);
    ToRacking(d, in);
    Input r = in;
    r.gripDown = true;
    r.handToSlideM = 0.04f;
    r.slideBackM = 0.04f;
    d.Step(r);                              // RackDone (grip held)
    r.gripDown = false;
    d.Step(r);
    const std::vector<Event> want{ Event::Grab, Event::PullOut, Event::Drop, Event::PouchTake, Event::Seat, Event::RackPull,
                                   Event::RackDone };
    CHECK(Same(d.events, want));
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::RackDone }));
    CHECK(d.m.step() == Step::Idle);
}

void RackDoneSettlesTheBoltAtRest()
{
    Driver d;
    Input in = Pistol(0, 32, 64);
    in.family = Family::Smg;
    in.racks = true;
    ToRacking(d, in);
    CHECK(!d.last.slideAtRest);             // mid-rack: the bolt follows the hand, no settle
    CHECK(!d.last.slideAtGrab);
    Input r = in;
    r.gripDown = true;
    r.handToSlideM = 0.04f;
    r.slideBackM = 0.04f;
    d.Step(r);                              // RackDone (grip held)
    CHECK(d.last.ev == Event::RackDone);
    CHECK(d.last.slideAtRest);
    CHECK(!d.last.slideAtGrab);
    int n = 1;
    while (n < 100 && d.Step(r).slideAtRest) { ++n; }
    CHECK(n == kSettleUpdates);
    CHECK(!d.last.slideAtRest);
}

void ShortRackSettlesTheBoltAtTheGrab()
{
    Driver d;
    Input in = Pistol(0, 32, 64);
    in.family = Family::Smg;
    in.racks = true;
    ToRacking(d, in);
    Input r = in;
    r.gripDown = true;
    r.handToSlideM = 0.04f;
    r.slideBackM = 0.01f;                   // < rack pull 0.03
    d.Step(r);
    r.gripDown = false;
    d.Step(r);
    CHECK(d.m.step() == Step::NeedsRack);
    CHECK(d.last.slideAtGrab);
    CHECK(!d.last.slideAtRest);
    int n = 1;
    while (n < 100 && d.Step(r).slideAtGrab) { ++n; }
    CHECK(n == kSettleUpdates);
    // a new rack grab clears any settle at once
    ToRacking(d, in);   // (pull / drop steps are no-ops from NeedsRack; the last update is the RackPull edge)
    CHECK(!d.last.slideAtGrab);
}

// Solid guns plan Task 3 (Jay 2026-11-02: "Some guns have racking, some do not. ARs are an example of guns that do."): racking
// is the gun ROW's field (in.racks), never the family. An Smg-family gun whose row does not rack completes at the seat; a
// Pistol-family gun whose row racks (an AR-style row later) goes to NeedsRack.
void RackingFromRowNotFamily()
{
    {
        Driver d;
        Input in = Smg(0, 32, 64);
        in.racks = false;
        PullDropTake(d, in, 0.07f);
        d.Step(Seating(in));
        CHECK(d.m.step() == Step::Idle);
        CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::Done }));
    }
    {
        Driver d;
        Input in = Pistol(0, 15, 30);
        in.racks = true;
        PullDropTake(d, in, 0.07f);
        d.Step(Seating(in));
        CHECK(d.m.step() == Step::NeedsRack);
    }
}

// One Fx per event that the player DOES (sound + haptics hang off it - Task 5): Grab -> MagGrab, PullOut -> MagOut, PouchTake,
// Seat / Done -> Seat, RackPull -> RackGrab, RackDone. Drop / SnapBack / PouchEmpty / None -> Fx::None.
void FxPerEvent()
{
    Driver d;
    Input in = Smg(0, 32, 64);
    std::vector<Fx> fx;
    auto step = [&](const Input& i) { const Out o = d.Step(i); if (o.fx != Fx::None) { fx.push_back(o.fx); } return o; };
    Input g = in;
    g.handToMagM = 0.03f;
    g.gripDown = true;
    step(g);                                // Grab
    g.pullFromGrabM = 0.07f;
    step(g);                                // PullOut
    g.gripDown = false;
    step(g);                                // Drop -> no fx
    g.leftInPouch = true;
    g.gripDown = true;
    step(g);                                // PouchTake
    g.leftInPouch = false;
    step(Seating(g));                       // Seat
    g.gripDown = false;
    step(g);
    g.handToSlideM = 0.04f;
    g.gripDown = true;
    step(g);                                // RackPull
    g.slideBackM = 0.04f;
    step(g);                                // RackDone
    const std::vector<Fx> want{ Fx::MagGrab, Fx::MagOut, Fx::PouchTake, Fx::Seat, Fx::RackGrab, Fx::RackDone };
    CHECK(fx == want);
    // a tactical seat (Done, no rack) is Fx::Seat too
    Driver t;
    Input ti = Smg(10, 32, 64);
    PullDropTake(t, ti, 0.07f);
    CHECK(t.Step(Seating(ti)).ev == Event::Done);
    CHECK(t.last.fx == Fx::Seat);
}

// Task 5: every step the player does pulses once - a fired-empty reload on a row that does NOT rack (the pistol) gives exactly
// MagGrab, MagOut, PouchTake, Seat (no rack fx), each once, and nothing on the Drop.
void EveryStepPulsesOnce()
{
    Driver d;
    Input in = Pistol(0, 15, 30);   // racks = false (the pistol row)
    std::vector<Fx> fx;
    auto step = [&](const Input& i) { const Out o = d.Step(i); if (o.fx != Fx::None) { fx.push_back(o.fx); } };
    Input g = in;
    g.handToMagM = 0.03f;
    g.gripDown = true;
    step(g);
    g.pullFromGrabM = 0.07f;
    step(g);
    step(g);                        // held: no second MagOut
    g.gripDown = false;
    step(g);                        // Drop
    g.leftInPouch = true;
    g.gripDown = true;
    step(g);
    step(g);                        // still pressed in the pouch: no second take
    g.leftInPouch = false;
    step(Seating(g));
    step(Seating(g));               // seated already: no second Seat
    const std::vector<Fx> want{ Fx::MagGrab, Fx::MagOut, Fx::PouchTake, Fx::Seat };
    CHECK(fx == want);
}

// ---- rifle manual reload, Task 1 (plan 2026-11-03-manual-reload-rifle): Family::Rifle runs the same steps; racking is the Input's
// `racks` (the gun's FxRow), never the family. The Railgun's row says racks = false.
Input Rifle(int rounds, int maxRounds, int spare, bool racks)
{
    Input i = Pistol(rounds, maxRounds, spare);
    i.family = Family::Rifle;
    i.racks = racks;
    return i;
}

void RailgunNeverRacks()
{
    CHECK(IsTableGun(Family::Smg));
    CHECK(IsTableGun(Family::Rifle));
    CHECK(!IsTableGun(Family::Pistol));
    CHECK(!IsTableGun(Family::None));
    Driver d;
    const Input in = Rifle(0, 5, 10, false);       // fired empty
    PullDropTake(d, in, 0.07f);
    CHECK(d.m.firedEmpty());
    d.Step(Seating(in));
    CHECK(d.last.ev == Event::Done);               // done at the seat ...
    CHECK(d.last.callAddBullet);
    CHECK(d.m.step() == Step::Idle);               // ... never NeedsRack
    CHECK(Same(d.addBulletOn, std::vector<Event>{ Event::Done }));
    CHECK(d.last.fx == Fx::Seat);
}

void RifleTacticalNoRack()
{
    Driver d;
    const Input in = Rifle(10, 30, 60, true);      // rounds left: a tactical reload, even on a racking rifle
    PullDropTake(d, in, 0.07f);
    CHECK(!d.m.firedEmpty());
    d.Step(Seating(in));
    CHECK(d.last.ev == Event::Done);
    CHECK(d.last.callAddBullet);
    CHECK(d.m.step() == Step::Idle);
    CHECK(d.last.fx == Fx::Seat);
}

void RifleFiredEmptyFx()
{
    for (int racks = 1; racks >= 0; --racks) {
        Machine m;
        std::vector<Fx> fx;
        auto step = [&](const Input& i) { const Out o = m.Update(i, kP); if (o.fx != Fx::None) { fx.push_back(o.fx); } };
        const Input in = Rifle(0, 30, 60, racks != 0);
        Input g = in;
        g.handToMagM = 0.03f;
        g.gripDown = true;
        step(g);
        g.pullFromGrabM = 0.07f;
        step(g);
        g.gripDown = false;
        step(g);
        g.leftInPouch = true;
        g.gripDown = true;
        step(g);
        g.leftInPouch = false;
        step(Seating(g));
        if (racks) {
            Input r = in;
            r.gripDown = false;
            step(r);
            r.handToSlideM = 0.04f;
            r.gripDown = true;
            step(r);                    // RackGrab
            r.slideBackM = 0.05f;
            step(r);                    // RackDone
            const std::vector<Fx> want{ Fx::MagGrab, Fx::MagOut, Fx::PouchTake, Fx::Seat, Fx::RackGrab, Fx::RackDone };
            CHECK(fx == want);
        } else {
            const std::vector<Fx> want{ Fx::MagGrab, Fx::MagOut, Fx::PouchTake, Fx::Seat };
            CHECK(fx == want);          // the Railgun: the first four only (spec section 8)
        }
    }
}

// Rifle plan Task 5 (Task 0 U-d): one left press never starts both the reload and the two-handed hold.
void LeftPressGoesToReloadOrTwoHand()
{
    // Outcome 1: a press at the mag (Idle, in the grab zone) is the reload's - the two-handed hold is refused.
    CHECK(ReloadOwnsLeftPress(false, true));
    CHECK(TwoHandPressRefused(false, ReloadOwnsLeftPress(false, true)));
    // ... and mid-reload (a step live) anywhere, even outside every grab zone (e.g. the hand at the front grip while Empty).
    CHECK(ReloadOwnsLeftPress(true, false));
    CHECK(TwoHandPressRefused(false, ReloadOwnsLeftPress(true, false)));
    // Outcome 2: Idle, outside the reload's zones (the front grip) - the reload does not own it, the two-handed hold may grab.
    CHECK(!ReloadOwnsLeftPress(false, false));
    CHECK(!TwoHandPressRefused(false, ReloadOwnsLeftPress(false, false)));
    // A left holster's press stays the holster's whatever the reload says.
    CHECK(TwoHandPressRefused(true, false));
    // Through the machine: a press at the mag starts Pulling (the reload's); after a tactical reload completes (Idle again), a NEW
    // press away from the mag is the two-handed hold's - nothing re-grips on its own (the arbitration only answers at a press).
    Driver d;
    Input in = Rifle(10, 30, 60, true);
    in.handToMagM = 0.03f;
    const bool zoneAtEdge = in.handToMagM <= kP.grabM;
    CHECK(ReloadOwnsLeftPress(d.m.step() != Step::Idle, zoneAtEdge));
    PullDropTake(d, in, 0.07f);
    CHECK(ReloadOwnsLeftPress(d.m.step() != Step::Idle, false));   // FreshInHand: live
    d.Step(Seating(in));
    CHECK(d.m.step() == Step::Idle);
    Input away = Rifle(10, 30, 60, true);
    away.handToMagM = 0.20f;                                         // at the front grip
    d.Step(away);
    CHECK(!ReloadOwnsLeftPress(d.m.step() != Step::Idle, away.handToMagM <= kP.grabM));
    CHECK(!TwoHandPressRefused(false, ReloadOwnsLeftPress(d.m.step() != Step::Idle, away.handToMagM <= kP.grabM)));
}

}  // namespace

int main()
{
    EveryStepPulsesOnce();
    RackingFromRowNotFamily();
    FxPerEvent();
    FullPistolReloadFromEmpty();
    FullSmgReloadFromEmpty();
    WithholdsR2FromGrabToRackDone();
    TacticalReloadSkipsRack();
    PullTooShortSnapsBack();
    GrabNeedsTheHandAtTheMag();
    GripHeldBeforeReachingIsNoGrab();
    SeatNeedsBothDistanceAndAngle();
    PouchEmptyGivesNothing();
    PouchFlagAfterTheGripEdgeTakes();
    OnePouchPressOneBuzz();
    TacticalNoSpareGivesTheMagBack();
    SeatSettlesTheMag();
    FullClipSeatAddsNothing();
    FreshMagLetGoDrops();
    GripOutsidePouchDoesNothing();
    ShortRackDoesNotFinish();
    UnbuiltFamilyKeepsGameReload();
    ManualOffIsInert();
    StatePerItemSurvivesSwitch();
    ManualOffMidReloadReleasesTheGun();
    SmgRunsThePistolSteps();
    RackDoneSettlesTheBoltAtRest();
    ShortRackSettlesTheBoltAtTheGrab();
    RackDoneWithoutRelease();
    PullOutAtTheDistanceInclusive();
    RackGrabUsesItsOwnRadius();
    RailgunNeverRacks();
    RifleTacticalNoRack();
    RifleFiredEmptyFx();
    LeftPressGoesToReloadOrTwoHand();
    std::printf("reload_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
