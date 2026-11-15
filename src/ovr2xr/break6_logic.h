// break6_logic.h - THE SCHOFIELD's manual reload (a six-chamber break-top) as a pure step machine (unit-tested:
// src/src/tests/break6_logic_test.cpp). Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 3; spec
// docs/superpowers/specs/2026-11-09-manual-reload-remaining-guns-design.md 3.4. Jay 2026-11-09, every rule below:
//   OPEN   "Flick open for the revolvers" + "Downward flick": the muzzle snapped DOWN (flickDps, held flickUpdates updates) swings the
//          barrel open to the game's 80 deg (m_fpp_schofield_reload f20-f35) on its own (openS, easing out).
//   CASES  "all 6 spent cases drop out on opening": every Spent chamber is Empty the moment it is open; a Live one stays.
//   ROUNDS "all 6 bullets in one hand, feed them in one at a time" (+ "Just the one that's about to be pushed in" - drawing is the
//          driver's): one hip take holds min(empty chambers, spare); ONE PUSH per entry at the breech fills the next EMPTY chamber
//          (the single-shot's push animation, frames 1..loadFrames, the add at seatFrame); the hand must leave (farLoad) to re-arm.
//   CLOSE  "by hand or upward flick": the muzzle snapped UP swings it shut (accelerating, flickCloseS - the double-barrel's), or a grip
//          on top rides it shut. Closing after at least one round went in = ReloadDone (the fan-cock is asked next - cock_logic.h).
// Shots: only while shut; the lowest Live chamber is spent first. The game counts rounds; the chambers are this machine's, made to equal
// the count whenever it is shut. No game access, no logging, no allocation. Times in seconds (the driver's QPC clock).
#pragma once

#include "dbarrel_logic.h"   // DChamber, DbFlickAngle

