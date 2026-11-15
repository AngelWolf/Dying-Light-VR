// The camouflage gesture - wipe an infected corpse (or a limb attached to / cut from one) held by body grab ACROSS the face to start
// the game's own camouflage, without the Camouflage skill, once per corpse. Spec docs/superpowers/specs/2026-11-07-camouflage-gesture-
// design.md, plan docs/superpowers/plans/2026-11-07-camouflage-gesture.md Task 2, facts INV-704. The rules are camo_logic.h
// (unit-tested). The kSettings rows are in ovr2xr.cpp after body_grab_hitbox_margin_m. Needs body_grab=1 (and so loot_by_hand=1).
// HAND INDEX: 0 = LEFT, 1 = RIGHT (looting.cpp / bodygrab.cpp's convention).
#pragma once

namespace dlvr {

extern bool  CamoGesture;       // camo_gesture - master. Compiled ON (Jay 2026-11-07 after RUN CG1: "Ship on.").
extern float CamoWipeFwdM;      // camo_wipe_fwd_m - how far in front of the head the face zone reaches
extern float CamoWipeRadiusM;   // camo_wipe_radius_m - the zone's half-width / half-height around the face
extern float CamoWipeSweepM;    // camo_wipe_sweep_m - the sideways travel inside the zone that makes a wipe
extern float CamoWipeMaxS;      // camo_wipe_max_s - the travel must happen within this long of entering the zone

// Pad thread (LootPoll), every poll, right after body grab's hand publish: `gripTrk` = the grip's tracking-space position (HandWorld's
// trk), valid when `havePos`. While the hand holds a body it runs the wipe detector in the head's frame and posts a wipe to the game
// thread. COST: camo_gesture off = one bool test; on and holding = one head-pose read + ~40 flops.
void CamoGesturePadPoll(int hand, const float gripTrk[3], bool havePos, double tS);

// Game thread, once per player update, right after BodyGrabTick (boneblend.cpp HookTail). Resolves + installs the limb detour once,
// then judges each posted wipe and starts the game's camouflage. COST: off = one bool test; idle = two loads per hand.
void CamoGestureTick();

}  // namespace dlvr
