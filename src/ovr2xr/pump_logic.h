// pump_logic.h - MANUAL RELOAD of the TUBE-LOADED SHOTGUNS: the pumps (wn_shotgun_b / wn_shotgun_f) and, with PParams::racks false,
// the semi-auto wn_shotgun_c (2026-11-07, Jay "No rack": the same take / push, no RACK step) - the hand steps (pure, unit-tested:
// src/src/tests/pump_logic_test.cpp). No game includes, no allocation.
// Plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 1; spec docs/superpowers/specs/2026-11-04-manual-reload-pump-shotgun-design.md.
// The pump's analogue of xbow_logic.h: this decides the steps, the driver (reload.cpp) draws the parts, withholds R2 and calls the
// game's own add-bullet when callAddBullet is set. The round count is the game's; CHAMBERED is ours (plan ruling P1).
//   TAKE   the rising edge of (grip held AND inHip) draws min(handMax, spare, capacity - rounds) shells into the hand (ledger ruling R1:
//          the pouch flag rises after the press, so a plain grip edge tested against inHip would miss it). The next load cycle of
//          the animation is handMax - n + 1 (a handful of n < 4 starts late, plan ruling P4).
//   PUSH   the hand entering the port (inPort), armed, holding shells, no cycle playing: one load cycle of cycleFrames in kPumpCycleS,
//          timed by frameNow; the add-bullet fires ONCE at animFrame >= addFrame whatever the hand does after the push. Re-armed
//          only by an update with outOfPort true.
//   RACK   grippedPump: the stroke's max reaching rackDoneFrac -> RackBack; then back under 1 - rackDoneFrac -> RackForward (the tube
//          shell moves up: chambered iff rounds > 0 -> reported as Chambered); released short -> RackShort.
// Several things can land in ONE update (a shot + the add-bullet frame, a take + a shot, a rack end + a shot). Their state changes all
// happen (the edges are consumed), so NO effect is dropped: `ev` is the FIRST event raised (order shot / cycle / clear / take / push /
// rack) for logging, `events` has a bit (1u << PEvent) for EVERY event raised, `fx[0..nFx)` every sound raised in order, and `haptic`
// is the OR of every pulse. The driver logs from `events` and plays from `fx` / `haptic`.
// A shot is a round count below (previous count + add-bullets this machine requested since the last update), so a shot that lands
// together with our own add-bullet is still seen (ruling R7). A fresh draw (freshDraw) drops the hand and any cycle at once, and sets
// chambered = (rounds > 0).
#pragma once

#include "reload_logic.h"   // Fx, kSettleUpdates