namespace dlvr {
namespace reload {

enum class SPhase { Closed, Opening, Open, Loading, Closing, Riding };
enum class SEvent {
    None, FlickOpen, Opened, CasesOut, Take, TakeNoRound, LoadStart, RoundIn, LoadDone, HandCleared, FlickClose, CloseGrab, Closed,
    ReloadDone, Shot, Count
};
enum class SFx { None, Open, Seat, Close };

struct SParams {
    float openDeg = 80.0f;        // the game's own (m_fpp_schofield_reload, INV-541 decode)
    float flickDps = 400.0f;      // reload_flick_dps (shared with the double-barrel; Jay: no new key)
    int   flickUpdates = 2;
    float openS = 0.15f;          // the barrel swings open (my call: the close's time, decelerating)
    float flickCloseS = 0.15f;    // the double-barrel's close
    float leadS = 0.10f;          // the lock's blend-in before the push plays
    int   loadFrames = 7;         // the single-shot's push (kDbPeLoadFrames) at 60 fps
    int   seatFrame = 7;          // the add at its seat frame (kDbPeLoadSeatFrame)
    int   fps = 60;
};

struct SInput {
    bool   manualOn = false, freshDraw = false;
    bool   gripDown = false, inHip = false;
    bool   nearLoad = false, farLoad = false;   // the palm at the next empty chamber's mouth / beyond 1.5 x its ball
    bool   nearTop = false;                     // the palm over the barrel (the hand close)
    bool   rideValid = false;
    float  rideAngle = 0.0f;
    float  flickDownDps = 0.0f, flickUpDps = 0.0f;   // the muzzle's turn rate down / up this update
    int    rounds = 0, spare = 0;
    double nowS = 0.0;
};

struct SOut {
    unsigned events = 0;
    SEvent   ev = SEvent::None;
    SFx      fx[4] = {};
    int      nFx = 0;
    bool     haptic = false;
    bool     callAddBullet = false;
    int      addChamber = -1;
    bool     withholdR2 = false;
    SPhase   phase = SPhase::Closed;
    float    angle = 0.0f;
    DChamber ch[6] = {};
    int      hand = 0;                 // rounds in the hand (one is drawn - Jay)
    int      loadChamber = -1;         // the chamber the push plays at (Loading) / the next empty one (Open, hand > 0)
    float    loadFrame = 1.0f;         // the push animation's frame (Loading)
    bool     locked = false;           // the hand locked to the gun (Loading / Riding)
    bool Has(SEvent e) const { return (events & (1u << static_cast<unsigned>(e))) != 0; }
};

class SMachine {
public:
    SOut Update(const SInput& in, const SParams& p)
    {
        const bool takeNow = in.gripDown && in.inHip;
        const bool takeEdge = takeNow && !takeWas_;   // the pump's R1: the rising edge of (held AND in the pouch), INV-673
        takeWas_ = takeNow;
        const bool topNow = in.gripDown && in.nearTop;
        const bool topEdge = topNow && !topWas_;
        topWas_ = topNow;
        if (!in.manualOn) {
            *this = SMachine{};
            return SOut{};
        }
        SOut o{};
        nowS_ = in.nowS;
        if (prevRounds_ < 0 || in.freshDraw) {
            const bool had = hand_ != 0 && prevRounds_ >= 0;
            *this = SMachine{};
            takeWas_ = takeNow;
            topWas_ = topNow;
            for (int s = 0; s < 6; ++s) { ch_[s] = s < in.rounds ? DChamber::Live : DChamber::Spent; }
            prevRounds_ = in.rounds;
            if (had) { Raise(o, SEvent::HandCleared, false, SFx::None); }
            return Fill(o, p);
        }
        if (in.farLoad) { loadArmed_ = true; }
        const int expected = prevRounds_ + addsReq_;
        if (phase_ == SPhase::Closed && in.rounds < expected) {
            for (int k = in.rounds; k < expected; ++k) {
                for (int s = 0; s < 6; ++s) {
                    if (ch_[s] == DChamber::Live) { ch_[s] = DChamber::Spent; break; }
                }
            }
            Raise(o, SEvent::Shot, false, SFx::None);
        }
        prevRounds_ = in.rounds;
        addsReq_ = 0;
        if (phase_ == SPhase::Closed) { Reconcile(in.rounds); }
        if (!in.gripDown && hand_ != 0 && phase_ != SPhase::Loading) {
            Raise(o, SEvent::HandCleared, false, SFx::None);
            hand_ = 0;
        }
        switch (phase_) {
        case SPhase::Closed:
            downRun_ = in.flickDownDps >= p.flickDps ? downRun_ + 1 : 0;
            if (downRun_ >= p.flickUpdates) {
                downRun_ = 0;
                phase_ = SPhase::Opening;
                t0_ = in.nowS;
                loaded_ = false;
                Raise(o, SEvent::FlickOpen, false, SFx::None);
            }
            break;
        case SPhase::Opening: {
            const float e = static_cast<float>(in.nowS - t0_);
            const float u = p.openS > 0.0f ? (e >= p.openS ? 1.0f : e / p.openS) : 1.0f;
            angle_ = p.openDeg * (1.0f - (1.0f - u) * (1.0f - u));   // decelerating into open
            if (u >= 1.0f) {
                angle_ = p.openDeg;
                phase_ = SPhase::Open;
                Raise(o, SEvent::Opened, true, SFx::Open);
                bool any = false;
                for (DChamber& c : ch_) {
                    if (c == DChamber::Spent) { c = DChamber::Empty; any = true; }
                }
                if (any) { Raise(o, SEvent::CasesOut, false, SFx::None); }
            }
            break;
        }
        case SPhase::Open: {
            if (topEdge && hand_ == 0) {
                phase_ = SPhase::Riding;
                Raise(o, SEvent::CloseGrab, true, SFx::None);
                break;
            }
            upRun_ = in.flickUpDps >= p.flickDps ? upRun_ + 1 : 0;
            if (upRun_ >= p.flickUpdates) {
                upRun_ = 0;
                phase_ = SPhase::Closing;
                t0_ = in.nowS;
                if (hand_ != 0) { Raise(o, SEvent::HandCleared, false, SFx::None); hand_ = 0; }
                Raise(o, SEvent::FlickClose, false, SFx::None);
                break;
            }
            if (takeEdge && hand_ == 0) {
                int n = Empties();
                if (in.spare < n) { n = in.spare; }
                if (n > 0) {
                    hand_ = n;
                    Raise(o, SEvent::Take, true, SFx::None);
                } else {
                    Raise(o, SEvent::TakeNoRound, false, SFx::None);
                }
            }
            if (in.gripDown && in.nearLoad && loadArmed_ && hand_ != 0 && NextEmpty() >= 0) {
                loadArmed_ = false;
                phase_ = SPhase::Loading;
                t0_ = in.nowS;
                pushCh_ = NextEmpty();
                seated_ = false;
                Raise(o, SEvent::LoadStart, true, SFx::None);
            }
            break;
        }
        case SPhase::Loading: {
            const float f = LoadFrame(static_cast<float>(in.nowS - t0_) - p.leadS, p);
            if (!seated_ && f >= static_cast<float>(p.seatFrame)) {
                seated_ = true;
                --hand_;
                ch_[pushCh_] = DChamber::Live;
                loaded_ = true;
                ++addsReq_;
                o.callAddBullet = true;
                o.addChamber = pushCh_;
                Raise(o, SEvent::RoundIn, true, SFx::Seat);
            }
            if (seated_ && f >= static_cast<float>(p.loadFrames)) {
                phase_ = SPhase::Open;
                Raise(o, SEvent::LoadDone, false, SFx::None);
            }
            break;
        }
        case SPhase::Closing: {
            const float e = static_cast<float>(in.nowS - t0_);
            angle_ = DbFlickAngle(p.openDeg, e, p.flickCloseS);
            if (e >= p.flickCloseS) { Shut(o); }
            break;
        }
        case SPhase::Riding: {
            if (in.rideValid) { angle_ = in.rideAngle < 0.0f ? 0.0f : (in.rideAngle > p.openDeg ? p.openDeg : in.rideAngle); }
            if (angle_ <= 0.5f) {
                Shut(o);
            } else if (!in.gripDown) {
                phase_ = SPhase::Open;   // let go before shut: it drops back open
                angle_ = p.openDeg;
            }
            break;
        }
        }
        if (phase_ != SPhase::Closed) { downRun_ = 0; }
        if (phase_ != SPhase::Open) { upRun_ = 0; }
        return Fill(o, p);
    }

