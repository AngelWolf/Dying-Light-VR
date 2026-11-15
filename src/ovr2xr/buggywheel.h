// buggywheel - the buggy's hands: turn the key to start it, grip the wheel to steer (spec
// docs/superpowers/specs/2026-11-10-buggy-wheel-and-key-design.md, plan docs/superpowers/plans/2026-11-10-buggy-wheel-and-key.md
// Task 2). Two gamedll hooks: the steer-input writer +D99730 (detour; after the original: hold / turn / start) and CarBuggyVis vtable
// slot 129 +108A9E0 (after the original: read the wheel and the hands, place the wheel, publish the hand pins). Decisions:
// buggywheel_logic.h. Facts: INV-715, INV-716 (+ addenda 1-5). See buggywheel.cpp.
#pragma once

namespace dlvr {

// buggy_hand_steer - the master (VR Comfort tab). Shipped ON (Jay's Tier C pick, 2026-11-10 (14)).
extern bool BuggyHandSteer;

// Install both hooks (the input detour first, then the vis slot). Idempotent; camhook's 1 Hz thread.
void BuggyWheelInstall();

// The rig's arm solve (Task 3): while a hand grips the wheel or the key, the GRIP pose that hand should be drawn at, in the same
// frame and convention as the rig's own published grip (boneblend.cpp MeleeGripSnapshot: pos = world grip position, cWorld =
// row-major basis, grip axes as COLUMNS). side: 0 = RIGHT, 1 = LEFT (the rig's kGripSide order). *pinRot false = position only
// (the key: the hand turns with the real wrist). False = not pinned (or stale > 150 ms). COST: one seqlock read.
// *mode (RUN BK4): kPinKey = the key (pos = the GRIP point, position only); kPinGrip = a wheel grip as a grip pose (pos = the grip
// point, cWorld = the GRIP basis - only when the body frame was unreadable at the grab); kPinHand = the wheel HOLD (Jay: "Use the hold
// you had in the render earlier"): pos = the HAND BONE origin, cWorld = the DRAWN hand bone's world rotation (the clip's frame 5).
// kPinRim (RUN BK6): a wheel grip - pos = the RIM point the hand holds (the rim tube's centre at the angle grabbed), cWorld = the
// GRIP basis; the arm solve sets the hand so its kWheelRimFromGrip point (buggy_poses.inc) lands there.
// kPinRimHold (RUN BK12; Jay BK11: "I felt like I had to break my actual wrist to get the right placement" -> "Yes"): a wheel grip -
// pos = the RIM point at the angle grabbed (the palm's grip centre goes on it, as kPinRim); cWorld = the DRAWN hand's world rotation
// = the rim frame at the grab x the pass hold (kWheelHoldInRim, from Jay's passes) - the controller's rotation is NOT used. The arm
// solve takes cWorld by kPinHand's route (StandardCWorld, INV-506) and the position by kPinRim's step-back.
enum BuggyPinMode { kPinKey = 0, kPinGrip = 1, kPinHand = 2, kPinRim = 3, kPinRimHold = 4 };
bool BuggyHandPin(int side, float pos[3], float cWorld[9], int* mode);

// RUN BK10 (Jay: "Yes, add it"), LOG-ONLY: the arm solve's DRAWN grip point (achieved wrist + drawn basis x handholder) and drawn hand
// basis while a wheel pin (kPinRim) holds; 200 ms into each wheel grab, one [BUGGY] DRAWN GRIP line: its offset from the pin in the
// rim's frame, the arm's asked vs drawn reach, the drawn hand in the rim's frame. side: 0 = RIGHT, 1 = LEFT. COST per pinned solve:
// one seqlock read + an interlocked exchange; the log once per grab.
void BuggyNoteDrawnGrip(int side, const float grip[3], const float drawnW[9], float askedM, float drawnM);

}  // namespace dlvr
