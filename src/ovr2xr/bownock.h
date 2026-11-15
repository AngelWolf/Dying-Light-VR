// The bow's MANUAL NOCK - docs/superpowers/specs/2026-10-17-bow-manual-nock-design.md (plan 2026-10-17-bow-manual-nock).
// Settings live in bownock.cpp (each with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp beside bow_manual_draw.
// The decisions are nock_logic.h (unit-tested); this file applies them on the game thread from BowDrawTick.
#pragma once

#include <cstdint>

namespace dlvr {

extern bool  BowManualNock;     // bow_manual_nock    - DEFAULT ON, LOCKED - Tier A 2026-10-17 (was OFF, Tier C: "Off until verified")
extern float BowNockCatchM;     // bow_nock_catch_m   - catch radius round the bow's line, metres
extern float BowNockHoldM;      // bow_nock_hold_m    - how far off the line a caught arrow may stray, metres
extern float BowNockSeatM;      // bow_nock_seat_m    - the notch this close to the string seats, metres
extern float BowNockCatchDeg;   // bow_nock_catch_deg - how far the arrow may point off the bow's line, degrees
extern float BowNockCatchLenM;  // bow_nock_catch_len_m - how far in front of the string the latch reaches, metres
extern float BowNockCarryBackM; // bow_nock_carry_back_m - how far back along itself the carried arrow sits, metres (tunable)
extern float BowNockCarryTiltDeg; // bow_nock_carry_tilt_deg - the carried arrow tilted about the hand's Y axis, degrees (tunable)
extern float BowNockCarryTurnDeg; // bow_nock_carry_turn_deg - the carried arrow turned about the hand's Z axis, degrees (tunable)
extern float BowNockCarrySideM;   // bow_nock_carry_side_m - the carried arrow slid horizontally across its shaft, metres

struct BowNockCtx {
    uintptr_t item, ctrl; int state;            // the LEFT item (the bow) and its controller, as BowDrawTick read them
    const float* rootW; const float* nockW;      // bone_root / arrow_pose world 3x4 this update
    const float* restNockInRoot;                 // the live REST LATCH (3x4), or nullptr -> kNockRestInRoot
    const float* handPos; const float* handR9;   // BowDrawRightHand's achieved position + drawn basis; nullptr = no hand
    int nockId; void* setWorld;                  // arrow_pose's element id, the SetElementWorldMatrix export (BsSetWorldFn)
    bool trigDown; long pressSeq;                // g_bdRealDown, g_bdPressSeq
    // The hand-carried arrow (spec 2026-10-22-hand-carried-model-design): the player model the rig reads, r_hand's element id on it
    // (E6(53), checked by name), and r_hand's RENDERED world 3x4 this update - nullptr = not available (no carry, no catch geometry).
    void* playerModel; int rHandId; const float* rHandW;
};

// Game thread (BowDrawTick, HookTail), once per player update with a bow and a readable controller. Off = one bool test.
void BowNockTick(const BowNockCtx& c);
// INV-590 (Jay 2026-10-17: "Build it, ride bow_manual_nock"): ItemCtlPlace's LEFT-hand grip rel for a bow. With the nock on (and
// live) and the item a bow, relR / relT are REPLACED with the fixed rel (the draw's l_handholder local) and true is returned, also
// when no live rel was captured (haveLive = false); otherwise false and they are untouched. Game thread. Off = one bool test.
bool BowNockFixedRel(uintptr_t item, bool haveLive, float relR[9], float relT[3]);
// The arrow-holding finger blend, 0..1 (100 ms each way; Jay 2026-10-17 "Use the game's reload pose"). False = not holding. Any thread.
bool BowNockFingerT(float* t);
// The left hand holds no usable bow this update: remember the old bow's arrow, stop faking its count (a carry is forgotten).
void BowNockNoBow();
// The arrow wheel from the quiver (2026-11-11, Jay: "Yes."): true while the quiver hold is down - RT pressed with the right hand in the
// shoulder zone, still down, and (until the arrow wheel is open, HolsterArrowWheelOpen) the hand still in the zone. Published by
// BowNockTick (game thread); false once its stamp is older than boltwheel::kWantFreshMs (the tick stops with the bow put away).
// Any thread. COST: two volatile reads + a tick read.
bool BowNockQuiverHold();
// FIRST thing in BowDrawTick, before any early return (final review 2026-10-22 (1)): with an arrow on the hand and BowNockTick not
// run during the last update, give it back to the live bow (or forget it if the left item changed). Off / not carrying = one bool.
void BowNockCarryWatch(void* ent);

}  // namespace dlvr
