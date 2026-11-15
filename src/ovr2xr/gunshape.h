// gunshape.h - SOLID GUNS at runtime: the held firearm is a solid to the OFF (left) hand, all the time (spec
// docs/superpowers/specs/2026-11-02-solid-guns-reload-sound-haptics-design.md G1; plan Task 4). The shapes are the generated table
// (gun_shapes.h); the stop is gunshape_logic.h (unit-tested). Rides on hand_collision (no new key - spec 6.5).
#pragma once

namespace dlvr {

// Game thread, once per player update, in HookTail AFTER ItemCtlPlace and ReloadTick (RUN SG2: before the tail solve the item still
// holds the ENGINE's CorrectItemPosition pose, not the VR-placed gun). `ent` = HookTail's PlayerFppVis (unused but kept for the same
// call shape). Reads the right hand's held model, finds its shape row once per model change, and poses it for the NEXT update's tail
// solve (the frame node's world, the seated mag's world, the parts' hidden flags) - one update behind the right hand.
// COST: hand_collision=0 -> one bool test. On, no row: one pointer read + one compare. With a row: 1 + (seated mag ? 1 : 0) element
// world reads, one IsElementHidden per part (<= 8), all SEH-guarded game calls.
void GunShapeTick(void* ent);

// THE HAND'S SHAPE (Jay 2026-11-02: "consider the players hand & fingers"). The write pass (boneblend.cpp, after the left fingers'
// pose writes) publishes the LEFT hand's joints - each joint's matrix in l_hand's frame, hand_shape_table.inc's kHandJointNames
// order (jm[0] = identity) - from the game's own finger locals. COST: one exclusive SRW acquire + a 17 x 12 float copy (the caller
// throttles); the first publish logs the FK check once.
void GunShapeNoteHandJoints(const float jm[][12], int n);
// SolveArmIk, LEFT arm, where the drawn hand basis is final: the commanded basis (handBasisW) and the drawn one. The hand's spheres
// are placed in the commanded frame turned by their difference (the drawn hand is what Jay sees; review finding 3's lesson for the
// mag). COST: one exclusive SRW acquire + a 3x3 product.
void GunShapeNoteDrawnHand(const float cmd9[9], const float drawn9[9]);

// Inside SolveArmIk, AFTER HandCollisionStop and before the reach clamp. armIdx 0 = right, 1 = left; S = the shoulder (unused since
// the ghost), T = the wrist target (game world), tipOff = wrist -> fingertip, hand9 = the hand's commanded basis (handBasisW; nullptr =
// not known); inGrip = the left hand is held ON the gun by a grip (the two-hand grip, the reload's rack lock). True = T moved.
// THE GHOST HAND (2026-11-02, D-309's replacement - RUN SG5's seizure): ONE wrist, kept in the GUN's frame, advanced ONE STEP AT A
// TIME by whichever solve takes gunshape.cpp's step lock (D-310: no thread identity - the solve rotates over the engine's job threads,
// INV-282) with gunshape_logic.h GhostStep - collide-and-slide, the hand's own turn walked back along the last contact normal, never a
// push along the nearest face. A solve that finds a step running reads the last result: while the ghost touches, its T is put at
// the ghost. The hand = its game-mesh spheres on the live finger joints turned by the ONE shared drawn-vs-commanded q (joints
// < 0.5 s old, hand9, q known), else one 5 mm sphere at the fingertip point (said once with the reason). During a reload's mag step
// the hand is free at the well's mouth (kMouthMargin). One pulse on the left controller per touch.
// COST: a reader -> one TryAcquire + one SRW acquire + a point transform; a step -> as GhostStep (only while the hand is within
// reach of the gun - a bounding test skips the rest). Never waits on the step lock.
bool GunShapeStop(int armIdx, bool inGrip, const float S[3], const float tipOff[3], const float* hand9, float T[3]);

// Inside SolveArmIk right after GunShapeStop (spec G2, plan Task 5): while a manual-reload step holds the mag IN THE LEFT HAND
// (reload.h ReloadMagInHand), the mag - posed at THIS solve's hand target (hand9 = the hand bone's world basis, axes as columns;
// holderPos = the handholder target, T + wristBack) - is stopped by the gun (gunshape_logic.h StopBoxOnHull: walls guide it up the
// well within reload_align_deg, the lip refuses it beyond, the body / receiver push it out); T moves by the same shift, so the hand
// and the mag stop together. One pulse per touch. Left arm only, any thread (RUN SG3), under gunshape.cpp's lock.
// COST: no mag in the hand -> a few tests; else one OBB-vs-box separating-axis test per gun box (x up to 4 passes).
bool GunShapeMagStop(int armIdx, const float hand9[9], const float holderPos[3], float T[3]);

// The held-mag WELL RAIL (reload.cpp, magrail_logic.h - Jay 2026-11-02 (4)): how far this mesh's seated mag travels out of its well
// until its top clears the open end (gun_shapes.h SeatedMagClearM = the generator's well depth). Any spelling of the mesh name.
// False = no row / no well / no mag for this mesh (the reload keeps its old free mag). COST: one table scan by name - call once per
// gun change, never per frame. Pure table read: no lock, any thread.
bool GunShapeMagClearM(const char* mesh, float* clearM);

}  // namespace dlvr
