// dbarrel_logic.h - the DOUBLE-BARREL wn_shotgun_a's manual reload as a pure step machine (unit-tested:
// src/src/tests/dbarrel_logic_test.cpp). Spec docs/superpowers/specs/2026-11-04-manual-reload-pump-shotgun-design.md section 9, every
// rule below is one of Jay's picks quoted there:
//   OPENING "A"      the left hand grips OVER THE TOP of the barrels and they follow it down to 45 deg, stopping there.
//   OPEN vs FOREND "A" a grip on top opens it; a grip under the forend is the existing two-hand hold (forendHeld refuses the top grip).
//   SPENT SHELLS "A" with the gun open, a grip at the chambers takes the spent shells together (a LOCKED pull-out - Jay "it needs to be
//                    part of a locked animation"); letting go drops them.
//   UNFIRED "A"      only the spent shell comes out; the live one stays in its chamber.
//   LOADING          both shells from the hip in one grab (1 if a live one stayed), each pushed in by the thumb (one locked animation).
//   CLOSING "B"      the opening grip reversed lifts the barrels until they snap shut, OR a flick (FLICK "A": the muzzle snapped UP);
//                    the flick swings them shut on their own, accelerating, ~0.15 s, stop dead + rmngtn_close + a pulse.
//   FIRING ORDER "A" the RIGHT barrel fires first; one round left = the LEFT chamber.
// Chambers: 0 = LEFT (bullet_l, ammo_bullet), 1 = RIGHT (bullet_r, ammo_bullet_b). The game counts rounds only (INV-700); which
// chamber holds what is this machine's, kept equal to the round count whenever the gun is shut.
// 2026-11-07 the FAMILY (Jay: d / e / g "Ship ON with a's"): DParams::set picks the tables - kDbSetA (a, d, g: two chambers) or kDbSetE
// (the Ox e, AmmoCount 1: ONE chamber - element 0, Jay's centre seat; chamber 1 is always Empty and never drawn).
// No game access, no logging, no allocation. Times in seconds (the driver's QPC clock).
#pragma once

#include "dbarrel_hands.h"

