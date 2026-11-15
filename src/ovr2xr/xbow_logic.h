// xbow_logic.h - MANUAL RELOAD of the CROSSBOW, the hand steps (pure, unit-tested: src/src/tests/xbow_logic_test.cpp). No game includes.
// Plan docs/superpowers/plans/2026-11-03-manual-reload-crossbow.md Task 1; spec docs/superpowers/specs/2026-11-03-manual-reload-crossbow-design.md
// (ORDER CHANGED: rack the string back FIRST with the empty left hand, THEN take a bolt from the left shoulder and lay it on the line).
// The crossbow's analogue of the gun Machine in reload_logic.h: this decides the steps, the driver (reload.cpp) draws the parts, withholds
// R2 and calls the game's own add-bullet when callAddBullet is set. The steps:
//   Idle    loaded (rounds > 0) or not manual / not a crossbow - the game draws it.
//   Empty   string FORWARD (fired). A grip edge at the rack handle -> Racking.
//   Racking the rack handle follows the hand; the running max of the pull-back reaches rackDoneM -> Cocked, let go short -> Empty.
//   Cocked  string at the catch, no bolt. A left-shoulder grip EDGE takes a bolt (spare > 0) -> Carry.
//   Carry   the bolt is in the hand. Grip up -> Cocked; the nock on the line and the angle inside catchDeg -> Laid.
//   Laid    the bolt sits on the groove. Grip up -> Cocked; hand off the line -> Carry; nock at the seat -> Idle (Seat, add-bullet).
// A take before the string is at the catch is REFUSED (ledger ruling R1, no pulse). A fresh draw of the same item (the driver's
// freshDraw) keeps a cocked string cocked (Jay after RUN XB1); a rack mid-pull goes back to Empty.
#pragma once

#include "reload_logic.h"   // Fx, kSettleUpdates

namespace dlvr {
namespace reload {

enum class XStep { Idle, Empty, Racking, Cocked, Carry, Laid };
enum class XEvent { None, RackGrab, RackLetGo, Catch, Take, TakeNoBolt, TakeNotCocked, Lay, LayRefusedAngle, Unlay, EarlyRelease, Seat };

// The bow nock's live tolerances (BowNockCatchM / HoldM / SeatM / CatchDeg), the rack grab radius (reload_guns.h kXbowRackGrabM) and the rack's catch
// distance (travel x ReloadRackPullM / kPistolSlideTravelM) - the driver fills these from the live values; no new settings key.
struct XParams { float rackGrabM = 0.08f, rackDoneM = 0.177f, catchM = 0.10f, holdM = 0.15f, seatM = 0.04f, catchDeg = 35.0f; };

struct XInput {
    bool  manualOn = false;
    bool  isCrossbow = false;
    bool  freshDraw = false;        // the crossbow was just drawn / switched to: Cocked / Carry / Laid -> Cocked, Racking -> Empty
    bool  gripDown = false;         // LEFT grip, already debounced
    bool  inShoulder = false;       // left hand inside the left-shoulder take zone
    int   rounds = 0, spare = 0;
    float handToRackM = 1e9f;       // left grip -> the rack handle
    float rackBackM = 0;            // hand travel back along the rack axis since the grab (+ = back)
    float nockToLineM = 1e9f;       // the held bolt's nock -> the groove line
    float nockToSeatM = 1e9f;       // the held bolt's nock -> the seat
    float dirDeg = 180;             // the held bolt's direction vs the groove's
};

struct XOut {
    XStep  step = XStep::Idle;
    XEvent ev = XEvent::None;
    bool   withholdR2 = false;          // the crossbow cannot fire: every step but Idle
    bool   gameReloadAllowed = true;    // false whenever manual + crossbow
    bool   rackFollowsHand = false;     // Racking: draw the rack handle at the hand's travel
    bool   stringAtCatch = false;       // Cocked / Carry / Laid: draw the string at the catch
    bool   boltInHand = false;          // Carry
    bool   boltOnLine = false;          // Laid
    bool   rackPose = false;            // Racking: the rack-grab hand pose
    bool   carryPose = false;           // Carry / Laid: the bolt-carry hand pose
    bool   callAddBullet = false;       // call the game's add-bullet ONCE, this update
    bool   haptic = false;              // one short pulse on the left controller
    Fx     fx = Fx::None;               // the player's action this update (one per update; from ev)
    bool   stringSettle = false;        // kSettleUpdates updates after Seat: the driver draws the string as the game does
    // Empty / Racking: the driver holds bolt_pose (the AIM POINT, INV-494) at its rest - the game's empty idle (Shoot 9-10) animates it
    // 5.8 / 8.4 deg off the loaded aim (INV-676 addendum; Jay 2026-11-05: "The crosshair sits off and shakes").
    bool   boltAtRest = false;
};

class XMachine {
public:
    XOut Update(const XInput& in, const XParams& p)
    {
        const bool edge = in.gripDown && !gripWas_;
        gripWas_ = in.gripDown;
        if (!in.manualOn || !in.isCrossbow) {
            Reset();
            settle_ = 0;
            return XOut{};
        }
        XOut o{};
        o.gameReloadAllowed = false;
        if (in.rounds > 0) {
            // Loaded: the game draws it. settle_ is NOT cleared - the add-bullet after a Seat makes rounds > 0 on the next update and
            // the settle must run on through it.
            Reset();
        } else if (in.freshDraw && step_ != XStep::Idle) {
            // Jay after RUN XB1 (2026-11-03: "Add all 3 fixes" - keep the cocked state through a redraw of the same crossbow item;
            // supersedes ruling R4's "back to Empty"): a cocked string stays cocked - Cocked / Carry / Laid come back Cocked (the driver
            // gives a carried / laid bolt back on the redraw; nothing is consumed); a rack mid-pull goes forward (Empty).
            step_ = (step_ == XStep::Cocked || step_ == XStep::Carry || step_ == XStep::Laid) ? XStep::Cocked : XStep::Empty;
            rackMax_ = 0;
            inRange_ = false;
        } else {
            // Empty (fired) is where an unloaded crossbow rests; not during the settle after a Seat (rounds is still 0 for the
            // update before the add-bullet shows).
            if (step_ == XStep::Idle && settle_ == 0) { step_ = XStep::Empty; }
            Step_(in, p, edge, o);
        }
        o.step = step_;
        switch (o.ev) {
        case XEvent::RackGrab: o.fx = Fx::RackGrab; break;
        case XEvent::Catch: o.fx = Fx::RackDone; break;
        case XEvent::Take: o.fx = Fx::PouchTake; break;
        case XEvent::Seat: o.fx = Fx::Seat; break;
        default: break;
        }
        o.withholdR2 = step_ != XStep::Idle;
        o.rackFollowsHand = step_ == XStep::Racking;
        o.rackPose = step_ == XStep::Racking;
        o.stringAtCatch = step_ == XStep::Cocked || step_ == XStep::Carry || step_ == XStep::Laid;
        o.boltInHand = step_ == XStep::Carry;
        o.boltOnLine = step_ == XStep::Laid;
        o.carryPose = step_ == XStep::Carry || step_ == XStep::Laid;
        o.boltAtRest = step_ == XStep::Empty || step_ == XStep::Racking;
        if (o.ev == XEvent::Seat) { settle_ = kSettleUpdates; }
        o.stringSettle = settle_ > 0;
        if (settle_ > 0) { --settle_; }
        return o;
    }
    XStep step() const { return step_; }

private:
    void Reset() { step_ = XStep::Idle; rackMax_ = 0; inRange_ = false; }

