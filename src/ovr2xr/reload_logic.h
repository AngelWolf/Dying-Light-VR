// reload_logic.h - MANUAL RELOAD, the hand steps (pure, unit-tested: src/src/tests/reload_logic_test.cpp). No game includes.
// Spec docs/superpowers/specs/2026-11-01-manual-reload-design.md; plan docs/superpowers/plans/2026-11-01-manual-reload-pistol.md
// Task 2 (+ Task 1 rulings, RESEARCH-MANUAL-RELOAD §18: a FIRED-EMPTY reload adds its rounds at the RACK - the game draws the slide
// locked back while the clip is 0 and forward once it is not; a TACTICAL reload adds them at the seat). 2026-11-02 (Jay): the PISTOL
// has no rack - a fired-empty pistol adds its rounds at the SEAT (the slide goes forward on its own); the SMG keeps the rack.
// The game's own reload is never started for a manual gun: this decides the steps, the driver (reload.cpp) draws the parts, withholds
// R2 and calls the game's own add-bullet +DAF160 when callAddBullet is set. One Machine per gun ITEM (a switch keeps its state).
#pragma once

namespace dlvr {
namespace reload {

struct Vec3 { float x = 0, y = 0, z = 0; };
// None = not a built family: Machine stays Idle and the game reloads it as today. Smg (plan 2026-11-01-manual-reload-smg) runs
// exactly the pistol's steps; only the driver differs (spots, rack length, the drawn bolt - reload_guns.h). Rifle (plan
// 2026-11-03-manual-reload-rifle) is the same again: a table gun whose mag bone is bone_clip; whether it racks is its row's `racks`.
// Crossbow (plan 2026-11-03-manual-reload-crossbow Task 2): its own row (reload_guns.h XbowRow) and its own steps (xbow_logic.h
// XMachine) - NOT a table gun and never run through Machine below (the driver, Task 5, routes it).
// Pump (plan 2026-11-04-manual-reload-pump-shotgun Task 4): wn_shotgun_b / f, its own row (reload_guns.h PumpRow) and steps
// (pump_logic.h PMachine) - NOT a table gun, never run through Machine below (reload.cpp DrivePump routes it).
// DBarrel: wn_shotgun_a (dbarrel_logic.h, 2026-11-07) + the single-shot (its own set, 2026-11-09). 2026-11-09 (plan
// 2026-11-09-manual-reload-remaining-guns Task 8): Break6 = the Schofield (break6_logic.h), Gate = the Sixshooter / Walker (gate_logic.h),
// Fuel = the chainsaw gun (fuel_logic.h).
enum class Family { None, Pistol, Smg, Rifle, Crossbow, Pump, DBarrel, Break6, Gate, Fuel };
// The families that have a per-gun row in reload_guns_table.inc (spots, travel, mag bone): the SMG and the rifles. Never a racking test.
inline bool IsTableGun(Family f) { return f == Family::Smg || f == Family::Rifle; }

// WHO OWNS A LEFT GRIP PRESS - the reload or the two-handed hold (rifle plan Task 5; Task 0 U-d: TwoHandPoll and ReloadPadPoll did
// not check each other, so one press at a rifle's mag could start both). Decided once, at the press edge:
//   - the reload owns it while a step is live (not Idle) OR the hand is in a reload grab zone (the mag spot within reload_grab_m,
//     or the rack handle within the rack grab radius while NeedsRack) - the same distance tests the machine grabs with;
//   - the two-handed hold may start only on a press nobody else owns (a left holster, or the reload).
// After a reload the two-handed grip comes back only on a NEW press at the front grip - no automatic re-grip (ruling, progress.md).
// Pure; two bool ops - safe per poll.
inline bool ReloadOwnsLeftPress(bool stepLive, bool inReloadGrabZone) { return stepLive || inReloadGrabZone; }
inline bool TwoHandPressRefused(bool holsterOwnsPress, bool reloadOwnsPress) { return holsterOwnsPress || reloadOwnsPress; }
enum class Step { Idle, Pulling, MagInHand, Empty, FreshInHand, NeedsRack, Racking };
enum class Event { None, Grab, PullOut, Drop, PouchTake, PouchEmpty, SnapBack, Seat, RackPull, RackDone, Done };
// What the player just DID this update - the sound and the haptic pulse hang off it (solid guns plan Task 3 / 5).
// Cock (2026-11-10): the revolver fan-cock's hammer sound - only the revolver driver plays it (RvCock maps CMachine's RackDone to it).
enum class Fx { None, MagGrab, MagOut, PouchTake, Seat, RackGrab, RackDone, Cock };

// RUN RL2B: the game does not always re-pose bone_magazine after we stop drawing it - the mag stayed where the hand left it. After a
// step that drew the mag ends with the mag back IN the gun (Done / Seat / SnapBack), the driver draws it at its seat for this many
// updates (~0.3 s at ~90 per s), so what stays drawn is the seated mag.
constexpr int kSettleUpdates = 30;

// reload_snap_m / reload_align_deg / reload_pull_out_m / reload_rack_pull_m / reload_grab_m (Tier D); rackGrabM = the slide / handle
// grab's radius (the driver: reload_guns.h RackGrabFor - the pistol 0.12, else reload_grab_m).
struct Params { float snapM = 0.05f, alignDeg = 45, pullOutM = 0.06f, rackPullM = 0.03f, grabM = 0.08f, rackGrabM = 0.08f; };

struct Input {
    Family family = Family::None;
    bool   racks = false;           // the gun's ROW says it racks (reload_fx.h; Jay 2026-11-02: racking is per gun, not per family)
    bool   manualOn = false;
    bool   gripDown = false;        // LEFT grip, already debounced
    bool   leftInPouch = false;     // left hand inside the left-hip zone
    float  handToMagM = 1e9f;       // left grip -> the mag's current world origin
    float  pullFromGrabM = 0;       // left grip -> where it grabbed the mag, in ANY direction (the mag is in the hand from the grab)
    float  handToWellM = 1e9f;      // held fresh mag -> the seat
    float  alignDeg = 180;          // held mag's well axis vs the gun's
    float  handToSlideM = 1e9f;     // left grip -> bone_chamber
    float  slideBackM = 0;          // hand travel back along the slide axis since the rack grab (+ = back)
    int    rounds = 0, maxRounds = 0, spare = 0;
};

struct Out {
    Step  step = Step::Idle;
    Event ev = Event::None;
    bool  withholdR2 = false;         // the gun cannot fire: every step but Idle
    bool  gameReloadAllowed = true;   // false -> the game's reload start +DA6E00 is refused for this gun
    bool  magFollowsHand = false;     // draw bone_magazine at the left hand
    bool  magHidden = false;          // bone_magazine not drawn (dropped, nothing taken yet)
    bool  slideFollowsHand = false;   // draw bone_chamber at the hand's travel
    bool  callAddBullet = false;      // call +DAF160 ONCE, this update
    bool  haptic = false;             // one short pulse on the left controller
    Fx    fx = Fx::None;              // the player's action this update (one per update; from ev)
    bool  magAtSeat = false;          // draw bone_magazine at its SEAT (kSettleUpdates updates after a drawn step ends in the gun)
    // The bolt after a rack (kSettleUpdates updates; the driver acts on these for an Smg only - the pistol's slide is never written):
    bool  slideAtRest = false;        // after RackDone: draw bone_chamber at its rest (the bolt has gone forward)
    bool  slideAtGrab = false;        // after a rack let go short: draw it where it was when grabbed
};

class Machine {
public:
    Out Update(const Input& in, const Params& p)
    {
        const bool edge = in.gripDown && !gripWas_;
        gripWas_ = in.gripDown;
        // The pouch press = the moment "grip held AND in the pouch" becomes true - NOT the grip's own edge: the holster raises the
        // pouch flag only after its hip yield (8-9 ms, RUN RL2), on a later update than the grip edge (test
        // PouchFlagAfterTheGripEdgeTakes). One press = one take / one buzz (test OnePouchPressOneBuzz).
        const bool pouchPress = in.gripDown && in.leftInPouch;
        const bool pouchEdge = pouchPress && !pouchWas_;
        pouchWas_ = pouchPress;
        if (!in.manualOn || in.family == Family::None) {
            step_ = Step::Idle;
            firedEmpty_ = false;
            rackMax_ = 0;
            settle_ = 0;
            slideSettle_ = 0;
            return Out{};
        }
        const Step before = step_;
        Out o{};
        o.gameReloadAllowed = false;
        switch (step_) {
        case Step::Idle:
            if (edge && in.handToMagM <= p.grabM) { step_ = Step::Pulling; o.ev = Event::Grab; }
            break;
        case Step::Pulling:
            if (!in.gripDown) {
                step_ = Step::Idle;
                o.ev = Event::SnapBack;
            } else if (in.pullFromGrabM >= p.pullOutM) {
                step_ = Step::MagInHand;
                o.ev = Event::PullOut;
                firedEmpty_ = in.rounds == 0;
            }
            break;
        case Step::MagInHand:
            if (!in.gripDown) { step_ = Step::Empty; o.ev = Event::Drop; }
            break;
        case Step::Empty:
            if (pouchEdge) {
                o.haptic = true;
                // Jay (RL2B, "Give back if rounds left"): the game still counts the rounds left in the gun, so a pulled mag with
                // rounds in it comes back even with 0 spare (test TacticalNoSpareGivesTheMagBack). Only 0 rounds AND 0 spare = nothing.
                if (in.spare > 0 || in.rounds > 0) { step_ = Step::FreshInHand; o.ev = Event::PouchTake; }
                else { o.ev = Event::PouchEmpty; }
            }
            break;
        case Step::FreshInHand:
            if (!in.gripDown) {
                step_ = Step::Empty;
                o.ev = Event::Drop;
            } else if (in.handToWellM <= p.snapM && in.alignDeg <= p.alignDeg) {
                o.haptic = true;
                // Fired empty: the SMG needs its rack; the PISTOL does not - the slide goes forward on its own at the seat (Jay
                // 2026-11-02, video e5xA-XflXmw 2:33-2:37: "Slide is automatic on modern pistols" -> "Yes, no pistol rack"), so its
                // rounds go in here and the game draws the slide forward after the add-bullet.
                if (firedEmpty_ && in.racks) {
                    step_ = Step::NeedsRack;
                    o.ev = Event::Seat;
                } else {
                    firedEmpty_ = false;
                    step_ = Step::Idle;
                    o.ev = Event::Done;
                    o.callAddBullet = true;
                }
            }
            break;
        case Step::NeedsRack:
            if (edge && in.handToSlideM <= p.rackGrabM) { step_ = Step::Racking; o.ev = Event::RackPull; rackMax_ = 0; }
            break;
        case Step::Racking:
            // Jay after RUN HP2 ("Lets just try it with the free release on the rack first"): done as soon as the pull reaches the
            // rack-done distance, the grip still held - the hand is free at once. Let go short = back to NeedsRack, as before.
            if (in.slideBackM > rackMax_) { rackMax_ = in.slideBackM; }
            if (rackMax_ >= p.rackPullM) {
                step_ = Step::Idle;
                o.ev = Event::RackDone;
                o.callAddBullet = true;
                o.haptic = true;
                firedEmpty_ = false;
            } else if (!in.gripDown) {
                step_ = Step::NeedsRack;
            }
            break;
        }
        o.step = step_;
        switch (o.ev) {
        case Event::Grab: o.fx = Fx::MagGrab; break;
        case Event::PullOut: o.fx = Fx::MagOut; break;
        case Event::PouchTake: o.fx = Fx::PouchTake; break;
        case Event::Seat:
        case Event::Done: o.fx = Fx::Seat; break;
        case Event::RackPull: o.fx = Fx::RackGrab; break;
        case Event::RackDone: o.fx = Fx::RackDone; break;
        default: break;
        }
        o.withholdR2 = step_ != Step::Idle;
        o.magFollowsHand = step_ == Step::Pulling || step_ == Step::MagInHand || step_ == Step::FreshInHand;
        o.magHidden = step_ == Step::Empty;
        o.slideFollowsHand = step_ == Step::Racking;
        const bool drewBefore = before == Step::Pulling || before == Step::MagInHand || before == Step::FreshInHand;
        if (drewBefore && (o.ev == Event::Done || o.ev == Event::Seat || o.ev == Event::SnapBack)) { settle_ = kSettleUpdates; }
        if (o.magFollowsHand || o.magHidden) { settle_ = 0; }
        o.magAtSeat = settle_ > 0;
        if (settle_ > 0) { --settle_; }
        if (o.ev == Event::RackDone) { slideSettle_ = kSettleUpdates; slideToRest_ = true; }
        else if (before == Step::Racking && step_ == Step::NeedsRack) { slideSettle_ = kSettleUpdates; slideToRest_ = false; }
        if (o.slideFollowsHand) { slideSettle_ = 0; }
        o.slideAtRest = slideSettle_ > 0 && slideToRest_;
        o.slideAtGrab = slideSettle_ > 0 && !slideToRest_;
        if (slideSettle_ > 0) { --slideSettle_; }
        return o;
    }
    Step step() const { return step_; }
    bool firedEmpty() const { return firedEmpty_; }

private:
    Step  step_ = Step::Idle;
    bool  gripWas_ = false;
    bool  pouchWas_ = false;
    bool  firedEmpty_ = false;
    float rackMax_ = 0;
    int   settle_ = 0;
    int   slideSettle_ = 0;
    bool  slideToRest_ = false;
};

}  // namespace reload
}  // namespace dlvr
