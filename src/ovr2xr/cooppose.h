// cooppose.h - the co-op arm-pose channel's SENDER: this player's solve inputs, in the body frame, handed to the transport
// ([COOPPOSE]). Plan docs/superpowers/plans/2026-11-13-coop-arm-pose-channel.md Task 4; trace docs/traces/20261113_1850_coop_sender.md.
// The receiver (plan Task 6) will live here too.
#pragma once

namespace dlvr {

// SolveArmIk, once per side per solve, after the anchor is built - only while coop_pose is ON. The solve's own quantities, unchanged:
// bodyYaw (the tracking -> world mapping yaw), rootYaw (atan2 of element 0's column 0, x / z), rootPos (element 0's world translation),
// anchorRaw (the eye anchor, world), headQ (the headset orientation, tracking, xyzw), ctlPos / ctlQ (the controller pose the solve
// aims at, tracking), headRep (the head position the solve subtracts, tracking). Any thread; one exclusive SRW lock.
// INV-736 build (trace 20261113_2150): wristBack = the solve's step-back vector (world, INV-161; zero before the grip is known);
// handRot9 = the hand BONE's commanded world rotation (handBasisW x rest x correction, row-major) or nullptr when the solve has none yet.
void CoopPosePublishSide(bool isLeft, float bodyYaw, float rootYaw, const float rootPos[3], const float anchorRaw[3],
                         const float headQ[4], const float ctlPos[3], const float ctlQ[4], const float headRep[3],
                         const float wristBack[3], const float* handRot9);

// SolveArmIk, once per side per solve, after the elbow E and the achieved wrist are final (trace 20261115_1735_coop_arms_world, item 5)
// - only while coop_pose is ON, under the same condition as CoopPosePublishSide. S / E / wrist are the solve's world points, read only;
// rootYaw is the same root yaw. Stores the bend direction in the body frame, or "straight". Any thread; one exclusive SRW lock.
void CoopPosePublishElbow(bool isLeft, float rootYaw, const float S[3], const float E[3], const float wrist[3]);

// After each solve PAIR (tail and pad poll), on the solving thread. OFF = one bool test. ON: builds one PoseSample from the newest
// publishes + finger weights + hand state + netId and queues it (CoopNetQueueSend, latest wins).
void CoopPoseCaptureLocal();

// Protocol 5 (trace docs/traces/20261117_1905_coop_full_ik.md): at the very END of boneblend's HookTail - after every arm / finger write
// of the player update - read the sender's FINISHED local rotation of the 25 arm bones per side (coop::kArmBoneSuffix) from the captured
// player model (GetElementLocalMatrix, read only) and store them for the next CoopPoseCaptureLocal. A side is stored only when all 25
// resolve by name on this model and every matrix is sane (coopik::RotSane). OFF = one bool test. Game thread.
void CoopPoseCaptureArmBones();

// THE RECEIVER (spec 8.3; trace docs/traces/20261113_2045_coop_receiver.md): at the EXIT of PlayerTppVis slot 129 (+C532D0 - a
// partner's body; co-op only), after the original. Poses that body's arms (both, INV-736 build) and hands on the SENDER's yaw, with the
// sender's elbow bend (trace 20261115_1735), and re-places its held items (CorrectItemPosition x2), from the buffered sample of the peer
// bound by position. The head is left to the game (rx::PoseHead has no caller). OFF = one bool test.
// Protocol 5 (trace 20261117_1905): first turns the body's spine / spine1 / spine2 toward the sender's yaw (capped), restores the
// neck's world (the head stays the game's), then a side whose sample carries the sender's bones gets all 25 of them and its wrist
// landed on the target; a side without bones keeps the old solve.
void CoopPoseApplyRemote(void* vis);

}  // namespace dlvr