namespace dlvr {
namespace reload {

enum class DPhase { Closed, Riding, Open, FlickClosing, Pulling, Loading };
enum class DChamber { Empty, Live, Spent };
enum class DEvent {
    None, OpenGrab, Opened, OpenCancel, CloseGrab, Closed, FlickClose, PullStart, PullDone, SpentDropped, Take, TakeNoShell,
    TakeShut, LoadStart, ShellIn, LoadDone, HandCleared, Shot, Count
};
enum class DFx { None, Open, Seat, Close };

struct DParams {
    float openDeg = kDbOpenDeg;   // Jay "E" = 45
    float flickDps = 400.0f;      // reload_flick_dps (Tier D; Jay "Sure" to 400 as the start)
    int   flickUpdates = 2;       // consecutive updates at or above flickDps
    float flickCloseS = 0.15f;    // Jay: "~0.15 s"
    float leadS = 0.10f;          // the lock's blend-in (kHandBlendMs): the animation waits on it; the pull's shells ease seat -> frame 1
    int   set = kDbSetA;          // the gun's tables (its DbGunRow::set): kDbSetA two chambers, kDbSetE the Ox's one
};

struct DInput {
    bool   manualOn = false;
    bool   freshDraw = false;
    bool   gripDown = false;           // LEFT grip, debounced
    bool   gripEdge = false;           // the grip went DOWN this update - the top grip and the pull need a fresh squeeze (RUN DB1: a hand
                                       // sliding in with the grip already held grabbed and let go 27 / 33 ms later)
    bool   inHip = false;              // the left hip pouch zone
    int    rounds = 0, spare = 0;
    bool   nearTop = false;            // the drawn PALM over the barrels (dbarrel_hands.h DbZoneOf .top) - with gripEdge: the top grip
    bool   nearPull = false;           // the drawn palm at the breech (DbZoneOf .breech) - with gripEdge: the pull (gun open)
    bool   nearLoad = false, farLoad = false;   // the palm at the breech / beyond 1.5 x its ball - the load (grip held from the hip)
    bool   forendHeld = false;         // the two-hand grip holds the gun (the forend) - the top grip is refused while it does
    bool   rideValid = false;          // rideAngle is fresh (both controllers fresh, a latch exists)
    float  rideAngle = 0.0f;           // the angle the riding hand asks (DbRideAngle)
    float  flickUpDps = 0.0f;          // the muzzle's upward turn rate this update (DbMuzzleUpDps)
    double nowS = 0.0;
};

struct DOut {
    unsigned events = 0;               // bit per DEvent raised this update
    DEvent   ev = DEvent::None;        // the first raised
    DFx      fx[4] = {};
    int      nFx = 0;
    bool     haptic = false;
    bool     callAddBullet = false;    // call the game's add-bullet ONCE this update (one round - INV-699's +429 rule is the driver's)
    int      addChamber = -1;
    bool     withholdR2 = false;
    DPhase   phase = DPhase::Closed;
    float    angle = 0.0f;
    DChamber ch[2] = { DChamber::Empty, DChamber::Empty };
    // drawing (bit s = shell element s)
    int      animMask = 0;             // drawn from the playing animation's shellM (pull / load)
    int      handMask = 0;             // drawn in the hand: the hold frame's shellH
    int      seatMask = 0;             // drawn seated in its chamber (angle > 0, the chamber holds a shell, not drawn otherwise)
    bool     handSpent = false;        // the hand's shells came out of the gun (the pull's last frame) - else from the hip (the load's)
    DbAnim   anim = DbAnim::None;      // the fingers (and while locked the hand)
    float    animFrame = 1.0f;
    bool     animHeld = true;          // the reader must not advance it
    bool     locked = false;           // the drawn hand is locked to the gun
    float    pullLeadT = 1.0f;         // 0 .. 1 over the pull's lead-in: its shells seat -> frame 1
    bool Has(DEvent e) const { return (events & (1u << static_cast<unsigned>(e))) != 0; }
};

inline int DbBits(int m) { return (m & 1) + ((m >> 1) & 1); }

class DMachine {
public:
    DOut Update(const DInput& in, const DParams& p)
    {
        const bool takeNow = in.gripDown && in.inHip;
        const bool takeEdge = takeNow && !takeWas_;
        takeWas_ = takeNow;
        if (!in.manualOn) {
            *this = DMachine{};
            return DOut{};
        }
        DOut o{};
        set_ = NormSet(p.set);
        if (prevRounds_ < 0 || in.freshDraw) {
            const bool had = hand_ != 0 || phase_ == DPhase::Pulling || phase_ == DPhase::Loading;
            const bool wasHere = prevRounds_ >= 0;
            *this = DMachine{};
            set_ = NormSet(p.set);
            takeWas_ = takeNow;
            ch_[0] = in.rounds >= 1 ? DChamber::Live : DChamber::Spent;
            ch_[1] = DbChambers(set_) < 2 ? DChamber::Empty : in.rounds >= 2 ? DChamber::Live : DChamber::Spent;   // e: no second chamber
            prevRounds_ = in.rounds;
            if (had && wasHere) { Raise(o, DEvent::HandCleared, false, DFx::None); }
            return Fill(o, in, p);
        }
        if (in.farLoad) { loadArmed_ = true; }

        // Shots: only while shut (R2 is withheld otherwise). Our own add-bullets since the last update are expected in the count.
        const int expected = prevRounds_ + addsReq_;
        if (phase_ == DPhase::Closed && in.rounds < expected) {
            for (int k = in.rounds; k < expected; ++k) {
                const int c = ch_[1] == DChamber::Live ? 1 : ch_[0] == DChamber::Live ? 0 : -1;   // RIGHT first (Jay "A")
                if (c >= 0) { ch_[c] = DChamber::Spent; }
            }
            Raise(o, DEvent::Shot, false, DFx::None);
        }
        prevRounds_ = in.rounds;
        addsReq_ = 0;
        if (phase_ == DPhase::Closed) { Reconcile(in.rounds); }

        // The hand's shells go when the grip lets go (never mid-animation - the pull / load are locked).
        if (!in.gripDown && hand_ != 0 && phase_ != DPhase::Pulling && phase_ != DPhase::Loading) {
            Raise(o, handSpent_ ? DEvent::SpentDropped : DEvent::HandCleared, false, DFx::None);
            hand_ = 0;
        }

        switch (phase_) {
        case DPhase::Closed:
        case DPhase::Open: {
            const bool open = phase_ == DPhase::Open;
            const int spent = Mask(DChamber::Spent) & ChamberMask();
            // the pull first (open, spent shells in, the palm at the breech): the breech ball and the top band never overlap (z 0.15)
            if (open && in.gripEdge && in.nearPull && hand_ == 0 && spent != 0) {
                phase_ = DPhase::Pulling;
                t0_ = in.nowS;
                animMask_ = spent;
                Raise(o, DEvent::PullStart, true, DFx::None);
                break;
            }
            if (in.gripEdge && in.nearTop && hand_ == 0 && !in.forendHeld) {
                phase_ = DPhase::Riding;
                openFlag_ = open;
                Raise(o, open ? DEvent::CloseGrab : DEvent::OpenGrab, true, DFx::None);
                break;
            }
            if (!open) {
                if (takeEdge) { Raise(o, DEvent::TakeShut, false, DFx::None); }
                break;
            }
            // the flick (FLICK "A"): the muzzle snapped up, held for flickUpdates updates
            flickRun_ = in.flickUpDps >= p.flickDps ? flickRun_ + 1 : 0;
            if (flickRun_ >= p.flickUpdates) {
                flickRun_ = 0;
                phase_ = DPhase::FlickClosing;
                t0_ = in.nowS;
                Raise(o, DEvent::FlickClose, false, DFx::None);
                break;
            }
            if (takeEdge && hand_ == 0) {
                const int empty = Mask(DChamber::Empty) & ChamberMask();   // e: its one chamber only
                int n = DbBits(empty);
                if (in.spare < n) { n = in.spare; }
                if (n > 0) {
                    hand_ = n == 2 ? 3 : ((empty & 1) ? 1 : 2);   // one shell: the LEFT chamber first, else the right
                    handSpent_ = false;
                    Raise(o, DEvent::Take, true, DFx::None);
                } else {
                    Raise(o, DEvent::TakeNoShell, false, DFx::None);
                }
            }
            // ONE PUSH per entry (Jay after RUN DB2: "like the other shotguns & we have to push each shell manually" - the pump's port
            // rule): the LEFT chamber's shell first (cycle 1 = load frames 1 .. kDbLoadOneStart), else the RIGHT's (kDbLoadOneStart .. end).
            // Every shell still in the hand is drawn by the animation during the push; the hand leaves the breech ball (farLoad) to re-arm.
            if (in.gripDown && in.nearLoad && loadArmed_ && hand_ != 0 && !handSpent_) {
                loadArmed_ = false;
                phase_ = DPhase::Loading;
                t0_ = in.nowS;
                animMask_ = hand_;
                pushCh_ = (hand_ & 1) ? 0 : 1;
                seated_ = 0;
                Raise(o, DEvent::LoadStart, true, DFx::None);
            }
            break;
        }
        case DPhase::Riding: {
            if (in.rideValid) { angle_ = in.rideAngle < 0.0f ? 0.0f : (in.rideAngle > p.openDeg ? p.openDeg : in.rideAngle); }
            if (!openFlag_ && angle_ >= p.openDeg - 0.5f) {
                openFlag_ = true;
                Raise(o, DEvent::Opened, true, DFx::Open);
            } else if (openFlag_ && angle_ <= 0.5f) {
                openFlag_ = false;
                angle_ = 0.0f;
                Raise(o, DEvent::Closed, true, DFx::Close);
            }
            if (!in.gripDown) {
                if (openFlag_) {
                    phase_ = DPhase::Open;   // the barrels drop back to 45 (they hang open)
                    angle_ = p.openDeg;
                } else {
                    if (angle_ > 0.5f) { Raise(o, DEvent::OpenCancel, false, DFx::None); }
                    phase_ = DPhase::Closed;
                    angle_ = 0.0f;
                }
            }
            break;
        }
        case DPhase::FlickClosing: {
            const float e = static_cast<float>(in.nowS - t0_);
            angle_ = DbFlickAngle(p.openDeg, e, p.flickCloseS);
            if (e >= p.flickCloseS) {
                angle_ = 0.0f;
                phase_ = DPhase::Closed;
                Raise(o, DEvent::Closed, true, DFx::Close);
            }
            break;
        }
        case DPhase::Pulling: {
            const float e = static_cast<float>(in.nowS - t0_);
            const float animS = e - p.leadS;
            if (animS >= DbTimeOfFrame(DbPullFramesOf(set_))) {
                for (int s = 0; s < 2; ++s) {
                    if (animMask_ & (1 << s)) { ch_[s] = DChamber::Empty; }
                }
                hand_ = animMask_;
                handSpent_ = true;
                animMask_ = 0;
                phase_ = DPhase::Open;
                Raise(o, DEvent::PullDone, false, DFx::None);
            }
            break;
        }
        case DPhase::Loading: {
            const float animS = static_cast<float>(in.nowS - t0_) - p.leadS;
            const float f = LoadFrame(animS);
            const int b = 1 << pushCh_;
            if (!(seated_ & b) && f >= static_cast<float>(DbSeatFrameOf(set_, pushCh_))) {   // this push's ONE shell
                seated_ |= b;
                hand_ &= ~b;
                ch_[pushCh_] = DChamber::Live;
                ++addsReq_;
                o.callAddBullet = true;
                o.addChamber = pushCh_;
                Raise(o, DEvent::ShellIn, true, DFx::Seat);
            }
            if ((seated_ & b) && f >= static_cast<float>(LoadEnd())) {   // the push is over: the hand is free (any shell left in it)
                animMask_ = 0;
                seated_ = 0;
                phase_ = DPhase::Open;
                Raise(o, DEvent::LoadDone, false, DFx::None);
            }
            break;
        }
        }
        if (phase_ != DPhase::Open) { flickRun_ = 0; }
        return Fill(o, in, p);
    }