namespace dlvr {
namespace reload {

enum class PEvent { None, Take, TakeNoShell, PortPush, PortFull, ShellIn, Shot, RackBack, RackForward, RackShort, Chambered, HandCleared };

// rackDoneFrac = ReloadRackPullM / kPistolSlideTravelM (the driver fills it from the live value; no new settings key).
// capacity + racks = the gun's ROW (reload_pump_table.inc; the driver fills both). racks false (2026-11-07, the semi-auto wn_shotgun_c,
// Jay: "No rack"): the gun cycles itself - CHAMBERED is (rounds + this update's add-bullets) > 0 on EVERY update (a shot, a shell in,
// a fresh draw alike) and the rack stroke is never read (no RackBack / RackForward / Chambered / RackShort events).
struct PParams { int capacity = 6; int handMax = 4; float rackDoneFrac = 0.815f; int cycleFrames = 36; int addFrame = 23; bool racks = true; };

struct PInput {
    bool  manualOn = false;
    bool  isPump = false;
    bool  freshDraw = false;     // the pump was just drawn / switched to
    bool  gripDown = false;      // LEFT grip, already debounced
    bool  inHip = false;         // left hand inside the hip pouch zone (the flag lags the press by 8-9 ms, INV-673)
    bool  inPort = false;        // the hand's shell point inside the port sphere (the push signal)
    bool  outOfPort = false;     // beyond 1.5 x the port radius (the re-arm signal)
    bool  grippedPump = false;   // the two-hand grip is on the pump (the rack hold)
    int   rounds = 0, spare = 0;
    float strokeFrac = 0;        // 0 forward .. 1 fully back
    int   frameNow = 0;          // the frame counter the driver advances (PumpFrameAt) (may jump by more than 1 between updates)
};

struct POut {
    PEvent ev = PEvent::None;           // the FIRST event raised this update (logging); see `events` for all of them
    unsigned events = 0;                // bit (1u << PEvent) for EVERY event raised this update
    bool   withholdR2 = false;          // !chambered
    bool   gameReloadAllowed = true;    // false whenever manual + pump
    bool   chambered = false;
    bool   callAddBullet = false;       // call the game's add-bullet ONCE, this update
    bool   haptic = false;              // one short pulse on the left controller (OR of every event's pulse)
    int    handShells = 0;              // shells drawn in the hand
    int    animCycle = 0;               // 0 = none, 1-4 = playing
    int    animFrame = 0;               // 1..cycleFrames within the cycle (0 when none)
    bool   pumpFollowsHand = false;     // grippedPump
    Fx     fx[4] = { Fx::None, Fx::None, Fx::None, Fx::None };   // every sound raised this update, in order (at most 4 sources)
    int    nFx = 0;
    bool Has(PEvent e) const { return (events & (1u << static_cast<unsigned>(e))) != 0; }
    bool HasFx(Fx f) const { for (int k = 0; k < nFx; ++k) { if (fx[k] == f) { return true; } } return false; }
};

// THE LOAD CYCLE's FRAME CLOCK (PS1 fixes; Jay after RUN PS1: "0.3 s per shell (4x)"): one 36-frame C13 cycle plays in kPumpCycleS
// seconds (was 1.2 s = 30 fps). frameNow = elapsed ms x cycleFrames / (cycleS x 1000), so the add-bullet (cycle frame 23) lands
// 22 / 36 x 0.3 = 0.183 s after the push. Pure; a multiply and a divide.
constexpr float kPumpCycleS = 0.3f;
inline int PumpFrameAt(unsigned long long elapsedMs, float cycleS, int cycleFrames)
{
    const unsigned long long cycleMs = static_cast<unsigned long long>(cycleS * 1000.0f + 0.5f);
    if (cycleMs == 0 || cycleFrames <= 0) { return 0; }
    return static_cast<int>(elapsedMs * static_cast<unsigned long long>(cycleFrames) / cycleMs);
}

// THE RACK STROKE (final fix wave: final review I1 / #19, ledger ruling R17). `s` = (left controller - right controller) . the pump's BACK
// axis, both controllers in the SAME tracking space (the driver: boneblend.cpp ReloadNoteController), so locomotion, turning and moving
// the gun with both hands cancel. The stroke = s - s0, s0 latched at the first FRESH sample of the grip, clamped 0 .. travelM. A sample
// that is not fresh (either controller stale) HOLDS the last stroke - it never reads as 0 or full, so staleness can raise neither a
// RACK BACK nor a CHAMBERED. Not gripping: 0, and the next grip latches afresh. Pure; a few compares per update.
struct PStroke {
    bool  latched = false;
    float s0 = 0.0f;
    float last = 0.0f;   // metres
    float Update(bool gripped, bool fresh, float s, float travelM)
    {
        if (!gripped) { latched = false; last = 0.0f; return 0.0f; }
        if (!fresh) { return last; }
        if (!latched) { latched = true; s0 = s; }
        float m = s - s0;
        m = m < 0.0f ? 0.0f : (m > travelM ? travelM : m);
        last = m;
        return m;
    }
};

class PMachine {
public:
    POut Update(const PInput& in, const PParams& p)
    {
        const bool takeNow = in.gripDown && in.inHip;
        const bool takeEdge = takeNow && !takeWas_;
        takeWas_ = takeNow;
        if (!in.manualOn || !in.isPump) {
            Reset();
            prevRounds_ = -1;
            return POut{};
        }
        POut o{};
        o.gameReloadAllowed = false;
        if (prevRounds_ < 0) { prevRounds_ = in.rounds; }

        if (in.freshDraw) {
            // Put away / switched / redrawn: every drawn shell is gone at once, no add-bullet after (spares untouched).
            const bool had = handShells_ > 0 || cycleActive_;
            chambered_ = in.rounds > 0;
            handShells_ = 0;
            cycleActive_ = false;
            armed_ = true;
            ResetStroke();
            prevRounds_ = in.rounds;
            addsReq_ = 0;
            if (had) { o.ev = PEvent::HandCleared; }
            return Fill(o, in);
        }

        // Shot: the round count drops by one while chambered; the gun is then not chambered (the cycle, if any, plays on).
        // Our own add-bullets since the last update are expected to have raised the count (ruling R7), so a shot that lands together
        // with one still shows as a count below prev + adds.
        if (chambered_ && in.rounds < prevRounds_ + addsReq_) {
            chambered_ = false;
            Raise(o, PEvent::Shot, false, Fx::None);
        }
        prevRounds_ = in.rounds;
        addsReq_ = 0;

        // A load cycle, timed by the frame counter.
        if (cycleActive_) {
            const int af = in.frameNow - startFrame_ + 1;
            if (!shellFired_ && af >= p.addFrame) {
                shellFired_ = true;
                if (handShells_ > 0) { --handShells_; }
                o.callAddBullet = true;
                ++addsReq_;
                Raise(o, PEvent::ShellIn, true, Fx::Seat);
            }
            if (af > p.cycleFrames) {
                cycleActive_ = false;
            } else {
                o.animCycle = cycle_;
                o.animFrame = af < 1 ? 1 : af;
            }
        }

        // Grip up while holding shells and no cycle playing: the hand is empty.
        if (!in.gripDown && handShells_ > 0 && !cycleActive_) {
            handShells_ = 0;
            Raise(o, PEvent::HandCleared, false, Fx::None);
        }

        // The hip take.
        if (takeEdge && handShells_ == 0 && !cycleActive_) {
            int n = p.handMax;
            if (in.spare < n) { n = in.spare; }
            if (p.capacity - in.rounds < n) { n = p.capacity - in.rounds; }
            if (n > 0) {
                handShells_ = n;
                nextCycle_ = p.handMax - n + 1;
                Raise(o, PEvent::Take, true, Fx::PouchTake);
            } else {
                Raise(o, PEvent::TakeNoShell, false, Fx::None);
            }
        }

        // The port push.
        if (in.outOfPort) { armed_ = true; }
        if (in.inPort && armed_) {
            armed_ = false;   // disarmed on EVERY entry; only an outOfPort update re-arms (a lingering hand never pushes twice)
            if (handShells_ > 0 && !cycleActive_) {
                const int pending = (cycleActive_ && !shellFired_) ? 1 : 0;
                if (in.rounds + pending < p.capacity) {
                    cycleActive_ = true;
                    shellFired_ = false;
                    startFrame_ = in.frameNow;
                    cycle_ = nextCycle_;
                    ++nextCycle_;
                    o.animCycle = cycle_;
                    o.animFrame = 1;
                    Raise(o, PEvent::PortPush, false, Fx::None);
                } else {
                    Raise(o, PEvent::PortFull, false, Fx::None);
                }
            }
        }

        // No rack (2026-11-07): the gun chambers itself from the tube - chambered whenever a round is in it or one was added this update
        // (the add raises the count only by the next update). The stroke is not read (the driver never reports a grip on a no-rack gun).
        if (!p.racks) {
            chambered_ = in.rounds + addsReq_ > 0;
            ResetStroke();
            return Fill(o, in);
        }

        // The rack stroke.
        if (in.grippedPump) {
            if (!stroking_) { stroking_ = true; strokeMax_ = 0; backFired_ = false; }
            if (in.strokeFrac > strokeMax_) { strokeMax_ = in.strokeFrac; }
            if (!backFired_ && strokeMax_ >= p.rackDoneFrac) {
                backFired_ = true;
                Raise(o, PEvent::RackBack, true, Fx::RackGrab);
            } else if (backFired_ && in.strokeFrac <= 1.0f - p.rackDoneFrac) {
                // The stroke is complete: the tube shell moves up iff there is one. Racking a chambered gun loses nothing.
                if (in.rounds > 0) { chambered_ = true; }
                Raise(o, in.rounds > 0 ? PEvent::Chambered : PEvent::RackForward, true, Fx::RackDone);
                strokeMax_ = 0;       // a further stroke on the same hold counts afresh
                backFired_ = false;
            }
        } else if (stroking_) {
            if (strokeMax_ > 0.0f) { Raise(o, PEvent::RackShort, false, Fx::None); }
            ResetStroke();
        }

        return Fill(o, in);
    }
    // Task 4 review M6 (ruling R15): the driver's add-bullet call for this update's callAddBullet FAULTED - the game added nothing, so it
    // is not counted as requested (else the next update's unchanged count reads as a false Shot and un-chambers). Call it right after
    // the Update that asked. Never below zero. Test FaultedAddIsNotCounted.
    void AddFailed() { if (addsReq_ > 0) { --addsReq_; } }
    // PS2 (Jay: "No snap until ready"): an entry into the port NOW would PortPush - armed, shells in the hand, no cycle playing, room
    // in the tube (the same tests as the push above). The driver snaps (lock + buzz) only when this holds. Test CanPushOnlyWhenAPushWouldLoad.
    bool CanPush(int rounds, const PParams& p = PParams{}) const
    {
        return armed_ && handShells_ > 0 && !cycleActive_ && rounds < p.capacity;
    }
    bool chambered() const { return chambered_; }
    int  handShells() const { return handShells_; }

private:
    // Every event raised is recorded: ev keeps the first, events the bit of each, haptic ORs, fx appends.
    static void Raise(POut& o, PEvent e, bool haptic, Fx fx)
    {
        if (o.ev == PEvent::None) { o.ev = e; }
        o.events |= 1u << static_cast<unsigned>(e);
        if (haptic) { o.haptic = true; }
        if (fx != Fx::None && o.nFx < 4) { o.fx[o.nFx++] = fx; }
    }
    POut& Fill(POut& o, const PInput& in) const
    {
        o.chambered = chambered_;
        o.withholdR2 = !chambered_;
        o.handShells = handShells_;
        o.pumpFollowsHand = in.grippedPump;
        return o;
    }
    void ResetStroke() { stroking_ = false; strokeMax_ = 0; backFired_ = false; }
    void Reset()
    {
        chambered_ = false;
        handShells_ = 0;
        cycleActive_ = false;
        shellFired_ = false;
        armed_ = true;
        nextCycle_ = 1;
        cycle_ = 0;
        startFrame_ = 0;
        addsReq_ = 0;
        ResetStroke();
    }

    bool  chambered_ = false;
    bool  takeWas_ = false;
    bool  armed_ = true;
    bool  cycleActive_ = false;
    bool  shellFired_ = false;
    bool  stroking_ = false;
    bool  backFired_ = false;
    int   handShells_ = 0;
    int   nextCycle_ = 1;
    int   cycle_ = 0;
    int   startFrame_ = 0;
    int   prevRounds_ = -1;
    int   addsReq_ = 0;      // add-bullets requested since the last update (they raise the round count by 1 each)
    float strokeMax_ = 0;
};

}  // namespace reload
}  // namespace dlvr