    // The add-bullet of this update's callAddBullet FAULTED: the chamber is empty again, the add is not expected in the next count.
    void AddFailed(int chamber)
    {
        if (addsReq_ > 0) { --addsReq_; }
        if (chamber >= 0 && chamber < 6) { ch_[chamber] = DChamber::Empty; }
    }
    SPhase   phase() const { return phase_; }
    float    angle() const { return angle_; }
    int      hand() const { return hand_; }
    DChamber chamber(int s) const { return ch_[s < 0 ? 0 : (s > 5 ? 5 : s)]; }

private:
    int Empties() const
    {
        int n = 0;
        for (DChamber c : ch_) { n += c == DChamber::Empty ? 1 : 0; }
        return n;
    }
    int NextEmpty() const
    {
        for (int s = 0; s < 6; ++s) {
            if (ch_[s] == DChamber::Empty) { return s; }
        }
        return -1;
    }
    void Reconcile(int rounds)
    {
        int live = 0;
        for (DChamber c : ch_) { live += c == DChamber::Live ? 1 : 0; }
        for (int s = 0; s < 6 && live < rounds; ++s) {
            if (ch_[s] != DChamber::Live) { ch_[s] = DChamber::Live; ++live; }
        }
        for (int s = 5; s >= 0 && live > rounds; --s) {
            if (ch_[s] == DChamber::Live) { ch_[s] = DChamber::Spent; --live; }
        }
    }
    void Shut(SOut& o)
    {
        angle_ = 0.0f;
        phase_ = SPhase::Closed;
        Raise(o, SEvent::Closed, true, SFx::Close);
        if (loaded_) { Raise(o, SEvent::ReloadDone, false, SFx::None); }
        loaded_ = false;
    }
    static float LoadFrame(float animS, const SParams& p)
    {
        float f = 1.0f + (animS > 0.0f ? animS : 0.0f) * static_cast<float>(p.fps);
        if (f > static_cast<float>(p.loadFrames)) { f = static_cast<float>(p.loadFrames); }
        return f;
    }
    static void Raise(SOut& o, SEvent e, bool haptic, SFx fx)
    {
        if (o.ev == SEvent::None) { o.ev = e; }
        o.events |= 1u << static_cast<unsigned>(e);
        if (haptic) { o.haptic = true; }
        if (fx != SFx::None && o.nFx < 4) { o.fx[o.nFx++] = fx; }
    }
    SOut& Fill(SOut& o, const SParams& p) const
    {
        o.phase = phase_;
        o.angle = angle_;
        for (int s = 0; s < 6; ++s) { o.ch[s] = ch_[s]; }
        o.hand = hand_;
        o.withholdR2 = phase_ != SPhase::Closed;
        o.locked = phase_ == SPhase::Loading || phase_ == SPhase::Riding;
        o.loadChamber = phase_ == SPhase::Loading ? pushCh_ : (hand_ != 0 ? NextEmpty() : -1);
        o.loadFrame = phase_ == SPhase::Loading ? LoadFrame(static_cast<float>(nowS_ - t0_) - p.leadS, p) : 1.0f;
        return o;
    }

    SPhase   phase_ = SPhase::Closed;
    DChamber ch_[6] = { DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent, DChamber::Spent };
    float    angle_ = 0.0f;
    int      hand_ = 0;
    int      pushCh_ = -1;
    bool     seated_ = false;
    bool     loaded_ = false;     // a round went in since it opened (closing = ReloadDone)
    double   t0_ = 0.0;
    double   nowS_ = 0.0;     // this update's time (Fill's push frame)
    int      downRun_ = 0, upRun_ = 0;
    bool     takeWas_ = false, topWas_ = false;
    bool     loadArmed_ = true;
    int      prevRounds_ = -1;
    int      addsReq_ = 0;
};

}  // namespace reload
}  // namespace dlvr
