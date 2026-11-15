// Two-handed grip, Phase 1 (firearms) and Phase 2 (melee, spec section 7) -docs/SPEC-TWO-HANDED-GRIP-2026-10-09.md. Settings live in twohand.cpp (each
// with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp next to rig_hand_grip_pose.
#pragma once

#include <cstdint>

#include "controllers.h"   // XrPosef

namespace dlvr {

extern bool  RigTwoHandGrip;        // rig_two_hand_grip        - master. DEFAULT ON.
extern float RigTwoHandGunRadius;   // rig_two_hand_gun_radius  - grab: max distance from the right AIM ray, metres
extern float RigTwoHandGunMin;      // rig_two_hand_gun_min     - grab: nearest point along the ray, metres
extern float RigTwoHandGunMax;      // rig_two_hand_gun_max     - grab: farthest point along the ray, metres
extern float RigTwoHandSlideS;      // rig_two_hand_slide_s     - settle time into / out of the hold, seconds
extern bool  RigTwoHandPin;         // rig_two_hand_pin         - pin the DRAWN left hand to the gun's holderl (phase 1b)
extern float RigTwoHandPinRadius;   // rig_two_hand_pin_radius  - grab zone radius around holderl, metres
extern float RigTwoHandHaptic;      // rig_two_hand_haptic      - pulse strength 0..1, 0 = no pulses
extern bool  RigTwoHandPalm;        // rig_two_hand_palm        - the pin holds the PALM centre, not the handholder (INV-502)
extern float RigTwoHandWristOverflowDeg; // rig_two_hand_wrist_overflow_deg - wrist overflow while two-handing (INV-503)
extern bool  RigTwoHandForend;      // rig_two_hand_forend      - the pin point moves from holderl to the forend centre
extern bool  RigTwoHandRiflePose;   // rig_two_hand_rifle_pose  - rifles: the left hand takes the standard rotation (INV-506)
extern float RigTwoHandSlideLen;    // rig_two_hand_slide_len   - rifles: grab strip length along the handguard, metres
extern float RigTwoHandZoneX;       // rig_two_hand_zone_x      - rifles: grab strip reach across the gun, metres
extern bool  RigTwoHandPinReach;    // rig_two_hand_pin_reach   - pinned left arm: no controller reach mapping (INV-507)
extern bool  RigTwoHandZoneDrawn;   // rig_two_hand_zone_drawn  - the grab zone tests the DRAWN palm, not the controller (INV-507)
extern float RigTwoHandRiflePalmXCm; // rig_two_hand_rifle_palm_x_cm - rifles: palm across the grip line, cm (INV-508)
extern float RigTwoHandRiflePalmYCm; // rig_two_hand_rifle_palm_y_cm - rifles: palm below (-) / above (+) the grip line, cm
extern bool  RigTwoHandMelee;        // rig_two_hand_melee          - phase 2: two-handed MELEE (spec section 7)
extern float RigTwoHandMeleeRadius;  // rig_two_hand_melee_radius   - the shaft grab strip's radius, metres
extern float RigTwoHandMeleeButtCm;  // rig_two_hand_melee_butt_cm  - the strip's lower end along the shaft from holder, cm
extern float RigTwoHandMeleeStackCm; // rig_two_hand_melee_stack_cm - the left palm below holder along the shaft, cm
extern float RigTwoHandMeleeCurlDeg; // rig_two_hand_melee_curl_deg - the gripping left fingers' added curl, degrees
extern bool  RigTwoHandMeleePose;    // rig_two_hand_melee_pose     - 1 = the standard left rotation, 0 = capture (INV-510)
extern float RigTwoHandMeleeRollDeg; // rig_two_hand_melee_roll_deg - the left hand rolled about the shaft, degrees (INV-514)
extern float RigTwoHandMeleeWristCm; // rig_two_hand_melee_wrist_cm - the left hand toward its wrist: handle on the finger bases, cm
// run R6: live tuning of a gripped melee weapon in the right hand - offset along the right hand-bone x / y / z (cm), turn about them (deg)
extern float RigTwoHandTuneXCm;      // rig_two_hand_tune_x_cm
extern float RigTwoHandTuneYCm;      // rig_two_hand_tune_y_cm
extern float RigTwoHandTuneZCm;      // rig_two_hand_tune_z_cm
extern float RigTwoHandTuneRxDeg;    // rig_two_hand_tune_rx_deg
extern float RigTwoHandTuneRyDeg;    // rig_two_hand_tune_ry_deg
extern float RigTwoHandTuneRzDeg;    // rig_two_hand_tune_rz_deg
extern float RigTwoHandAimShare;     // rig_two_hand_aim_share - firearms: the hand line's share of the aim (the right wrist has the rest)
extern bool  DiagTwoHandRecord;      // diag_twohand_record    - [TWOHANDREC] per-frame two-hand recorder -> dlvr\twohand_rec.bin

// ItemCtlPlace's once-per-item-pointer block, RIGHT slot only: classify the item - firearms by class + mesh table (phase 1),
// melee by the desc ItemType 15..17 (slot 56, INV-644) OR the TwoHanded flag (slot 176, INV-498); descOk / typeOk false = unreadable.
// Never call it per update.
void TwoHandClassifyItem(uintptr_t item, const char* cls, const char* meshRaw, bool descOk, bool descTwoHanded, bool typeOk,
                         int itemType);
// Every player update, RIGHT slot: which item is held right now (0 = empty). One compare.
void TwoHandNoteHeld(uintptr_t item);
// ItemCtlPlace (HookTail thread), RIGHT slot: the SAME model pointer now carries another InventoryItem (the game reused the address
// on a swap - a tester, 2026-11-08). Drops the HookTail per-item latches and counts it; TwoHandPoll lets go of a held grip on its own
// thread. Called once per such change, after TwoHandNoteHeld(0).
void TwoHandNoteItemReplaced();
// Once per scePad poll, AFTER HolsterLeftPoll (pass its result as holsterBusyL). True = do NOT OR L1 this poll.
// rig_two_hand_grip=0 -> false at the first line.
// reloadOwnsL (rifle plan Task 5 fix round 1): the manual reload owns this press - the caller has ALREADY folded it into holsterBusyL
// (reload::TwoHandPressRefused); it only lets the PRESS line name the reload as the owner instead of a holster.
bool TwoHandPoll(bool vrGripOn, bool holsterBusyL, bool reloadOwnsL = false);
// SolveArmIk, right after it reads this side's controller pose. While gripping, rewrites ctl's ORIENTATION only.
// rig_two_hand_grip=0 -> returns at the first line.
void TwoHandAdjustCtl(bool isLeft, bool gripPoseUsed, XrPosef& ctl);

// HookTail, right after ItemCtlPlace: the right snapshot the gun was just built from, the gun's holderl world (12 floats,
// accessor layout) and the LEFT hand's world grip point. Latches holderl relative to the right frame per item and measures
// the grab zone. rig_two_hand_grip=0 -> returns at the first line.
// haveBarrel / barrelWorld: the gun's `barrel` element world position - the forend table's self-check (INV-502 (c)).
// haveHolder / holder34: the weapon's "Holder" world - read only while TwoHandWantsHolder() (melee); the melee frame.
void TwoHandFeedPlaced(bool haveRight, const float rightDrawn[9], const float rightAchieved[3], bool haveHl,
                       const float hl34[12], const char* hlName, bool haveLeft, const float leftGripWorld[3],
                       bool haveBarrel, const float barrelWorld[3],
                       bool haveLeftDrawn, const float leftDrawn[9], const float leftAchieved[3],
                       bool haveHolder, const float holder34[12]);
// ClampWristSwing's overflow in effect for this solve (twohand::WristOverflowDeg): rig_wrist_overflow_deg, or while
// two-handing the larger rig_two_hand_wrist_overflow_deg. One interlocked read.
float TwoHandWristOverflow();
// SolveArmIk, LEFT pinned solves only, after the reach scale + soft ease: the reach distance to use (INV-507). With
// rig_two_hand_pin_reach=0 returns `mapped` unchanged. Counts and accumulates the change for the log.
float TwoHandPinnedReach(float mapped, float raw, float t);
// The LEFT SolveArmIk, after cWorld is computed: true = this solve is pinned - POSITION ONLY (INV-501): cWorld is read, NOT
// changed; wristPos = the wrist target that puts the DRAWN l_handholder - or with rig_two_hand_palm=1 the palm centre - on
// holderl (the caller blends T toward it by *t). post =
// the rotation SolveArmIk post-multiplies onto the commanded hand (authored rest x CorrectHandLocal dials).
bool TwoHandLeftPin(const float rightDrawn[9], const float rightAchieved[3], const float* hh12, const float grip[9],
                    const float post[9], float cWorld[9], float wristPos[3], float* t);
// Around SolveArmIk's one ClampWristSwing call: counts per arm while gripping. excess4 = degrees asked past each final
// (overflowed) limit, [flex, ext, ulnar, radial] (INV-502 (b) measurement; log-only).
void TwoHandNoteWrist(bool isLeft, bool pastLimit, bool stillClamped, const float excess4[4]);
// BulletInitHook's firearm branch: a player gun shot. Pulses both controllers while gripping.
void TwoHandNoteShot();

// HookTail: true = the right hand holds a two-handed MELEE weapon with the option on, so read its Holder world this update.
// Two bool tests + one interlocked read.
bool TwoHandWantsHolder();
// ItemCtlPlace, RIGHT slot, after itemrel::Get: while gripping melee, replaces relR / relT with the latched transform slid so
// Holder comes to the right handholder along the shaft (spec 7.2 item 3), blended by t. hhT = the right handholder offset in the
// hand bone frame. False = untouched (not melee, not gripping, option off). Logs the idle rel once per item (INV-499).
bool TwoHandMeleeRel(uintptr_t item, const float hhT[3], float relR[9], float relT[3]);
// ApplyDrivenBones, once per pass: true + t (0..1] while the left hand grips a melee weapon and the finger self-check has not
// refused. Off / not gripping: two bool tests + one interlocked read.
bool TwoHandLeftFingerT(float* t);
// Once per session per pin: the generated finger row for a pin name ("l_finger01" ...), -1 = none.
int TwoHandFingerRow(const char* pinName);
// Per left finger pin while TwoHandLeftFingerT: the pin's local for this pass. The first call per row compares the generated rest
// rotation with the authored pin and refuses the whole finger pose by name if they differ (> 0.02).
bool TwoHandLeftFingerLocal(int row, const float authored12[12], float t, float out12[12]);
// The l_hand1 pin (the palm bone) while TwoHandLeftFingerT: a gripped shotgun row with hasPalm1 (Jay's pump grip) -> its local for
// this pass (rotation = row.palm1 blended by t). False = leave the authored pin.
bool TwoHandLeftPalm1Local(const float authored12[12], float t, float out12[12]);
// The published "left hand gripping a two-handed MELEE weapon" state (spec section 1: a later heavy attack). Lock-free.
bool TwoHandGrippingMelee();
// The same published state for ANY kind (gun or melee): the left hand is on the weapon. Lock-free. handcoll.cpp: a gripping
// left hand is not an empty hand.
bool TwoHandGripping();

}  // namespace dlvr
