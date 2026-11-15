// gate_logic.h - THE SIXSHOOTER / WALKER loading gate as a pure step machine (unit-tested: src/src/tests/gate_logic_test.cpp). Plan
// docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 5; spec 2026-11-09-manual-reload-remaining-guns-design.md 3.5.
// Jay 2026-11-09, every rule below:
//   GATE   "Flick open the gate too, has to be a flick to the right ... the player will need to have the gun sideways to reload it, with
//          the gate facing up, so we'd need a harder flick to close the gate" -> "Yes, flick left to close" -> "use a multiplier, no new
//          key ... just a bit more than currently" (RG3 / RG4: rates on the gun - SUPERSEDED by RG5, the controller's twist, below).
//   CASES  each spent case pops out by itself as its chamber comes to the gate (the cylinder turns to the next chamber not loaded).
//   ROUNDS "same story with bullets in hand": one hip take holds min(chambers not loaded, spare); ONE PUSH per entry at the gate loads
//          the chamber there (the Schofield's push animation), then the cylinder clicks on (Index) to the next chamber not loaded.
//   CLOSE  closing the gate after a round went in = ReloadDone (the fan-cock, cock_logic.h).
// The game counts rounds; the six chambers and the cylinder index are this machine's (reconciled to the count while the gate is shut).
// No game access, no logging, no allocation. Times in seconds.
#pragma once

#include "dbarrel_logic.h"   // DChamber

