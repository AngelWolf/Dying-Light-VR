// The arm fit, stage 1 (docs/SPEC-ARM-FIT-2026-10-02.md, INV-545): a guided calibration that MEASURES each arm's length and
// shoulder pivot. Started from the VR Comfort tab; four held poses; a sphere fit per arm; the result is written to
// dlvr\arm_fit.txt and logged under the ARMFIT tag. STAGE 1 IS MEASURE-ONLY - nothing reads the file yet.
#pragma once

#include <cstddef>

namespace dlvr {

// Overlay (render thread): start / cancel a calibration. Start while one runs restarts it.
void ArmFitStart();
void ArmFitCancel();

// Solve threads, every solve: one volatile read. True only while a calibration is collecting.
bool ArmFitWanted();
// True until this calibration has been given the arm's live bone lengths (so the solve reads them once, not per solve).
bool ArmFitNeedsBones(int side);

// Solve threads, after SolveArmIk's values are final (boneblend.cpp, beside the ARMCAP capture). World frame; side 0 = R.
// anchorRaw = the anchor before the view offsets (cam_eye_forward / cam_eye_up_mm); wristBack = T's offset from the grip point.
// handOk = the solve had a hand axis; held = a pin (two-handed grip, bow string) moved T off the controller. boneSum < 0 =
// not read this call.
// handB = the commanded hand basis from the CONTROLLER (world, columns = axes, before any bow lock); nullptr = not available.
void ArmFitNote(int side, const float anchor[3], const float anchorRaw[3], const float S[3], const float T[3],
                const float wristBack[3], float bodyYaw, bool handOk, bool held, float boneSum, const float* handB);
// The forearm's direction IN THE HAND'S FRAME (unit), measured by the calibration's straight-arm poses (2026-10-13, INV-558):
// forearm(world) = handBasis . v. False = not measured (older file / no calibration). COST: a shared lock, three copies.
bool ArmFitForearmInHand(int side, float v[3]);

// Overlay: what to show. Returns true while a calibration runs; `line1` / `line2` are filled either way (the idle text is
// the last result or "not calibrated").
bool ArmFitStatus(char* line1, size_t n1, char* line2, size_t n2);

// ---- Stage 2 (Jay 2026-10-12: "Yes, set it to 1.0 for calibrated players, go ahead") - the arm LENGTH. ----
// rig_arm_fit: the switch. With it on AND a valid dlvr\arm_fit.txt, the arm bones are scaled to the player's measured length
// (same percent on the upper arm and the forearm) and rig_reach_scale is treated as 1.0. No file = today's arms exactly.
extern bool RigArmFit;
// Solve threads, every solve while rig_arm_fit is on: the scale for this arm. liveUpper / liveFore = the CURRENT model's bone
// lengths (49's and 53's local |t|; <= 0 = could not be read). Returns true and sets *k (clamped 0.85..1.25) only when a
// calibration applies; otherwise *k = 1 and the arm is unchanged. Logs only when the answer changes. COST: a shared lock,
// a divide, a compare.
bool ArmFitScale(int side, float liveUpper, float liveFore, float* k);

// ---- Stage 3a (Jay 2026-10-12: "Follow the reach first") - the shoulder follows the reach. ----
// rig_arm_fit_follow_cm: how far (cm) the shoulder may slide toward the hand when the hand asks for more than the calibrated
// arm's reach. 0 = off. Acts only while rig_arm_fit applies a calibration.
extern float RigArmFitFollowCm;
// rig_arm_fit_straight_cm (stage 4): with a calibration applied - no reach ease, a <= 0.5 mm margin, and the last N cm before full
// reach blended to a straight arm. 0 = off (the shipped ease and margin).
extern float RigArmFitStraightCm;
// Solve threads: publish this arm's shoulder offset for the upper-arm translation writes, in the CLAVICLE's local frame
// (metres). moveM = its length in the world (for the log). A zero offset is published as "off".
void ArmFitShoulderPublish(int side, const float local3[3], float moveM, float capM);
// The four upper-arm restore sites (UpperRestoreVector, ~19,000 calls a second): the offset to ADD to the written translation.
// False = none (off, never published, or older than 200 ms - the solve stopped). COST: two volatile reads, a tick read,
// three float copies.
bool ArmFitShoulderDelta(int side, float out3[3]);

// ---- Stage 3c (Jay 2026-10-13: "We'll try B", "Go with ii & 0.5") - the shoulder ROLLS with the reach. ----
// rig_arm_fit_shoulder_cm: with rig_arm_fit applying a calibration, the clavicle is held at the fixed melee pose (the
// rig_clavicle_hold machinery, INV-296) and ROTATED about its own origin so the rig's shoulder covers the distance by which the
// rig's reach runs ahead of the player's (|T - S| - |T - pivot|), eased in from 0.5 of the player's extension to 1.0, capped at
// this many cm. 0 = off. Not while a bow is held (INV-544: those arms are verified) or for an arm a pin places.
extern float RigArmFitShoulderCm;
// The calibration's pivot for this arm, body frame (fwd, up, OUT; metres, from the eye anchor, the left mirrored), its fitted
// length, and the length the ARM uses (the anatomical one under rig_arm_fit_anatomical when measured, else the fitted one).
// False = no calibration (or rig_arm_fit off). COST: a shared lock and eight float copies.
bool ArmFitPivot(int side, float pivotBody[3], float* lenM, float* rigLenM);
// S1 (2026-10-13): the arm at the player's anatomical length (the calibration's side pose); the roll supplies the rest.
extern bool RigArmFitAnatomical;
// The shoulder roll's clavicle angle limit, degrees (33 = VRArmIK's; clamped 10..60 at use).
extern float RigArmFitShoulderMaxDeg;
// The shoulder roll's status line for the log: first engagement and each new 1 cm maximum per arm, plus refusals by name.
// wantM / gotM = the distance asked / achieved this solve, deg = the clavicle angle, ext = the player's extension.
void ArmFitRollNote(int side, float wantM, float gotM, float deg, float ext, float capM, bool limited);

// ---- Calibration screen (build A, 2026-10-14) - the UNTIMED step mode. The timed seven-pose ArmFitStart is unchanged. ----
// Start a step-mode run: the four fit poses only, no timer; nothing is collected until ArmFitBeginPose + ArmFitCollect(1).
// Build C: seated = the seated set - pose 3 is "down and out at 45 deg", and no rest hand is measured (the loaded one is kept).
void ArmFitStartStep(bool seated);
// The pose now on screen (0 forward, 1 side, 2 up, 3 down - or down 45 out on a seated run).
void ArmFitBeginPose(int pose);
// 1 = start storing (grips held), 0 = DISCARD this pose's samples on both arms (grips released), 2 = stop and keep (pose done).
void ArmFitCollect(int mode);
// The current pose's accepted counts and each arm's live verdict (0 none, 1 OK, 2 not extended, 3 wrong direction,
// 4 not tracked; 0 if older than 300 ms). Render thread. COST: a shared lock, four reads.
void ArmFitPoseProgress(int* nR, int* nL, int* vR, int* vL);
// Fit + write (the same Finish() as the timed run). Returns 1 written, -1 refused; `why` gets the reason line.
int ArmFitStepFinish(char* why, size_t n);

// ---- Calibration screen, build B (2026-10-14) - the RAW-pose path (calibraw_logic.h). ----
// Which store a step run fits and writes: 0 = in-level (today; the raw store is fitted too and compared - [CALIBEQ]),
// 1 = raw only (the main menu: no solve runs). Set before the run's first pose.
void ArmFitSetStepSource(int source);
// One raw sample (render thread, once per submit while a step run shows a pose). T and handB in the body frame. Applies the
// stand-in acceptance (ruling B1) and build A's pause / cap rules to the RAW store; sets the raw verdict.
// holsterLocal = the GRIP point from the RAW head, head yaw removed (holster::LocalOffset), or nullptr: the down pose's are
// kept as the resting hand for holster_hip_from_calib.
void ArmFitNoteRaw(int side, const float T[3], const float handB[9], const float* holsterLocal);
// This hand's pose is not valid this frame: the raw verdict reads "not tracked" (never a stale pose).
void ArmFitNoteRawUntracked(int side);
// A valid dlvr\arm_fit.txt is loaded (both arms within 0.40..0.90 m). Loads it on first call. COST: one exclusive lock.
bool ArmFitHaveCalibration();
// holster_hip_from_calib (Jay 2026-10-15): this arm's resting hand (the calibration's down pose, grip point from the RAW head,
// head yaw removed, +X right +Y up +Z behind, metres). 1 = out filled; 0 = no valid calibration loaded; -1 = a calibration but
// no rest hand for this arm (a file written before 2026-10-15, or the down pose took too few raw samples). *seq (optional) =
// the calibration's change counter, so a caller re-logs when a new calibration lands. Loads the file on the first call.
// COST: one shared lock and three float copies (the first call per launch also reads the file).
int ArmFitRestHand(int side, float out[3], long* seq);

}  // namespace dlvr
