// fuel_logic.h - THE CHAINSAW GUN's manual reload (pour the fuel, pull the cord) as a pure machine (unit-tested:
// src/src/tests/fuel_logic_test.cpp). Plan docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 6; spec
// 2026-11-09-manual-reload-remaining-guns-design.md 3.8. Jay 2026-11-09: "Pour the fuel by hand, then pull the cord once" + the bottle from
// the "Left hip pouch". Plan ruling R4 (my calls, for Jay):
//   TAKE   the rising edge of (grip held AND in the hip pouch) - the pump's R1 (INV-673) - gives the bottle when the gun is not full.
//   POUR   time accumulates only while the bottle is tipped >= tiltMinDeg from upright AND its spout is at the filler; pourNeedS (1.0 s)
//          of it = Filled: ONE add-bullet = the game's whole fill (ClipReload true, AmmoCount 100). Let go first = the bottle goes away
//          and the pour time is lost (Review 3).
//   CORD   after the fill the gun is not ready (R2 withheld) until a grip at the starter T-handle pulls it >= cordM (0.25 m - the game's
//          first pull ~0.26 m); a release short of it = CordShort.
// A gun drawn with fuel in it is ready; empty (rounds 0) it is not. No game access, no logging, no allocation.
#pragma once

#include "reload_logic.h"   // Fx

namespace dlvr {
namespace reload {

enum class FEvent { None, Take, TakeNone, Filled, BottleAway, CordGrab, CordPulled, CordShort, Count };

struct FInput {
    bool  manualOn = false, isChainsaw = false, freshDraw = false;
    bool  gripDown = false, inHip = false;
    bool  spoutAtFiller = false;   // the bottle's spout tip within the filler ball
    bool  nearCord = false;        // the palm at the starter T-handle
    float tiltDeg = 0.0f;          // the bottle's up axis from world up (0 = upright, 180 = upside down)
    float dtS = 0.0f;              // seconds since the last update
    float cordPullM = 0.0f;        // the hand's travel out along the cord since the cord grab
    int   rounds = 0, spare = 0, capacity = 100;
};

struct FOut {
    unsigned events = 0;
    FEvent   ev = FEvent::None;
    bool     bottleInHand = false;
    bool     cordHeld = false;      // the T-handle follows the hand along the cord
    bool     callAddBullet = false;
    bool     ready = false;
    bool     withholdR2 = false;
    bool     haptic = false;
    float    pourS = 0.0f;
    Fx       fx = Fx::None;
    bool Has(FEvent e) const { return (events & (1u << static_cast<unsigned>(e))) != 0; }
};

class FMachine {
public:
    FOut Update(const FInput& in, float pourNeedS = 1.0f, float tiltMinDeg = 90.0f, float cordM = 0.25f)
    {
        const bool takeNow = in.gripDown && in.inHip;
        const bool takeEdge = takeNow && !takeWas_;
        takeWas_ = takeNow;
        const bool cordNow = in.gripDown && in.nearCord;
        const bool cordEdge = cordNow && !cordWas_;
        cordWas_ = cordNow;
        if (!in.manualOn || !in.isChainsaw) {
            *this = FMachine{};
            return FOut{};
        }
        FOut o{};
        if (!init_ || in.freshDraw) {
            const bool keepNeed = init_ && needCord_;
            *this = FMachine{};
            init_ = true;
            takeWas_ = takeNow;
            cordWas_ = cordNow;
            needCord_ = keepNeed;
            return Fill(o, in);
        }
        if (bottle_ && !in.gripDown) {
            bottle_ = false;
            pourS_ = 0.0f;
            Raise(o, FEvent::BottleAway, false);
        }
        if (cordHeld_ && !in.gripDown) {
            cordHeld_ = false;
            if (needCord_) { Raise(o, FEvent::CordShort, false); }
        }
        if (takeEdge && !bottle_ && !cordHeld_) {
            if (in.rounds < in.capacity && in.spare > 0) {
                bottle_ = true;
                filled_ = false;
                pourS_ = 0.0f;
                Raise(o, FEvent::Take, true);
                o.fx = Fx::PouchTake;
            } else {
                Raise(o, FEvent::TakeNone, false);
            }
        }
        if (bottle_ && !filled_ && in.spoutAtFiller && in.tiltDeg >= tiltMinDeg && in.dtS > 0.0f) {
            pourS_ += in.dtS;
            if (pourS_ >= pourNeedS) {
                filled_ = true;
                needCord_ = true;
                o.callAddBullet = true;
                Raise(o, FEvent::Filled, true);
                o.fx = Fx::Seat;
            }
        }
        if (cordEdge && !bottle_) {
            cordHeld_ = true;
            Raise(o, FEvent::CordGrab, true);
        }
        if (cordHeld_ && needCord_ && in.cordPullM >= cordM) {
            needCord_ = false;
            Raise(o, FEvent::CordPulled, true);
            o.fx = Fx::RackDone;
        }
        return Fill(o, in);
    }
    bool bottle() const { return bottle_; }

private:
    static void Raise(FOut& o, FEvent e, bool haptic)
    {
        if (o.ev == FEvent::None) { o.ev = e; }
        o.events |= 1u << static_cast<unsigned>(e);
        if (haptic) { o.haptic = true; }
    }
    FOut& Fill(FOut& o, const FInput& in) const
    {
        o.bottleInHand = bottle_;
        o.cordHeld = cordHeld_;
        o.pourS = pourS_;
        const int rounds = o.callAddBullet ? in.capacity : in.rounds;
        o.ready = rounds > 0 && !needCord_;
        o.withholdR2 = !o.ready;
        return o;
    }

    bool  init_ = false;
    bool  bottle_ = false;
    bool  filled_ = false;
    bool  needCord_ = false;
    bool  cordHeld_ = false;
    float pourS_ = 0.0f;
    bool  takeWas_ = false, cordWas_ = false;
};

}  // namespace reload
}  // namespace dlvr