namespace dlvr {
namespace reload {

// RUN RG5 (Jay: "The gate keeps not working" -> "The flick for the gate is a wrist rotation, very quick" -> "Controller twist"): the flick
// is the RIGHT CONTROLLER's twist (twist_logic.h - the RG3 / RG4 rate rule on the gun's bone_root missed: its world updates every second
// hook call). OPEN = a twist RIGHT of kGateOpenTwistDeg within kGateTwistWindowS; CLOSE = a twist LEFT of kGateCloseTwistDeg (a bit harder -
// Jay RG3: "a harder flick to close"). Still only with the gun ON ITS SIDE (GInput::onSide - RG3, "Sideways controller rotation,
// remember") and the RG4 lockout (Jay: "Yes, build both"): after a toggle nothing counts for kGateLockoutS, and the driver clears the twist
// window (GOut::toggled) so one twist never counts twice.
// RG9 (Jay: "The swing for the gate is still hard to do" -> "I'm saying it's difficult to get OPEN AND CLOSED"): his quick twists
// measured 30-41 deg (24 misses "too small" at 45) - both directions now 30.
constexpr float kGateOpenTwistDeg = 30.0f;
// RG6: 60 -> 45 (Jay's left twists read 47-55 deg and missed; the 0.4 s lockout is the rebound guard) - "You can fix the gate now"
constexpr float kGateCloseTwistDeg = 30.0f;
constexpr double kGateTwistWindowS = 0.15;
constexpr float kGateLockoutS = 0.4f;

// Review N4 (2026-11-10; Jay: "Fix N3, N4" -> "we'll go with 1"): the gate OPENS only when the gun can load. A full gun (six rounds) =
// the twist is refused (the driver's GATE TWIST MISSED names this reason). The close is unchanged. One rule shared by GMachine and the
// driver's miss line. RUN SK1 (Jay: "wasn't able to open the gate at all ... till near the end"): the spare half (spare > 0) refused 52
// twists - the driver's spare is -1 until a hip take refreshes it (reload.cpp ItemState::spare, read only at the take edge / after an
// add) -> Jay: "Drop the spare check". Full gun only.
inline bool GateCanLoad(int rounds) { return rounds < 6; }

enum class GEvent {
    None, GateOpen, GateClose, CaseOut, Take, TakeNoRound, LoadStart, RoundIn, Index, HandCleared, ReloadDone, Shot, Count
};
enum class GFx { None, Open, Seat, Close, Click };

struct GParams {
    float openTwistDeg = kGateOpenTwistDeg;
    float closeTwistDeg = kGateCloseTwistDeg;
    float leadS = 0.10f;
    int   loadFrames = 7;        // the Schofield's / single-shot's push at 60 fps
    int   seatFrame = 7;
    int   fps = 60;
};

struct GInput {
    bool   manualOn = false, freshDraw = false;
    bool   gripDown = false, inHip = false;
    bool   nearLoad = false, farLoad = false;   // the palm at the gate's chamber mouth / beyond 1.5 x its ball
    float  twistRightDeg = 0.0f, twistLeftDeg = 0.0f;   // RG5: the controller's peak twist right / left in the window (twist_logic.h)
    bool   onSide = false;                              // RG3: the gun on its side, the gate facing up (the driver's test)
    int    rounds = 0, spare = 0;
    double nowS = 0.0;
};

struct GOut {
    unsigned events = 0;
    GEvent   ev = GEvent::None;
    GFx      fx[4] = {};
    int      nFx = 0;
    bool     haptic = false;
    bool     callAddBullet = false;
    int      addChamber = -1;
    bool     withholdR2 = false;
    bool     gateOpen = false;
    int      cylinderIndex = 0;     // the chamber at the gate (the driver turns bone_cylinder 60 deg per index)
    DChamber ch[6] = {};
    int      hand = 0;
    bool     loading = false;
    float    loadFrame = 1.0f;
    bool     locked = false;
    bool     toggled = false;       // RG5: the gate opened / closed this update - the driver clears the twist window
    bool Has(GEvent e) const { return (events & (1u << static_cast<unsigned>(e))) != 0; }
};

class GMachine {
public:
    GOut Update(const GInput& in, const GParams& p)
    {
        const bool takeNow = in.gripDown && in.inHip;
        const bool takeEdge = takeNow && !takeWas_;   // the pump's R1 edge (INV-673)
        takeWas_ = takeNow;
        if (!in.manualOn) {
            *this = GMachine{};
            return GOut{};
        }
        GOut o{};
        nowS_ = in.nowS;
        if (prevRounds_ < 0 || in.freshDraw) {
            const bool had = hand_ != 0 && prevRounds_ >= 0;
            const int keepIdx = idx_;
            *this = GMachine{};
            takeWas_ = takeNow;
            idx_ = keepIdx;
            for (int s = 0; s < 6; ++s) { ch_[s] = s < in.rounds ? DChamber::Live : DChamber::Spent; }
            prevRounds_ = in.rounds;
            if (had) { Raise(o, GEvent::HandCleared, false, GFx::None); }
            return Fill(o, p);
        }
        if (in.farLoad) { loadArmed_ = true; }
        const int expected = prevRounds_ + addsReq_;
        if (!open_ && in.rounds < expected) {
            for (int k = in.rounds; k < expected; ++k) {
                for (int s = 0; s < 6; ++s) {
                    const int c = (idx_ + s) % 6;
                    if (ch_[c] == DChamber::Live) { ch_[c] = DChamber::Spent; break; }
                }
            }
            Raise(o, GEvent::Shot, false, GFx::None);
        }
        prevRounds_ = in.rounds;
        addsReq_ = 0;
        if (!open_) { Reconcile(in.rounds); }
        if (!in.gripDown && hand_ != 0 && !loading_) {
            Raise(o, GEvent::HandCleared, false, GFx::None);
            hand_ = 0;
        }
        // RG4 lockout after a toggle; RG5 the flick = the controller's twist in its window, the gun on its side
        const bool flickOk = in.nowS >= lockUntil_ && in.onSide;
        if (!open_) {
            if (flickOk && in.twistRightDeg >= p.openTwistDeg && GateCanLoad(in.rounds)) {   // N4
                open_ = true;
                loaded_ = false;
                lockUntil_ = in.nowS + kGateLockoutS;
                o.toggled = true;
                Raise(o, GEvent::GateOpen, true, GFx::Open);
                Advance(o);
            }
        } else if (loading_) {
            const float f = LoadFrame(static_cast<float>(in.nowS - t0_) - p.leadS, p);
            if (!seated_ && f >= static_cast<float>(p.seatFrame)) {
                seated_ = true;
                --hand_;
                ch_[idx_] = DChamber::Live;
                loaded_ = true;
                ++addsReq_;
                o.callAddBullet = true;
                o.addChamber = idx_;
                Raise(o, GEvent::RoundIn, true, GFx::Seat);
            }
            if (seated_ && f >= static_cast<float>(p.loadFrames)) {
                loading_ = false;
                Raise(o, GEvent::Index, false, GFx::Click);
                idx_ = (idx_ + 1) % 6;
                Advance(o);
            }
        } else {
            if (flickOk && in.twistLeftDeg >= p.closeTwistDeg) {
                open_ = false;
                lockUntil_ = in.nowS + kGateLockoutS;
                o.toggled = true;
                if (hand_ != 0) { Raise(o, GEvent::HandCleared, false, GFx::None); hand_ = 0; }
                Raise(o, GEvent::GateClose, true, GFx::Close);
                if (loaded_) { Raise(o, GEvent::ReloadDone, false, GFx::None); }
                loaded_ = false;
                return Fill(o, p);
            }
            if (takeEdge && hand_ == 0) {
                int n = 0;
                for (DChamber c : ch_) { n += c != DChamber::Live ? 1 : 0; }
                if (in.spare < n) { n = in.spare; }
                if (n > 0) {
                    hand_ = n;
                    Raise(o, GEvent::Take, true, GFx::None);
                } else {
                    Raise(o, GEvent::TakeNoRound, false, GFx::None);
                }
            }
            if (in.gripDown && in.nearLoad && loadArmed_ && hand_ != 0 && ch_[idx_] == DChamber::Empty) {
                loadArmed_ = false;
                loading_ = true;
                seated_ = false;
                t0_ = in.nowS;
                Raise(o, GEvent::LoadStart, true, GFx::None);
            }
        }
        return Fill(o, p);
    }

