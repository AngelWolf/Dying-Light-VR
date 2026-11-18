// The current head pose, for callers outside ovr2xr.cpp that need to express
// something relative to where the player is actually looking (see aimhook.cpp's
// controller-relative-to-head redirect math). ovr2xr.cpp owns the real state
// (s->headPose, written every frame from the two eye views under its own
// frame lock) - this is the one-line accessor for it, mirroring how
// controllers.h exposes GetControllerPose() for hand poses.
#pragma once

#include <openxr/openxr.h>

namespace dlvr {

// False if no session has ever located a view yet - outPose left untouched.
bool GetHeadPose(XrPosef& outPose);
// (NOTE 2026-11-15: GetHeadPose's code returns true once a SESSION exists, before any locate - the pose is then XrIdentity().)
// True from the first xrLocateViews with both valid bits, for the rest of the launch (B13, trace 20261115_1545).
bool HeadPoseLocatedOnce();
// The appSpace base (2026-11-16, trace 20261116_1450_hip_anchor_facing_v2): appSpace = the reference space turned by yawDeg about +Y
// and moved to pos (ShiftAppSpaceTo / a recentre absorb), so a point p in appSpace is ToWorldYaw(p, yaw) + pos in the reference
// space. Roomscale moves it every step. False = no session. Any thread; takes the frame Guard like GetHeadPose.
bool GetAppBase(float pos[3], float* yawDeg);
// The head's height above the runtime's floor (STAGE), metres (2026-11-16, trace 20261116_1545_standing_posture). False = no floor
// (no STAGE space, or it has not located yet). Any thread; the frame Guard.
bool GetEyeAboveFloor(float* metres);

// ===== HEAD-YAW DECOUPLE (docs/SPEC-HEAD-YAW-DECOUPLE-2026-09-26.md, R10) =====
// When active, ovr_GetTrackingState reports the head to the ENGINE with its yaw
// stripped (C1), so the engine's look - and with it the body, movement and the
// game's shot direction - turns only by the stick (INV-257/258). camhook puts
// the real yaw back on the rendered view (C2). Every place that pairs "game yaw"
// with "head yaw" must pair with the head the engine was GIVEN, which is
// LastReportedHead() - not GetHeadPose(), which is the real one.
//
// Latched ONCE, at the first ovr_GetTrackingState after startup: a live toggle
// would leave boneblend's [CTRL] constant K (latched once over 240 samples)
// calibrated against the other mode. Refused - stays off, logged by name - when
// use_render_pose=1, because that declares the ENGINE's (yaw-free) pose to OpenXR.
bool HeadYawDecoupleActive();

// The head orientation reported to the engine on the last ovr_GetTrackingState
// (horizon lock, the camera-scanner probe and - when decoupled - the yaw strip
// all applied). Also declared in settings.h.
const XrQuaternionf& LastReportedHead();

// The head POSITION of that same report (metres, the app's LOCAL space; neck pivot applied when on; with the decouple active,
// x/z turned by +folded - SPEC-HEAD-POSITION-FOLD-2026-10-19). Written at the same instant as LastReportedHead().
const XrVector3f& LastReportedHeadPos();

// SPEC-HEAD-POSITION-FOLD-2026-10-19: the degrees the BODY (root) has turned beyond the OpenXR->game mapping (the value
// boneblend's camyaw::follow publishes; 0 until the fold has run, 0 on a new player object, frozen while the switch is off).
// Since torso-from-controllers (docs/superpowers/specs/2026-10-29-torso-from-controllers-design.md §4.3) that is the folded head
// yaw plus the torso gap; without a torso reading it is the folded head yaw, as before. One interlocked read - safe from
// ovr_GetTrackingState's job workers.
float HeadFollowFoldedDeg();

// The yaw-only rotation (OpenXR, about +Y) that C1 removed from that same report.
// Identity when the decouple is not active. Written at the same instant as
// LastReportedHead(), so the two always describe the same frame.
const XrQuaternionf& LastStrippedHeadYaw();

// ===== HEAD NECK PIVOT (docs/SPEC-HEAD-NECK-PIVOT-2026-10-01.md) =====
// The head POSITION the engine is given, for a raw head pose from GetHeadPose(): with head_neck_pivot on,
// p_eye - (R_head - I) * n (the head-rotation arc removed); off / refused, the raw position - so OFF is today.
// Every consumer that adds R*(ctl - head) to an ENGINE anchor (hspine1/eyecamera/camera) must subtract THIS,
// not the raw position, or it reintroduces the arc with the opposite sign. Tracking-space consumers keep raw.
XrVector3f NeckPivotReportedPos(const XrPosef& rawHead);

// n itself (head_neck_pivot_x/y/z_mm in metres, head frame: neck -> eye), whether or not the pivot is on - the holster body-lock's
// neck point (eye - R n) uses the player's measured n (INV-321).
XrVector3f NeckOffsetM();

}  // namespace dlvr
