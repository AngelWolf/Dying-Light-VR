// Manual reload - spec docs/superpowers/specs/2026-11-01-manual-reload-design.md, plan
// docs/superpowers/plans/2026-11-01-manual-reload-pistol.md (Task 4: the [RELOAD] probe; Task 5: the steps driven).
// The hand steps are reload_logic.h (unit-tested). The driver is reload.cpp.
#pragma once

#include <cstdint>   // uintptr_t - ReloadWristFamily (stage B)

namespace dlvr {

extern bool  ManualReload;      // manual_reload - master. DEFAULT ON (was OFF, Tier C - Jay: "Decide after the pistol").
extern float ReloadSnapM;       // reload_snap_m      - a held fresh mag this close to the seat snaps in (Tier D)
extern float ReloadAlignDeg;    // reload_align_deg   - ... and pointed within this of the well (Tier D)
extern float ReloadPullOutM;    // reload_pull_out_m  - pulled this far along the well = out of the gun (Tier D)
extern float ReloadRackPullM;   // reload_rack_pull_m - the hand's travel back along the slide for a rack (Tier D)
extern float ReloadGrabM;       // reload_grab_m      - the left grip this close to the mag / slide grabs it (Tier D)
extern float ReloadFlickDps;    // reload_flick_dps   - the double-barrel's flick close: the muzzle's upward turn, deg/s (Tier D)

// GAME THREAD, the player-update tail (boneblend.cpp HookTail), after the bow's tail writes. playerVis = HookTail's argument
// (PlayerFppVis; +0x50 = PlayerDI). leftPos / leftR9 = the LEFT hand's world position and basis (row-major, axes as COLUMNS) the
// rig published - the ACHIEVED handholder + DRAWN hand basis when leftAchieved, else the commanded grip; nullptr when none.
// COST: manual_reload=0 -> one bool test. On, no pistol in the right hand: one held-item pointer read + one compare. With a pistol:
// per tail 2 guarded game calls (rounds / max), <= 4 element world reads, the step machine, and while a step draws a part one
// SetElementWorldMatrix; the spare count (2 game calls) only while the mag is out of the gun or once a second for [RELOAD] STATE.
// leftCmdPos = the COMMANDED left grip point (the controller's - the rig's published pos; the rack pull is read from it on a gun
// with a rack row), leftHolderT = l_handholder's translation in l_hand (the mag row's hand bone from the achieved handholder);
// either may be nullptr (today's measure / the grab's latch then).
// THE CROSSBOW (plan 2026-11-03-manual-reload-crossbow Task 5): playerModel = the captured player body (BoneProbeCapturedModel) and
// lHandId = its l_hand element (E6(17), checked against the name "l_hand" by the caller - the bow nock's r_hand rule), -1 = unresolved
// (no bolt is carried in the hand then). Read only while a crossbow bolt is carried / laid: one world-accessor read per update.
void ReloadTick(void* playerVis, const float* leftPos, const float* leftR9, bool leftAchieved, const float* leftCmdPos,
                const float* leftHolderT, void* playerModel, int lHandId);

// GAME THREAD, the player-update tail, INSTEAD of ReloadTick while manual_reload=0: a crossbow bolt still carried on l_hand goes home
// (or is forgotten when the held model is not its crossbow), and the crossbow's left-shoulder take / bolt-visibility flags drop.
// COST: four loads when nothing of the crossbow is live (always, unless manual_reload was switched off mid-reload).
void ReloadOffTick();

// THE PUMP SHOTGUN (plan 2026-11-04-manual-reload-pump-shotgun Task 4). Published by ReloadTick (game thread) while a manual pump is
// driven; stale after 150 ms (a pump put away / the tail stopped -> false). Any thread.
// ReloadPumpAnim: the C13 loading animation's cycle (1-4) and frame (1-36) the LEFT fingers + l_hand1 take (reload_hands.h PumpAnimFrame)
// - the playing cycle's frame, or the HOLD (cycle 1 frame 1) while shells are held and no cycle plays (ledger ruling R9); false = no
// pump anim (every other gun unchanged). PS2 smoothing: the frame is a FLOAT (1 .. 36), advanced from the publish to the reader's own
// time on a quarter-ms clock - blend frame floor / floor + 1 (reload_hands.h PumpFrameSplit). COST: one volatile load + one QPC call.
bool ReloadPumpAnim(int* cycle, float* frame);
// ReloadPumpStroke: while the two-hand grip holds the pump (the rack hold, plan P5): the pump's drawn stroke back, metres, and the pump's
// BACK axis as a unit vector in the gun's holderl frame (the two-hand pin's rel frame - twohand.cpp slides the pin target by
// axisHolderl x m). False = not gripping a manual pump. COST: one shared SRW acquire + a 16-byte copy.
bool ReloadPumpStroke(float* m, float axisHolderl[3]);
// Fix round 1 (Task 4 review C1) + final fix wave (final review I1, ruling R17): each arm solve (boneblend.cpp SolveArmIk) hands over its
// CONTROLLER's grip point in TRACKING space (T + wristBack - anchor = Ry(bodyYaw) x (controller - headRep)) BEFORE the two-hand pin moves
// the wrist target. The pump's rack stroke = (left - right) projected on the pump's back axis, from its value when the grip latched - so
// locomotion, turning and moving the gun with both hands do not rack. Kept only while a manual pump is driven; each side stale after
// 150 ms (a stale side HOLDS the stroke). Any thread. COST: off a pump one bool test + one volatile load; on one SRW acquire + 12 bytes.
void ReloadNoteController(bool isLeft, const float gripTrk[3]);
// The finger pass's own count while it writes the pump anim (the [PUMP] SHELL IN / 5 s lines). COST: two interlocked adds per pass.
void ReloadNotePumpFingerPass(int pinsWritten);
// THE DOUBLE-BARREL (2026-11-07): the animation (dbarrel_hands.h DbAnim as an int: 1 open grip, 2 pull-out, 3 load) and its FLOAT frame
// the LEFT fingers + l_hand1 take (reload::DbFrameAt), advanced to the reader's time at 60 fps unless held; false = none (stale after
// 150 ms). Any thread. COST: one volatile load + one QPC call.
bool ReloadDbAnim(int* anim, float* frame);
// The finger pass's own count while it writes the double-barrel's anim (the [DBARREL] lines). COST: two interlocked adds per pass.
void ReloadNoteDbFingerPass(int pinsWritten);

// The crossbow's bolt is carried or laid by the LEFT hand (crossbow plan Task 5, carry ruling T2 M-4): the solid-gun stop exempts the
// left hand as it does for a grip on the gun. Any thread. COST: one bool test + one volatile load.
bool ReloadXbowHandFree();

// ---- the LEFT hand's poses (plan docs/superpowers/plans/2026-11-01-reload-hand-poses.md Task 3) ----
// Read by the left arm's solve (tick thread / tail solve) and the finger-pin pass. All false when manual_reload=0, nothing is on,
// or the tail's publish is older than 150 ms. COST: off = one bool test (+ one volatile load with manual_reload on); on = one
// seqlock read (~20 words) + GetTickCount64, and for the two pins one LockedHand (a Gram-Schmidt + a 3x3 product).
// ReloadLockOn: the rack lock holds the left hand at all (blend > 0) - the two-hand pin is skipped (reload_hands.h TwoHandPinAllowed).
bool ReloadLockOn();
// Solid guns plan Task 5 (G2): the held mag's pose in the LEFT handholder frame (the drawn mag = H x rel, H = the ACHIEVED
// handholder position + the drawn hand basis, row-major 3x4) while a step draws it in the hand (Pulling / MagInHand / FreshInHand);
// false otherwise, with manual_reload=0, or when H was not the achieved one. drawnW12 = the mag's world as drawn this tick (H x rel)
// - the solve re-expresses it in its own commanded frame (review finding 3). ANY thread (RUN SG3: the pad-poll solve reads it) - a
// shared SRW lock around two 48-byte copies; ReloadTick publishes it once per tick under the exclusive lock.
bool ReloadMagInHand(float rel12[12], float drawnW12[12]);
// ReloadPinRot: cWorld slerped by the lock's blend to the one whose DRAWN hand is the rack row's hand on bone_chamber (INV-506).
bool ReloadPinRot(const float grip9[9], const float post9[9], float cWorld[9]);
// ReloadPinPos: the locked hand BONE's world position (the solve's wrist target) and the blend.
bool ReloadPinPos(float handPos[3], float* t);
// ReloadFingerPose: the left fingers' row as an INDEX (0 .. kRackRowsCount-1 = kRackRows, then kMagRows, then kPullRows, then
// kCarryRows - the crossbow's bolt fist, crossbow plan Task 5) and its
// blend 0..1 (the rack 100 ms on / off; the mag hold at once - reload_hands.h PoseEases). An index, never a pointer: reload_hand_poses.inc's const tables are a separate copy in every .cpp that
// includes them (HP1: a reload.cpp pointer never matched boneblend.cpp's copy and no finger was written).
bool ReloadFingerPose(float* t, int* rowIndex);
// The consumers' own counts for the [RELOAD] STATE line (proof the pose was APPLIED, not only published): ReloadFingersApply
// reports each pass that wrote pins. COST: one interlocked add per pass, only while a finger pose is on.
void ReloadNoteFingerPass(int pinsWritten);

// scePad poll (pad thread), BEFORE the vr_grip block, AFTER HolsterLeftPoll. Publishes the LEFT grip for the step machine and returns
// withholdL1 = true for a left-grip press that a reload step owns (the press began with the hand at the mag / slide or mid-reload),
// so the grip's L1 does not reach the game. COST: manual_reload=0 -> one bool test; on: one squeeze read + two volatile reads.
void ReloadPadPoll(bool vrGripOn, bool& withholdL1);

// Pad thread, after the throwable / loot rules: true while the held manual pistol must not fire (a reload step is live). Then the
// R2 byte is zeroed. COST: one bool test + two volatile reads. The crossbow: true in every step but Idle (xbow_logic.h withholdR2).
bool ReloadWithholdsR2();
// WRIST HUD stage B (trace docs/traces/20261113_1420_wrist_stageB_gun.md): the right hand's gun family name ("None", "Pistol", "Smg",
// "Rifle", "Crossbow", "Pump", "DBarrel", "Break6", "Gate", "Fuel" ...) as read for the held model *model (0 = nothing held), whatever
// manual_reload is. Read only while the wrist HUD is on this launch. *type = the item type read with it (18 Pistol, 21 Rifle, 24 Shotgun,
// 37 Crossbow, ...; 0 = not read) - the wrist key "<family>/<type>" (trace 20261113_1540). Any thread. COST: three volatile loads.
const char* ReloadWristFamily(uintptr_t* model, int* type);
// Jay after RUN XB1: true while a manual crossbow is held and not loaded - the left grip is not sent as L1 (pad thread; it does not
// claim the press from loot / push-grab). False with manual_reload off or no fresh crossbow update.
bool ReloadWithholdsL1();

}  // namespace dlvr
