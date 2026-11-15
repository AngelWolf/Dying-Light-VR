// cock_logic.h - THE REVOLVERS' FAN-COCK as a pure machine (unit-tested: src/src/tests/cock_logic_test.cpp). Plan
// docs/superpowers/plans/2026-11-09-manual-reload-remaining-guns.md Task 4; spec 2026-11-09-manual-reload-remaining-guns-design.md 3.6.
// Jay 2026-11-09: "I want the hammer to be cocked back manually" -> "Once after reload, on all revolvers" -> "Left hand, like if you were
// to fan fire". After a reload (break6 / gate machine ReloadDone) the trigger is withheld until the LEFT palm, having ENTERED the
// hammer zone (the driver's ball round bone_hammer), travels at least sweepM from its entry point before it leaves (the driver's
// palmTravelM). 2026-11-10 (Jay: "Took me like 2/3 fans over it to get the firing to start"; he felt the zone's enter pulse on the fans
// that did not count -> "Yes" to counting travel in ANY direction): was BACK along the gun only (-z).
// One cock per reload: shots after it never ask again (the game's own shot clip re-cocks the drawn hammer).
// The need survives a redraw (the driver keeps one machine per item - Review 1). No game access, no logging, no allocation.
#pragma once

#include "reload_logic.h"   // Fx

namespace dlvr {
namespace reload {

enum class CEvent { None, CockNeeded, Cocked, Shot };

struct CInput {
    bool  manualOn = false, isRevolver = false, freshDraw = false;
    bool  reloadDone = false;      // the reload machine's ReloadDone this update
    bool  palmInZone = false;      // the drawn left palm inside the hammer ball
    float palmTravelM = 0.0f;      // the palm's straight-line distance from where it entered the ball (any direction, >= 0)
    int   rounds = 0;
};

struct COut {
    CEvent ev = CEvent::None;
    bool   withholdR2 = false;
    bool   cocked = false;         // draw the hammer at the cocked angle (until the next shot)
    bool   haptic = false;
    Fx     fx = Fx::None;          // RackDone on the cock (the game's own hammer sound - the driver's fx row)
};

class CMachine {
public:
    COut Update(const CInput& in, float sweepM = 0.03f)
    {
        if (!in.manualOn || !in.isRevolver) {
            *this = CMachine{};
            return COut{};
        }
        COut o{};
        if (prevRounds_ < 0) { prevRounds_ = in.rounds; }
        if (in.freshDraw) { inZone_ = false; }   // the need and the cocked hammer stay (Review 1)
        if (in.rounds < prevRounds_) {
            cocked_ = false;                      // the hammer fell; never asked again until the next reload
            o.ev = CEvent::Shot;
        }
        prevRounds_ = in.rounds;
        if (in.reloadDone) {
            needed_ = true;
            cocked_ = false;
            o.ev = CEvent::CockNeeded;
        }
        if (needed_ && in.palmInZone) {
            if (!inZone_) {
                inZone_ = true;                   // the sweep counts from the entry
            } else if (in.palmTravelM >= sweepM) {
                needed_ = false;
                cocked_ = true;
                inZone_ = false;
                o.ev = CEvent::Cocked;
                o.haptic = true;
                o.fx = Fx::RackDone;
            }
        } else if (!in.palmInZone) {
            inZone_ = false;
        }
        o.withholdR2 = needed_;
        o.cocked = cocked_;
        return o;
    }
    bool needed() const { return needed_; }
    bool cocked() const { return cocked_; }

private:
    bool needed_ = false;
    bool cocked_ = false;
    bool inZone_ = false;
    int  prevRounds_ = -1;
};

}  // namespace reload
}  // namespace dlvr