    // The add-bullet of this update's callAddBullet FAULTED: the game added nothing - the chamber is empty again and the add is not
    // expected in the next count (else it reads as a shot). Call right after the Update that asked.
    void AddFailed(int chamber)
    {
        if (addsReq_ > 0) { --addsReq_; }
        if (chamber == 0 || chamber == 1) { ch_[chamber] = DChamber::Empty; }
    }
    DPhase   phase() const { return phase_; }
    float    angle() const { return angle_; }
    DChamber chamber(int s) const { return ch_[s & 1]; }
    int      hand() const { return hand_; }

private:
    // a / e / the single-shot (2026-11-09); anything else = a's tables
    static int NormSet(int s) { return (s == kDbSetE || s == kDbSetPe) ? s : kDbSetA; }
    int Mask(DChamber c) const { return (ch_[0] == c ? 1 : 0) | (ch_[1] == c ? 2 : 0); }
    int ChamberMask() const { return DbChambers(set_) < 2 ? 1 : 3; }
    // shut: the live chambers are made to equal the count (the game's own number wins - a pickup, a cheat, a co-op sync). e: its one.
    void Reconcile(int rounds)
    {
        const int n = DbChambers(set_);
        int live = DbBits(Mask(DChamber::Live));
        for (int s = 0; s < n && live < rounds; ++s) {
            if (ch_[s] != DChamber::Live) { ch_[s] = DChamber::Live; ++live; }
        }
        for (int s = n - 1; s >= 0 && live > rounds; --s) {   // the RIGHT is the one fired first
            if (ch_[s] == DChamber::Live) { ch_[s] = DChamber::Spent; --live; }
        }
    }
    // a: the LEFT push = frames 1 .. kDbLoadOneStart, the RIGHT = kDbLoadOneStart .. end. e: its one push = the whole table.
    int LoadStart() const { return DbOneChamber(set_) ? 1 : pushCh_ == 1 ? kDbLoadOneStart : 1; }
    int LoadEnd() const { return DbOneChamber(set_) ? DbLoadFramesOf(set_) : pushCh_ == 0 ? kDbLoadOneStart : kDbLoadFrames; }
    float LoadFrame(float animS) const
    {
        float f = static_cast<float>(LoadStart()) + (animS > 0.0f ? animS : 0.0f) * static_cast<float>(kDbFps);
        if (f > static_cast<float>(LoadEnd())) { f = static_cast<float>(LoadEnd()); }
        return f;
    }
    static void Raise(DOut& o, DEvent e, bool haptic, DFx fx)
    {
        if (o.ev == DEvent::None) { o.ev = e; }
        o.events |= 1u << static_cast<unsigned>(e);
        if (haptic) { o.haptic = true; }
        if (fx != DFx::None && o.nFx < 4) { o.fx[o.nFx++] = fx; }
    }
    DOut& Fill(DOut& o, const DInput& in, const DParams& p)
    {
        o.phase = phase_;
        o.angle = angle_;
        o.ch[0] = ch_[0];
        o.ch[1] = ch_[1];
        o.withholdR2 = phase_ != DPhase::Closed;
        o.handSpent = handSpent_;
        o.handMask = 0;
        o.animMask = 0;
        switch (phase_) {
        case DPhase::Riding:
            o.anim = DbOpenAnim(set_);   // a / e: a's open grip; the single-shot: Jay's (2026-11-09)
            o.locked = true;
            break;
        case DPhase::Pulling: {
            const float e = static_cast<float>(in.nowS - t0_);
            o.anim = DbPullAnim(set_);
            o.animFrame = DbFrameOfTime(o.anim, e - p.leadS);
            o.animHeld = false;
            o.locked = true;
            o.animMask = animMask_;
            o.pullLeadT = p.leadS > 0.0f ? (e >= p.leadS ? 1.0f : (e > 0.0f ? e / p.leadS : 0.0f)) : 1.0f;
            break;
        }
        case DPhase::Loading: {
            o.anim = DbLoadAnim(set_);
            o.animFrame = LoadFrame(static_cast<float>(in.nowS - t0_) - p.leadS);
            o.animHeld = false;
            o.locked = true;
            o.animMask = animMask_;
            break;
        }
        default:
            if (hand_ != 0) {
                o.handMask = hand_;
                o.anim = handSpent_ ? DbPullAnim(set_) : DbLoadAnim(set_);
                o.animFrame = handSpent_ ? static_cast<float>(DbPullFramesOf(set_)) : static_cast<float>(hand_ == 2 ? DbOneStartOf(set_) : 1);
            }
            break;
        }
        if (DbIsLoad(o.anim) && o.animFrame >= static_cast<float>(LoadEnd()) && phase_ == DPhase::Loading) { o.animHeld = true; }
        // seated: a chamber holding a shell, the barrels off shut, and the shell not drawn by the animation / the hand
        if (angle_ > 0.0f) {
            for (int s = 0; s < 2; ++s) {
                const int b = 1 << s;
                if (ch_[s] != DChamber::Empty && !(o.animMask & b) && !(o.handMask & b)) { o.seatMask |= b; }
            }
        }
        return o;
    }

    DPhase   phase_ = DPhase::Closed;
    int      set_ = kDbSetA;        // the gun's tables (DParams::set, kept from each Update)
    DChamber ch_[2] = { DChamber::Spent, DChamber::Spent };
    float    angle_ = 0.0f;
    bool     openFlag_ = false;     // Riding: the barrels have reached open on this hold (a lift back to 0 then closes them)
    int      hand_ = 0;             // bit s: shell element s in the hand
    bool     handSpent_ = false;
    int      animMask_ = 0;         // the pull's / load's shells
    int      seated_ = 0;           // this push's shell is in
    int      pushCh_ = 0;           // the chamber this push loads (0 LEFT, 1 RIGHT)
    double   t0_ = 0.0;
    int      flickRun_ = 0;
    bool     takeWas_ = false;
    bool     loadArmed_ = true;
    int      prevRounds_ = -1;
    int      addsReq_ = 0;
};

}  // namespace reload
}  // namespace dlvr