    void AddFailed(int chamber)
    {
        if (addsReq_ > 0) { --addsReq_; }
        if (chamber >= 0 && chamber < 6) { ch_[chamber] = DChamber::Empty; }
    }
    bool loading() const { return loading_; }
    int  hand() const { return hand_; }
    bool open() const { return open_; }

private:
    // turn to the next chamber that is not loaded (from the one at the gate); a spent one there pops out (CaseOut). All loaded = stay.
    void Advance(GOut& o)
    {
        for (int s = 0; s < 6; ++s) {
            const int c = (idx_ + s) % 6;
            if (ch_[c] != DChamber::Live) {
                idx_ = c;
                if (ch_[c] == DChamber::Spent) {
                    ch_[c] = DChamber::Empty;
                    Raise(o, GEvent::CaseOut, false, GFx::None);
                }
                return;
            }
        }
    }
    void Reconcile(int rounds)
    {
        int live = 0;
        for (DChamber c : ch_) { live += c == DChamber::Live ? 1 : 0; }
        for (int s = 0; s < 6 && live < rounds; ++s) {
            const int c = (idx_ + s) % 6;
            if (ch_[c] != DChamber::Live) { ch_[c] = DChamber::Live; ++live; }
        }
        for (int s = 5; s >= 0 && live > rounds; --s) {
            const int c = (idx_ + s) % 6;
            if (ch_[c] == DChamber::Live) { ch_[c] = DChamber::Spent; --live; }
        }
    }
    static float LoadFrame(float animS, const GParams& p)
    {
        float f = 1.0f + (animS > 0.0f ? animS : 0.0f) * static_cast<float>(p.fps);
        if (f > static_cast<float>(p.loadFrames)) { f = static_cast<float>(p.loadFrames); }
        return f;
    }
    static void Raise(GOut& o, GEvent e, bool haptic, GFx fx)
    {
        if (o.ev == GEvent::None) { o.ev = e; }
        o.events |= 1u << static_cast<unsigned>(e);
        if (haptic) { o.haptic = true; }
        if (fx != GFx::None && o.nFx < 4) { o.fx[o.nFx++] = fx; }
    }
    GOut& Fill(GOut& o, const GParams& p) const
    {
        o.gateOpen = open_;
        o.withholdR2 = open_;
        o.cylinderIndex = idx_;
        for (int s = 0; s < 6; ++s) { o.ch[s] = ch_[s]; }
        o.hand = hand_;
        o.loading = loading_;
        o.locked = loading_;
        o.loadFrame = loading_ ? LoadFrame(static_cast<float>(nowS_ - t0_) - p.leadS, p) : 1.0f;
        return o;
    }

    bool     open_ = false;
    bool     loading_ = false;
    bool     seated_ = false;
    bool     loaded_ = false;
    DChamber ch_[6] = { DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent };
    int      idx_ = 0;
    int      hand_ = 0;
    double   t0_ = 0.0, nowS_ = 0.0;
    double   lockUntil_ = 0.0;   // RG4: no flick counts before this (after a toggle)
    bool     takeWas_ = false;
    bool     loadArmed_ = true;
    int      prevRounds_ = -1;
    int      addsReq_ = 0;
};

}  // namespace reload
}  // namespace dlvr