    void Step_(const XInput& in, const XParams& p, bool edge, XOut& o)
    {
        switch (step_) {
        case XStep::Idle:
            break;
        case XStep::Empty:
            if (edge && in.handToRackM <= p.rackGrabM) {
                step_ = XStep::Racking;
                o.ev = XEvent::RackGrab;
                o.haptic = true;   // Jay after RUN XB1b ("Rack needs ... haptic buzz"), 2026-11-04
                rackMax_ = 0;
            } else if (edge && in.inShoulder) {
                // R1: the string is forward - nothing to load a bolt onto. Logged by the driver, no step change, no pulse.
                o.ev = XEvent::TakeNotCocked;
            }
            break;
        case XStep::Racking:
            if (in.rackBackM > rackMax_) { rackMax_ = in.rackBackM; }
            if (rackMax_ >= p.rackDoneM) {
                step_ = XStep::Cocked;
                o.ev = XEvent::Catch;
                o.haptic = true;
            } else if (!in.gripDown) {
                step_ = XStep::Empty;
                o.ev = XEvent::RackLetGo;
            }
            break;
        case XStep::Cocked:
            // The take is the grip EDGE inside the zone: a grip held on from the rack, or held on after an early release, never
            // takes again until a new press (test OnePressOneTake).
            if (edge && in.inShoulder) {
                if (in.spare > 0) {
                    step_ = XStep::Carry;
                    o.ev = XEvent::Take;
                    o.haptic = true;
                    inRange_ = false;
                } else {
                    o.ev = XEvent::TakeNoBolt;
                }
            }
            break;
        case XStep::Carry: {
            if (!in.gripDown) {
                step_ = XStep::Cocked;
                o.ev = XEvent::EarlyRelease;
                inRange_ = false;
                break;
            }
            const bool inRange = in.nockToLineM <= p.catchM;
            if (inRange && in.dirDeg <= p.catchDeg) {
                step_ = XStep::Laid;
                o.ev = XEvent::Lay;
            } else if (inRange && !inRange_) {
                o.ev = XEvent::LayRefusedAngle;   // ONCE per entry into range; re-armed when it leaves
            }
            inRange_ = inRange;
            break;
        }
        case XStep::Laid:
            if (!in.gripDown) {
                step_ = XStep::Cocked;
                o.ev = XEvent::EarlyRelease;
            } else if (in.nockToLineM > p.holdM) {
                step_ = XStep::Carry;
                o.ev = XEvent::Unlay;
                inRange_ = false;
            } else if (in.nockToSeatM <= p.seatM) {
                step_ = XStep::Idle;
                o.ev = XEvent::Seat;
                o.haptic = true;
                o.callAddBullet = true;
                rackMax_ = 0;
            }
            break;
        }
    }

    XStep step_ = XStep::Idle;
    bool  gripWas_ = false;
    bool  inRange_ = false;
    float rackMax_ = 0;
    int   settle_ = 0;
};

}  // namespace reload
}  // namespace dlvr
