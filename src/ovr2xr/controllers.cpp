#include "controllers.h"

#include "xrloader.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/log.h"
#include "steady_logic.h"
#include "uipin.h"          // uipin::GamePausedForMenu - the finger weights held in a pause (trace 20261113_1655)

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

// boneblend.cpp (FingerTrackApply's side): passes that wrote, and the last skip reason. side 0 right, 1 left.
void FingerTrackWriteStats(int side, long long* passes, long* reason);

// [DEP: comfort_hand_steady] required-for=hand steadiness, the VR Comfort tab (ComfortHandSteady). SPEC-VR-COMFORT
//      §6, C1; Jay 2026-09-30 "Start C1, hand steadiness". tree=LOADBEARING while 1: the rig's hand input reads the
//      steadied grip pose (boneblend.cpp SolveArmIk, except while the right hand holds melee), and with
//      fire_aim_from_barrel=1 the shot, the reticle and grapple_aim_from_gun all come from that drawn gun; aimhook's
//      controller-aim paths and grappleaim's hand-aim reference read the steadied aim pose. 0 = the steady copy IS the
//      raw pose, bit for bit. DEFAULT 1. COST: a few dozen flops per hand per pose per frame, in ControllersLocate.
bool ComfortHandSteady = true;
// [DEP: comfort_hand_steady_strength] required-for=the filter's still-hand cutoff (ComfortHandSteadyStrength):
//      steady::FromSliders, 0 = 5 Hz (light) .. 100 = 0.3 Hz (heavy), 50 = 1.22 Hz. tree=LOADBEARING with the above.
int  ComfortHandSteadyStrength = 50;
// [DEP: comfort_hand_steady_response] required-for=how fast the cutoff rises with hand speed
//      (ComfortHandSteadyResponse): both betas x0.1 (0) .. x1 (50) .. x10 (100). tree=LOADBEARING with the above.
int  ComfortHandSteadyResponse = 75;   // DEFAULT 75 2026-10-01 (Tier D: Jay's live tuning)
// [DEP: comfort_melee_steady] required-for=the VERY slight melee filter (ComfortMeleeSteady), Jay 2026-09-30: "a very
//      SLIGHT filtering on for the melee weapons ... to stop the most MINUTE of hand movements". steady::MeleeFromSlider:
//      the max-response curve, still cutoff 3.28 Hz at 15; swing-speed lag ~2.2 mm / 0.83 deg (steady_logic_test).
//      Read by the rig INSTEAD of the raw pose while the right hand holds melee - the swing's hit sweep reads that solve,
//      the swing DETECTOR still reads raw. 0 = OFF (the melee copy IS the raw pose). tree=LOADBEARING while > 0 and melee
//      is held. DEFAULT 15. Independent of comfort_hand_steady.
int  ComfortMeleeSteady = 10;   // DEFAULT 10 2026-10-01 (Tier D: Jay's live tuning; steady::kMeleeDefaultStrength 15 is the unit-test reference)
// [DEP: rig_hand_offset_r_fwd_cm] [DEP: rig_hand_offset_r_up_cm] [DEP: rig_hand_offset_r_right_cm] [DEP: rig_hand_offset_l_fwd_cm]
// [DEP: rig_hand_offset_l_up_cm] [DEP: rig_hand_offset_l_right_cm] required-for=the controller grip offset (RigHandOffsetRFwdCm ..
//      RigHandOffsetLRightCm), Jay 2026-11-16 (testers: "a grip offset slider would be very helpful"; "Move the controller"). Per hand,
//      in the GRIP frame: right = grip +X (the player's right on both hands, INV-727), up = +Y, forward = -Z (INV-627). ControllersLocate
//      adds the world delta to the RAW grip AND aim positions before the steadiness filters, so every getter reader sees the shifted
//      point; wristquad adds the grip-local offset to quads it submits in the raw grip space. Trace
//      docs/traces/20261116_1750_grip_offset_sliders.md. tree=FEATURE. 0 = the branch is skipped (poses exactly as the runtime's).
//      DEFAULT 0 (Tier D: per-player tuning).
float RigHandOffsetRFwdCm   = 0.0f;
float RigHandOffsetRUpCm    = 0.0f;
float RigHandOffsetRRightCm = 0.0f;
float RigHandOffsetLFwdCm   = 0.0f;
float RigHandOffsetLUpCm    = 0.0f;
float RigHandOffsetLRightCm = 0.0f;

// The grip-local offset in metres (x = right, y = up, z = -forward: the grip frame's own axes). False when it is all zero.
bool RigHandOffsetLocalM(Hand hand, float out[3])
{
    const bool l = hand == kLeftHand;
    out[0] = 0.01f * (l ? RigHandOffsetLRightCm : RigHandOffsetRRightCm);
    out[1] = 0.01f * (l ? RigHandOffsetLUpCm : RigHandOffsetRUpCm);
    out[2] = -0.01f * (l ? RigHandOffsetLFwdCm : RigHandOffsetRFwdCm);
    return out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
}
// [DEP: diag_steady] required-for=[STEADY] per-second windows per hand (DiagSteady) - raw vs steadied frame-to-frame
//      change of the GRIP pose's forward axis on STILL frames, and the worst raw-to-steady angle (the lag).
//      tree=PROBE-ONLY. Log-only; the window's sums are a few adds per frame, the LOG once a second. The [STEADY]
//      ARMED line logs regardless. HEADSET: controller poses exist only with a live OpenXR session. DEFAULT OFF.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in controllers.h (docs/BETA_CENSUS.md)
bool DiagSteady = false;
#endif
// [DEP: index_grip_force] required-for=which Valve Index input drives the grips (IndexGripForce) - 1 = squeeze/force
//      (the grip's force sensor: rests at 0 until the hand actually squeezes), 0 = squeeze/value (largely capacitive -
//      Blender T93509 and VRChat report it reading high with a relaxed hand / one or two fingers resting on the grip).
//      Either way, on Index ONLY, the value passes a 0.5 on / 0.3 off hysteresis before GetControllerSqueeze hands it
//      out (ConditionIndexSqueeze). Touch and every other profile are untouched, bit for bit. Read every sync, so it
//      switches live. tree=LOADBEARING while an Index profile is active (L1/R1, both holsters, the two-handed grab all
//      read GetControllerSqueeze). DEFAULT 1 (2026-10-14, Jay: "Index-specific bindings" - was unverified, Tier C until an
//      Index tester confirmed; Tier A since 2026-10-17).
bool IndexGripForce = true;
// [DEP: finger_track] required-for=controller finger tracking on an EMPTY hand (FingerTrack) - spec 2026-11-11-controller-finger-
//      tracking-design: index = trigger TOUCH, thumb = any thumb touch (stick / A,B or X,Y / thumbrest), middle + ring + little =
//      squeeze/value, eased between Jay's OPEN / CLOSED shapes (finger_track_poses.inc) by FingerTrackApply (boneblend.cpp). The
//      touch actions are bound at startup whatever this says (a refused suggestion falls back to the old binding set); this gates
//      only the finger WRITE, read live. tree=FEATURE (writes the finger pins of an empty hand). HEADSET: controllers only.
//      DEFAULT ON (Tier A, 2026-11-11: RUN FT2, Jay "Done, that's better" + ticked empty-hand fingers follow / firearm 2-hand
//      hold kept / melee 2-hand hold kept / held grips take over; was OFF Tier C until then).
bool  FingerTrack = true;
// [DEP: finger_track_ease_ms] required-for=how long a finger takes to go fully open <-> closed (FingerTrackEaseMs). Tier D. DEFAULT 100.
float FingerTrackEaseMs = 100.0f;
// [DEP: finger_track_fist_at] required-for=the squeeze/value at which middle / ring / little reach the full fist (FingerTrackFistAt);
//      the curl rises from 0 at no press. Tier D. DEFAULT 0.6.
float FingerTrackFistAt = 0.6f;
// [DEP: diag_ctl_input] required-for=[CTLIN] - every 2 s per hand: the active interaction profile, the aim and grip
//      poses' location flags, and the window min / max of trigger, squeeze value, squeeze force and trackpad force
//      (DiagCtlInput) - the resting values an Index tester's hands actually produce. tree=PROBE-ONLY. Log-only; a few
//      compares per hand per frame in ControllersSync, the LOG every 2 s. The [CTLIN] ARMED line logs regardless.
//      HEADSET: action states exist only with a live OpenXR session. DEFAULT OFF.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in controllers.h (docs/BETA_CENSUS.md)
bool DiagCtlInput = false;
#endif

namespace {

XrPosef Identity()
{
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

bool Ok(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    LOG("controllers: %s failed: XrResult %d", what, static_cast<int>(r));
    return false;
}

XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_aimPose = XR_NULL_HANDLE;
XrAction g_trigger = XR_NULL_HANDLE;
XrAction g_stick = XR_NULL_HANDLE;
XrAction g_stickClick = XR_NULL_HANDLE;
// Grip/squeeze - analog, same shape as trigger. Added 2026-09-03 for the
// still-unbound grip buttons (see controllers.h's own comment).
XrAction g_squeeze = XR_NULL_HANDLE;
// Vibration output (the holster's pulses, SPEC-SHOULDER-HOLSTER §3). One action, left/right subaction paths.
// Pulses are QUEUED by PulseController (any thread) and APPLIED in ControllersSync (frame thread).
// g_hapticPending packs (amplitude*1000) << 32 | durationMs; 0 = nothing pending.
XrAction g_haptic = XR_NULL_HANDLE;
volatile LONG64 g_hapticPending[2] = { 0, 0 };
// Game rumble (SetGameRumble, fed by the bootstrap's scePadSetVibration hook through dlvr_PadVibration; spec
// 2026-10-25-game-rumble-to-touch-haptics-design): the stronger of the pad's two motors x 1000, sent to BOTH hands every frame
// while non-zero. A hand with its own pulse pending (holster / bow / two-hand) gets ours that frame. 0 = off.
volatile LONG g_gameRumbleMilli = 0;
constexpr int kGameRumblePulseMs = 50;   // re-issued every frame, so the vibration ends within 50 ms of the game's 0
volatile LONG g_hapticOk[2] = { 0, 0 };
volatile LONG g_hapticFail[2] = { 0, 0 };
volatile LONG g_hapticLast[2] = { 0, 0 };
// Left controller's menu button - inherently single-hand, same treatment as
// the face buttons below (no subaction path, bound to one concrete input).
// Added 2026-09-03.
XrAction g_menu = XR_NULL_HANDLE;
// Face buttons - each is inherently single-hand (A/B live only on the right
// Touch controller, X/Y only on the left), unlike aim_pose/trigger/stick
// above which are genuinely symmetric. Created with zero subaction paths and
// bound directly to one concrete input path each, rather than forcing a
// left/right pair that would only ever have one live half.
XrAction g_button[4] = { XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE };
XrPath g_handPath[2] = { XR_NULL_PATH, XR_NULL_PATH };
XrSpace g_handSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
XrPosef g_pose[2] = { Identity(), Identity() };

// THE GRIP POSE - a second, anatomy-defined pose per hand. See controllers.h
// for why the rig needs this rather than the aim pose. Entirely parallel to
// the aim state above: its own action, its own spaces, its own seqlock. The
// aim path is untouched.
XrAction g_gripPose = XR_NULL_HANDLE;
XrSpace g_gripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
XrPosef g_gripPoseVal[2] = { Identity(), Identity() };
volatile LONG g_gripSeq[2] = { 0, 0 };
bool g_gripValid[2] = { false, false };
// The grip's raw XrSpaceLocation::locationFlags from the latest locate, VALID or not - read by the swing driver's
// candidate-path instrument (swing.cpp, Stage 1 redo 4) to tell a TRACKED position from an inferred one. A plain
// 32-bit store, read separately from the pose: diagnostic only, never a gate. -1 = never located.
volatile LONG g_gripFlags[2] = { -1, -1 };

// Hand steadiness: one filter per hand per pose, and the published steady copies - written INSIDE the same seqlock
// write as the raw pose (g_poseSeq / g_gripSeq), so GetController*Steady is exactly as tear-free as the raw getters.
// Filter state is frame-thread only (ControllersLocate).
steady::PoseFilter g_steadyAim[2];
steady::PoseFilter g_steadyGrip[2];
XrPosef g_poseSteady[2] = { Identity(), Identity() };
XrPosef g_gripSteady[2] = { Identity(), Identity() };
// The melee copies (comfort_melee_steady) - the same arrangement, their own filters and parameters.
steady::PoseFilter g_meleeAim[2];
steady::PoseFilter g_meleeGrip[2];
XrPosef g_poseMelee[2] = { Identity(), Identity() };
XrPosef g_gripMelee[2] = { Identity(), Identity() };
XrTime  g_lastLocateTime = 0;

// [STEADY] window, per hand, on the GRIP pose (what the rig - and so the drawn gun and the shot - uses). Frame thread.
struct SteadyWin {
    long long frames = 0, still = 0;
    double rawStep = 0.0, steadyStep = 0.0;   // summed frame-to-frame forward-axis change on STILL frames, degrees
    float maxLag = 0.0f;                      // worst raw-vs-steady forward-axis angle this window, degrees
    double meleeStep = 0.0;                   // the same, for the melee copy
    float meleeMaxLag = 0.0f;
    bool havePrev = false;
    float prevRaw[3] = {}, prevSteady[3] = {}, prevMelee[3] = {};
};
SteadyWin g_sw[2];
ULONGLONG g_swStart = 0;
bool g_steadyArmedSaid = false;
// A frame counts as STILL below this filtered rotation speed: 20 deg/s, i.e. holding an aim, not re-aiming.
constexpr float kStillRadPerSec = 0.349f;

// TORN READS. An XrPosef is 28 bytes - a quaternion and a position - written
// here by the frame thread and read by callers on entirely different threads,
// with no synchronisation of any kind. A reader can therefore observe the
// position from one frame and the orientation from the next, or a half-updated
// position, and the result is a pose that never existed.
//
// That is not theoretical. The IK jitter decomposition measured single-tick
// steps of 0.05-0.06 m in the controller offset - about 24 m/s, which is not
// hand motion - in windows where the body yaw was simultaneously still, and
// clean readings of 0.0003 m in others. Intermittent, which is what a race
// looks like.
//
// ovr2xr.cpp already found and fixed exactly this bug for s->views,
// s->frameState and s->headPose ("written by one thread and read by another with
// no barrier at all") using a critical section. GetHeadPose takes that lock;
// GetControllerPose never did, because these globals live in a different file
// and the fix did not reach them. The reference project hit the same class of
// problem from the other side - its notes repeatedly describe having "bypassed
// TrackingMutex entirely, racing the game''s own ovr_GetTrackingState callers".
//
// A seqlock rather than a lock, deliberately: the WRITER here is the frame
// thread, and it must never block on a reader. The writer bumps a counter before
// and after (odd while writing); a reader retries while the counter is odd or
// changed underneath it. Wait-free for the writer, and a reader that loses
// several races simply keeps the previous pose for one tick, which is
// indistinguishable from a dropped sample.
volatile LONG g_poseSeq[2] = { 0, 0 };
bool g_valid[2] = { false, false };

// ===== [POSEMISS] - B-1 of docs/PROPOSAL-FIX-SHAKE-AND-SNAP-2026-09-24.md =====
//
// WHY THIS COUNTER EXISTS AND WHY IT IS UNCONDITIONAL. SolveArmIk does
// `if (!GetControllerPose(...)) { return false; }` (boneblend.cpp:6909) and
// g_drivenCount is only published on a SUCCESSFUL solve (boneblend.cpp:10593), so
// every frame counted here is a frame the rig held LAST frame's rotations while the
// body and the heading kept turning - and the jump back on re-acquisition is
// proportional to how far you turned during it. That is the leading candidate for
// "snaps hands back every few moments whilst turning", and the runtime here is
// inside-out (virtualdesktop-openxr), where turning sweeps the hands out of the
// headset cameras' FOV.
//
// COST: two InterlockedIncrements on a path that runs once per frame per hand, not
// per tick - checked against CLAUDE.md's per-frame rule before it went in. It is
// unconditional so that the number exists in EVERY log rather than only in a run
// where someone remembered to enable it; the LOG line is what the setting gates.
// A miss is counted where the VALID BIT is read, which is the only place that knows.
volatile LONG64 g_poseMissRun[2]   = { 0, 0 };  // misses this session, per hand
volatile LONG64 g_poseFrames[2]    = { 0, 0 };  // located frames this session
volatile LONG64 g_poseMissBouts[2] = { 0, 0 };  // dropout EVENTS (a run of >=1 miss)
volatile LONG   g_poseMissNow[2]   = { 0, 0 };  // consecutive misses right now
volatile LONG   g_poseMissMax[2]   = { 0, 0 };  // longest run of consecutive misses
volatile LONG64 g_gripMissRun[2]   = { 0, 0 };
float g_triggerValue[2] = { 0.0f, 0.0f };
bool g_triggerActive[2] = { false, false };
float g_stickX[2] = { 0.0f, 0.0f };
float g_stickY[2] = { 0.0f, 0.0f };
bool g_stickActive[2] = { false, false };
bool g_stickClickValue[2] = { false, false };
bool g_stickClickActive[2] = { false, false };
float g_squeezeValue[2] = { 0.0f, 0.0f };
bool g_squeezeActive[2] = { false, false };
bool g_menuValue = false;
bool g_menuActive = false;
bool g_buttonValue[4] = { false, false, false, false };
bool g_buttonActive[4] = { false, false, false, false };

// VALVE INDEX (2026-10-14). Two float actions bound ONLY on /interaction_profiles/valve/index_controller:
// squeeze/force (the grip's force sensor) and trackpad/force (the menu press - Index has no menu input; system/click is
// "may not be available for application use", OpenXR semantic_paths). Both are scalars, so instead of leaving the
// boolean threshold to the runtime (OpenXR input.adoc: "may vary from device to device", hysteresis only "should"), the
// press is decided HERE with a fixed hysteresis - the same numbers on every runtime.
XrAction g_squeezeForce = XR_NULL_HANDLE;
XrAction g_trackpadForce = XR_NULL_HANDLE;
float g_squeezeForceValue[2] = { 0.0f, 0.0f };
bool  g_squeezeForceActive[2] = { false, false };
float g_trackpadForceValue[2] = { 0.0f, 0.0f };
bool  g_trackpadForceActive[2] = { false, false };
// The raw squeeze/value as the runtime reported it (g_squeezeValue is what GetControllerSqueeze hands out - on Index,
// after ConditionIndexSqueeze; elsewhere the same number).
float g_squeezeRaw[2] = { 0.0f, 0.0f };
bool  g_squeezeRawActive[2] = { false, false };
bool  g_gripLatch[2] = { false, false };
bool  g_menuLatch = false;
// FINGER TRACKING (2026-11-11). Two boolean actions with per-hand subaction paths: trigger/touch, and ONE thumb action bound to
// every thumb touch surface of the hand (stick, thumbrest, face buttons) - OpenXR combines several bindings of a boolean action with
// OR, which is "any thumb touch". The eased weights are computed HERE, once per frame on the frame thread (single writer), and read
// by FingerTrackApply on whichever job thread runs the finger pass (a torn read mixes two adjacent frames' groups - harmless).
XrAction g_trigTouch = XR_NULL_HANDLE;
XrAction g_thumbTouch = XR_NULL_HANDLE;
bool  g_trigTouchValue[2] = { false, false };
bool  g_thumbTouchValue[2] = { false, false };
bool  g_fingerInputsActive[2] = { false, false };
volatile float g_ftW[2][3] = {};           // eased index / thumb / lower, per hand
volatile LONG  g_ftActive[2] = { 0, 0 };
LARGE_INTEGER  g_ftLastQpc = {};
const char*    g_ftBoundProfile = nullptr;  // the last profile whose suggestion ACCEPTED the touch paths ([FINGERTRACK] BIND)
struct FtWin {
    unsigned frames = 0;
    int   trigEdges = 0, thumbEdges = 0;
    bool  lastTrig = false, lastThumb = false;
    float sqMin = 2.0f, sqMax = -1.0f;   // 2 / -1 = no sample in the window (printed as -1)
};
FtWin     g_ftWin[2];
ULONGLONG g_ftWinStartMs = 0;
constexpr float kIndexPressOn = 0.5f;    // the existing consumers' press threshold (kGripThreshold, 0.5)
constexpr float kIndexPressOff = 0.3f;   // release below this: 0.2 of hysteresis
// Which profile each hand is on - set by LogActiveInteractionProfiles from xrGetCurrentInteractionProfile.
XrPath g_indexProfilePath = XR_NULL_PATH;
bool g_profileIsIndex[2] = { false, false };
// The aim pose's raw locationFlags, as g_gripFlags is for the grip (-1 = never located). For [CTLIN].
volatile LONG g_aimFlags[2] = { -1, -1 };

// Kept only for LogActiveInteractionProfiles() below (xrPathToString needs
// it) - everything else here already gets the instance as a call parameter.
XrInstance g_instance = XR_NULL_HANDLE;
// The last profile logged per hand. g_profileSeen marks the FIRST read: before 2026-10-14 the comparison started at
// XR_NULL_PATH, so a hand that never got a profile logged NOTHING all session - the silence the 10-08 Index diagnosis
// had to read. Now the first read always logs, NULL included, and so does a failing call (once per hand).
XrPath g_lastLoggedProfile[2] = { XR_NULL_PATH, XR_NULL_PATH };
bool g_profileSeen[2] = { false, false };
bool g_profileFailSaid[2] = { false, false };

// Which interaction profile the RUNTIME actually picked for each hand - not
// the same question as "did our suggested bindings get accepted" (logged
// once, at attach time, in ControllersInit below): a runtime can accept
// several suggested profiles and then only ever activate one, and which one
// it picks for an unfamiliar controller (a headset never tested against this
// mod) is exactly the open question the public test build exists to answer.
// Logged once per hand per launch normally, and again on any actual change
// (a controller reconnecting under a different profile) - never every frame.
void LogActiveInteractionProfiles(XrSession session)
{
    if (g_instance == XR_NULL_HANDLE) {
        return;
    }
    const char* handName[2] = { "left", "right" };
    for (int h = 0; h < 2; ++h) {
        if (g_handPath[h] == XR_NULL_PATH) {
            continue;
        }
        XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
        const XrResult r = xrGetCurrentInteractionProfile_(session, g_handPath[h], &state);
        if (XR_FAILED(r)) {
            if (!g_profileFailSaid[h]) {
                g_profileFailSaid[h] = true;
                LOG("controllers: %s hand - xrGetCurrentInteractionProfile FAILED, XrResult %d - the active profile is "
                    "UNKNOWN (logged once per hand)", handName[h], static_cast<int>(r));
            }
            continue;
        }
        g_profileIsIndex[h] = state.interactionProfile != XR_NULL_PATH &&
                              state.interactionProfile == g_indexProfilePath;
        if (g_profileSeen[h] && state.interactionProfile == g_lastLoggedProfile[h]) {
            continue;
        }
        g_profileSeen[h] = true;
        g_lastLoggedProfile[h] = state.interactionProfile;
        if (state.interactionProfile == XR_NULL_PATH) {
            LOG("controllers: %s hand - no interaction profile active = XR_NULL_PATH (nothing bound yet, the controller "
                "is off / disconnected, or the runtime cannot emulate any profile we suggested - OpenXR input.adoc). "
                "A later line names the profile if one becomes active.", handName[h]);
            continue;
        }
        char path[256] = {};
        uint32_t written = 0;
        if (XR_SUCCEEDED(xrPathToString_(g_instance, state.interactionProfile, sizeof(path),
                                         &written, path))) {
            LOG("controllers: %s hand - active interaction profile = %s%s", handName[h], path,
                g_profileIsIndex[h] ? (IndexGripForce ? " | INDEX: grip = squeeze/force, menu = left trackpad/force, "
                                                        "both through 0.5 on / 0.3 off"
                                                      : " | INDEX: grip = squeeze/value (index_grip_force=0), menu = "
                                                        "left trackpad/force, both through 0.5 on / 0.3 off")
                                    : "");
        } else {
            LOG("controllers: %s hand - active interaction profile (path %llu, could not "
                "resolve to a string)", handName[h],
                static_cast<unsigned long long>(state.interactionProfile));
        }
    }
}

// See GetAimTriggerGeneration()'s own comment in controllers.h - a plain
// burst-boundary marker, bumped the instant the right trigger crosses the
// fire threshold. Deliberately does NOT also capture a pose here anymore
// (an earlier version did): aimhook.cpp now takes a fresh pose itself at
// the first ComputeShootDir-caller hit it sees for a new generation, which
// is the moment the game actually asks for a direction, rather than the
// raw trigger-crossing instant - closer to "where you'd settled your aim by
// the time it mattered" than "where your hand happened to be still moving
// into position".
constexpr float kAimLatchThreshold = 0.5f;
volatile LONG g_latchGeneration = 0;

}  // namespace

bool ControllersInit(XrInstance instance, XrSession session, XrSpace appSpace)
{
    if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE || appSpace == XR_NULL_HANDLE) {
        return false;
    }
    // A session that gets recreated mid-run (not something this mod normally
    // does - the session is created once, lazily, when the D3D device shows
    // up) would need the action set reattached and its spaces rebuilt against
    // the new session. Not handled - this only ever runs once per launch in
    // practice, and doing that properly without a real reason to exercise it
    // would just be unverifiable code.
    if (g_actionSet != XR_NULL_HANDLE) {
        return true;
    }
    g_instance = instance;

    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy_s(asci.actionSetName, "dlvr_gameplay", _TRUNCATE);
    strncpy_s(asci.localizedActionSetName, "DLVR Gameplay", _TRUNCATE);
    asci.priority = 0;
    if (!Ok(xrCreateActionSet_(instance, &asci, &g_actionSet), "xrCreateActionSet")) {
        return false;
    }

    if (!Ok(xrStringToPath_(instance, "/user/hand/left", &g_handPath[kLeftHand]),
            "xrStringToPath(/user/hand/left)") ||
        !Ok(xrStringToPath_(instance, "/user/hand/right", &g_handPath[kRightHand]),
            "xrStringToPath(/user/hand/right)")) {
        return false;
    }

    XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(aci.actionName, "aim_pose", _TRUNCATE);
    strncpy_s(aci.localizedActionName, "Aim Pose", _TRUNCATE);
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &aci, &g_aimPose), "xrCreateAction(aim_pose)")) {
        return false;
    }

    // THE GRIP POSE ACTION. Must be created HERE, before
    // xrAttachSessionActionSets below - an action set is IMMUTABLE once
    // attached, so xrCreateAction on it after that point fails at runtime with
    // XR_ERROR_ACTIONSETS_ALREADY_ATTACHED. Nothing about the ordering is
    // cosmetic.
    //
    // NOT fatal on failure, unlike the aim pose: the rig falls back to the aim
    // pose if this never comes up, which is strictly what we had before, so a
    // runtime that refuses grip/pose degrades instead of losing the hands.
    XrActionCreateInfo gci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(gci.actionName, "grip_pose", _TRUNCATE);
    strncpy_s(gci.localizedActionName, "Grip Pose", _TRUNCATE);
    gci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    gci.countSubactionPaths = 2;
    gci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &gci, &g_gripPose), "xrCreateAction(grip_pose)")) {
        g_gripPose = XR_NULL_HANDLE;
    }

    // Trigger pull, for redirecting weapon fire (see aimhook.cpp) - kept as
    // its own action rather than folded into aim_pose since it's a different
    // action type. Not fatal if this fails to create: the mod still works
    // for aim-pose tracking with no fire redirection, same "degrade, don't
    // abort" approach as a single hand's action space failing below.
    XrActionCreateInfo tci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(tci.actionName, "trigger", _TRUNCATE);
    strncpy_s(tci.localizedActionName, "Trigger", _TRUNCATE);
    tci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    tci.countSubactionPaths = 2;
    tci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &tci, &g_trigger), "xrCreateAction(trigger)")) {
        g_trigger = XR_NULL_HANDLE;
    }

    // Thumbstick, for left-stick movement / right-stick smooth turning (see
    // dlvr_FilterScePad in ovr2xr.cpp) - same "degrade, don't abort" approach
    // as trigger above. khr/simple_controller has no thumbstick at all, only
    // trigger's own touch_controller-only binding below - the profile loop
    // skips this action entirely for khr/simple_controller.
    XrActionCreateInfo sci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(sci.actionName, "thumbstick", _TRUNCATE);
    strncpy_s(sci.localizedActionName, "Thumbstick", _TRUNCATE);
    sci.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    sci.countSubactionPaths = 2;
    sci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &sci, &g_stick), "xrCreateAction(thumbstick)")) {
        g_stick = XR_NULL_HANDLE;
    }

    // Thumbstick click (L3/R3) - confirmed 2026-09-02 that left-hand click is
    // genuinely Dying Light's sprint binding (see dlvr_FilterScePad).
    XrActionCreateInfo clci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(clci.actionName, "thumbstick_click", _TRUNCATE);
    strncpy_s(clci.localizedActionName, "Thumbstick Click", _TRUNCATE);
    clci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    clci.countSubactionPaths = 2;
    clci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &clci, &g_stickClick),
            "xrCreateAction(thumbstick_click)")) {
        g_stickClick = XR_NULL_HANDLE;
    }

    // Grip/squeeze - analog, same shape as trigger above. khr/simple_controller
    // has no squeeze input at all, same nullptr-means-skip treatment as
    // thumbstick.
    XrActionCreateInfo sqci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(sqci.actionName, "squeeze", _TRUNCATE);
    strncpy_s(sqci.localizedActionName, "Squeeze", _TRUNCATE);
    sqci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    sqci.countSubactionPaths = 2;
    sqci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &sqci, &g_squeeze), "xrCreateAction(squeeze)")) {
        g_squeeze = XR_NULL_HANDLE;
    }

    // Vibration output - the holster's pulses (SPEC-SHOULDER-HOLSTER §3). Must exist before
    // xrAttachSessionActionSets below, like every action here. Not fatal: without it the holster
    // still works, it just cannot buzz (PulseController returns false and the holster logs that).
    XrActionCreateInfo hci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(hci.actionName, "haptic", _TRUNCATE);
    strncpy_s(hci.localizedActionName, "Haptic", _TRUNCATE);
    hci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    hci.countSubactionPaths = 2;
    hci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &hci, &g_haptic), "xrCreateAction(haptic)")) {
        g_haptic = XR_NULL_HANDLE;
    }

    // Valve Index scalars (see g_squeezeForce's comment) - bound only on the Index row. Not fatal: without them an
    // Index grip falls back to squeeze/value (still through the hysteresis) and the menu has no Index binding.
    XrActionCreateInfo sfci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(sfci.actionName, "squeeze_force", _TRUNCATE);
    strncpy_s(sfci.localizedActionName, "Squeeze Force", _TRUNCATE);
    sfci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    sfci.countSubactionPaths = 2;
    sfci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &sfci, &g_squeezeForce), "xrCreateAction(squeeze_force)")) {
        g_squeezeForce = XR_NULL_HANDLE;
    }
    XrActionCreateInfo tfci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(tfci.actionName, "trackpad_force", _TRUNCATE);
    strncpy_s(tfci.localizedActionName, "Trackpad Force", _TRUNCATE);
    tfci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    tfci.countSubactionPaths = 2;
    tfci.subactionPaths = g_handPath;
    if (!Ok(xrCreateAction_(g_actionSet, &tfci, &g_trackpadForce), "xrCreateAction(trackpad_force)")) {
        g_trackpadForce = XR_NULL_HANDLE;
    }
    // Finger tracking touches (see g_trigTouch). Not fatal: without them the fingers stay as today.
    {
        XrActionCreateInfo fci{ XR_TYPE_ACTION_CREATE_INFO };
        strncpy_s(fci.actionName, "trigger_touch", _TRUNCATE);
        strncpy_s(fci.localizedActionName, "Trigger Touch", _TRUNCATE);
        fci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        fci.countSubactionPaths = 2;
        fci.subactionPaths = g_handPath;
        if (!Ok(xrCreateAction_(g_actionSet, &fci, &g_trigTouch), "xrCreateAction(trigger_touch)")) {
            g_trigTouch = XR_NULL_HANDLE;
        }
        strncpy_s(fci.actionName, "thumb_touch", _TRUNCATE);
        strncpy_s(fci.localizedActionName, "Thumb Touch", _TRUNCATE);
        if (!Ok(xrCreateAction_(g_actionSet, &fci, &g_thumbTouch), "xrCreateAction(thumb_touch)")) {
            g_thumbTouch = XR_NULL_HANDLE;
        }
    }
    if (!Ok(xrStringToPath_(instance, "/interaction_profiles/valve/index_controller", &g_indexProfilePath),
            "xrStringToPath(index profile)")) {
        g_indexProfilePath = XR_NULL_PATH;
    }

    // Left controller's menu button - inherently single-hand (see this
    // action's own comment in controllers.h), so no subaction paths, exactly
    // like the face buttons below.
    XrActionCreateInfo mci{ XR_TYPE_ACTION_CREATE_INFO };
    strncpy_s(mci.actionName, "menu_button", _TRUNCATE);
    strncpy_s(mci.localizedActionName, "Menu Button", _TRUNCATE);
    mci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    mci.countSubactionPaths = 0;
    mci.subactionPaths = nullptr;
    if (!Ok(xrCreateAction_(g_actionSet, &mci, &g_menu), "xrCreateAction(menu_button)")) {
        g_menu = XR_NULL_HANDLE;
    }

    // Face buttons - A/B/X/Y, 1:1 physical parity with a real gamepad's face
    // buttons (see dlvr_FilterScePad). No subaction paths: each is a single,
    // specific control, bound to one concrete path per profile below rather
    // than a left/right pair.
    {
        const char* names[4] = { "button_a", "button_b", "button_x", "button_y" };
        const char* labels[4] = { "Button A", "Button B", "Button X", "Button Y" };
        for (int i = 0; i < 4; ++i) {
            XrActionCreateInfo bci{ XR_TYPE_ACTION_CREATE_INFO };
            strncpy_s(bci.actionName, names[i], _TRUNCATE);
            strncpy_s(bci.localizedActionName, labels[i], _TRUNCATE);
            bci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            bci.countSubactionPaths = 0;
            bci.subactionPaths = nullptr;
            if (!Ok(xrCreateAction_(g_actionSet, &bci, &g_button[i]), "xrCreateAction(button)")) {
                g_button[i] = XR_NULL_HANDLE;
            }
        }
    }

    // A binding suggestion for a profile the runtime never activates is
    // harmless - only whichever one matches the connected controller ever
    // takes effect. khr/simple_controller is a required-to-support floor for
    // every conformant runtime; oculus/touch_controller is what Virtual
    // Desktop and Quest present as, which is what this mod is actually built
    // for (see the machine-layout table in CLAUDE.md).
    //
    // khr/simple_controller has no analog trigger, only a boolean
    // select/click - OpenXR requires runtimes to implicitly convert a
    // boolean input bound to a float action to 0.0f/1.0f, so the same
    // trigger action still works there, just without analog pull.
    struct ProfileBinding {
        const char* profile;
        const char* leftPath;
        const char* rightPath;
        const char* triggerLeftPath;
        const char* triggerRightPath;
        // nullptr for a profile with no thumbstick at all (khr/simple_controller) -
        // the binding loop below skips the stick/click actions entirely in
        // that case.
        const char* stickLeftPath;
        const char* stickRightPath;
        const char* stickClickLeftPath;
        // R3 - added 2026-09-03. nullptr on khr/simple_controller, same
        // treatment as every other thumbstick-adjacent field.
        const char* stickClickRightPath;
        // Grip/squeeze - nullptr on khr/simple_controller (no squeeze input
        // at all). Added 2026-09-03.
        const char* squeezeLeftPath;
        const char* squeezeRightPath;
        // Left controller's menu button - nullptr on khr/simple_controller
        // (no menu input in that profile). Added 2026-09-03.
        const char* menuLeftPath;
        // Same nullptr-means-skip treatment as the stick fields above -
        // khr/simple_controller has no face buttons at all.
        const char* buttonAPath;
        const char* buttonBPath;
        const char* buttonXPath;
        const char* buttonYPath;
        // Grip POSE - the anatomy-defined hand transform, NOT the squeeze
        // button above. Added 2026-09-10, deliberately LAST in the struct so
        // every existing positional initialiser below keeps its alignment;
        // inserting mid-struct would silently shift sixteen path strings by
        // two and rebind half the controller. Both profiles support it -
        // khr/simple_controller carries grip/pose even though it has no
        // squeeze input at all - so unlike the button fields neither row
        // needs a nullptr here.
        const char* gripPoseLeftPath;
        const char* gripPoseRightPath;
        // Vibration OUTPUT - added 2026-09-29 for the holster, LAST for the same positional-initialiser
        // reason as the grip pose above. Both profiles define /user/hand/*/output/haptic.
        const char* hapticLeftPath;
        const char* hapticRightPath;
        // Valve Index scalars (2026-10-14), LAST for the positional-initialiser reason above; rows that omit them are
        // value-initialised to nullptr (aggregate init), which means "not bound".
        const char* squeezeForceLeftPath;
        const char* squeezeForceRightPath;
        const char* trackpadForceLeftPath;
        const char* trackpadForceRightPath;
        // FINGER TRACKING touches (2026-11-11), LAST for the positional-initialiser reason above; omitted = nullptr = not bound.
        // thumbTouch*: up to four surfaces per hand, all bound to the ONE thumb_touch action (OR).
        const char* trigTouchLeftPath;
        const char* trigTouchRightPath;
        const char* thumbTouchLeft[4];
        const char* thumbTouchRight[4];
    };
    const ProfileBinding profiles[] = {
        { "/interaction_profiles/khr/simple_controller", "/user/hand/left/input/aim/pose",
          "/user/hand/right/input/aim/pose", "/user/hand/left/input/select/click",
          "/user/hand/right/input/select/click", nullptr, nullptr, nullptr, nullptr, nullptr,
          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
          "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose",
          "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" },
        { "/interaction_profiles/oculus/touch_controller", "/user/hand/left/input/aim/pose",
          "/user/hand/right/input/aim/pose", "/user/hand/left/input/trigger/value",
          "/user/hand/right/input/trigger/value", "/user/hand/left/input/thumbstick",
          "/user/hand/right/input/thumbstick", "/user/hand/left/input/thumbstick/click",
          "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/squeeze/value",
          "/user/hand/right/input/squeeze/value", "/user/hand/left/input/menu/click",
          "/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
          "/user/hand/left/input/x/click", "/user/hand/left/input/y/click",
          "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose",
          "/user/hand/left/output/haptic", "/user/hand/right/output/haptic",
          nullptr, nullptr, nullptr, nullptr,
          "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch",
          { "/user/hand/left/input/thumbstick/touch", "/user/hand/left/input/thumbrest/touch",
            "/user/hand/left/input/x/touch", "/user/hand/left/input/y/touch" },
          { "/user/hand/right/input/thumbstick/touch", "/user/hand/right/input/thumbrest/touch",
            "/user/hand/right/input/a/touch", "/user/hand/right/input/b/touch" } },
        // Valve Index - added 2026-10-08 after a reviewer's Index controllers gave no input on
        // SteamVR: with only the two rows above suggested, no profile was ever activated for
        // either hand. The Index profile has no x/y and no menu (system/click is runtime-reserved),
        // so (Jay's mapping): X/Y = the LEFT controller's a/b, and menu = a press on the left
        // trackpad.
        // 2026-10-14: the menu is NO LONGER bound to the boolean menu action (that left the threshold
        // to the runtime); trackpad/force goes to its own float action and the press is decided by our
        // hysteresis (GetControllerMenuButton). squeeze/force is bound alongside squeeze/value; which one
        // drives the grips is index_grip_force (ConditionIndexSqueeze).
        { "/interaction_profiles/valve/index_controller", "/user/hand/left/input/aim/pose",
          "/user/hand/right/input/aim/pose", "/user/hand/left/input/trigger/value",
          "/user/hand/right/input/trigger/value", "/user/hand/left/input/thumbstick",
          "/user/hand/right/input/thumbstick", "/user/hand/left/input/thumbstick/click",
          "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/squeeze/value",
          "/user/hand/right/input/squeeze/value", nullptr,
          "/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
          "/user/hand/left/input/a/click", "/user/hand/left/input/b/click",
          "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose",
          "/user/hand/left/output/haptic", "/user/hand/right/output/haptic",
          "/user/hand/left/input/squeeze/force", "/user/hand/right/input/squeeze/force",
          "/user/hand/left/input/trackpad/force", "/user/hand/right/input/trackpad/force",
          "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch",
          { "/user/hand/left/input/thumbstick/touch", "/user/hand/left/input/trackpad/touch",
            "/user/hand/left/input/a/touch", "/user/hand/left/input/b/touch" },
          { "/user/hand/right/input/thumbstick/touch", "/user/hand/right/input/trackpad/touch",
            "/user/hand/right/input/a/touch", "/user/hand/right/input/b/touch" } },
    };
    int boundProfiles = 0;
    for (const auto& pb : profiles) {
        XrPath profilePath, leftPath, rightPath;
        if (!Ok(xrStringToPath_(instance, pb.profile, &profilePath), "xrStringToPath(profile)") ||
            !Ok(xrStringToPath_(instance, pb.leftPath, &leftPath), "xrStringToPath(binding left)") ||
            !Ok(xrStringToPath_(instance, pb.rightPath, &rightPath),
                "xrStringToPath(binding right)")) {
            continue;
        }
        // SIZE: 24, not 16. The worst case on touch_controller is now
        //   2 aim + 2 trigger + 2 stick + 1 L3 + 1 R3 + 2 squeeze + 1 menu
        //   + 4 face buttons + 2 GRIP POSE + 2 HAPTIC = 19
        // and it was 15 before the grip pose was added, i.e. the old [16] was
        // one slot from overflowing and adding grip would have written two
        // XrActionSuggestedBinding structs past the end of a stack array.
        // Headroom rather than exactly 17, so the next input does not have to
        // rediscover this.
        // 2026-10-14: 32. The Index row is now 2 aim + 2 grip pose + 2 haptic + 2 trigger + 2 stick + 2 clicks
        // + 2 squeeze + 4 face + 2 squeeze/force + 2 trackpad/force = 22 (no menu row).
        // 2026-11-11: 40. Finger tracking adds 2 trigger/touch + 8 thumb touches per row: Touch 19 + 10 = 29, Index 22 + 10 = 32
        // (exactly the old size - hence the headroom).
        XrActionSuggestedBinding bindings[40] = { { g_aimPose, leftPath },
                                                  { g_aimPose, rightPath } };
        uint32_t countBindings = 2;
        // Grip POSE - suggested alongside aim, not instead of it. Both poses
        // are bound simultaneously and the runtime reports both; which one the
        // rig consumes is decided in boneblend.cpp, not here.
        if (g_gripPose != XR_NULL_HANDLE && pb.gripPoseLeftPath && pb.gripPoseRightPath) {
            XrPath gripLeft, gripRight;
            if (Ok(xrStringToPath_(instance, pb.gripPoseLeftPath, &gripLeft),
                   "xrStringToPath(grip pose left)") &&
                Ok(xrStringToPath_(instance, pb.gripPoseRightPath, &gripRight),
                   "xrStringToPath(grip pose right)")) {
                bindings[countBindings++] = { g_gripPose, gripLeft };
                bindings[countBindings++] = { g_gripPose, gripRight };
            }
        }
        if (g_haptic != XR_NULL_HANDLE && pb.hapticLeftPath && pb.hapticRightPath) {
            XrPath hapticLeft, hapticRight;
            if (Ok(xrStringToPath_(instance, pb.hapticLeftPath, &hapticLeft),
                   "xrStringToPath(haptic left)") &&
                Ok(xrStringToPath_(instance, pb.hapticRightPath, &hapticRight),
                   "xrStringToPath(haptic right)")) {
                bindings[countBindings++] = { g_haptic, hapticLeft };
                bindings[countBindings++] = { g_haptic, hapticRight };
            }
        }
        if (g_trigger != XR_NULL_HANDLE) {
            XrPath triggerLeft, triggerRight;
            if (Ok(xrStringToPath_(instance, pb.triggerLeftPath, &triggerLeft),
                   "xrStringToPath(trigger left)") &&
                Ok(xrStringToPath_(instance, pb.triggerRightPath, &triggerRight),
                   "xrStringToPath(trigger right)")) {
                bindings[countBindings++] = { g_trigger, triggerLeft };
                bindings[countBindings++] = { g_trigger, triggerRight };
            }
        }
        if (g_stick != XR_NULL_HANDLE && pb.stickLeftPath && pb.stickRightPath) {
            XrPath stickLeft, stickRight;
            if (Ok(xrStringToPath_(instance, pb.stickLeftPath, &stickLeft),
                   "xrStringToPath(stick left)") &&
                Ok(xrStringToPath_(instance, pb.stickRightPath, &stickRight),
                   "xrStringToPath(stick right)")) {
                bindings[countBindings++] = { g_stick, stickLeft };
                bindings[countBindings++] = { g_stick, stickRight };
            }
        }
        if (g_stickClick != XR_NULL_HANDLE && pb.stickClickLeftPath) {
            XrPath clickLeft;
            if (Ok(xrStringToPath_(instance, pb.stickClickLeftPath, &clickLeft),
                   "xrStringToPath(stick click left)")) {
                bindings[countBindings++] = { g_stickClick, clickLeft };
            }
        }
        if (g_stickClick != XR_NULL_HANDLE && pb.stickClickRightPath) {
            XrPath clickRight;
            if (Ok(xrStringToPath_(instance, pb.stickClickRightPath, &clickRight),
                   "xrStringToPath(stick click right)")) {
                bindings[countBindings++] = { g_stickClick, clickRight };
            }
        }
        if (g_squeeze != XR_NULL_HANDLE && pb.squeezeLeftPath && pb.squeezeRightPath) {
            XrPath squeezeLeft, squeezeRight;
            if (Ok(xrStringToPath_(instance, pb.squeezeLeftPath, &squeezeLeft),
                   "xrStringToPath(squeeze left)") &&
                Ok(xrStringToPath_(instance, pb.squeezeRightPath, &squeezeRight),
                   "xrStringToPath(squeeze right)")) {
                bindings[countBindings++] = { g_squeeze, squeezeLeft };
                bindings[countBindings++] = { g_squeeze, squeezeRight };
            }
        }
        if (g_squeezeForce != XR_NULL_HANDLE && pb.squeezeForceLeftPath && pb.squeezeForceRightPath) {
            XrPath l, r;
            if (Ok(xrStringToPath_(instance, pb.squeezeForceLeftPath, &l), "xrStringToPath(squeeze force left)") &&
                Ok(xrStringToPath_(instance, pb.squeezeForceRightPath, &r), "xrStringToPath(squeeze force right)")) {
                bindings[countBindings++] = { g_squeezeForce, l };
                bindings[countBindings++] = { g_squeezeForce, r };
            }
        }
        if (g_trackpadForce != XR_NULL_HANDLE && pb.trackpadForceLeftPath && pb.trackpadForceRightPath) {
            XrPath l, r;
            if (Ok(xrStringToPath_(instance, pb.trackpadForceLeftPath, &l), "xrStringToPath(trackpad force left)") &&
                Ok(xrStringToPath_(instance, pb.trackpadForceRightPath, &r), "xrStringToPath(trackpad force right)")) {
                bindings[countBindings++] = { g_trackpadForce, l };
                bindings[countBindings++] = { g_trackpadForce, r };
            }
        }
        if (g_menu != XR_NULL_HANDLE && pb.menuLeftPath) {
            XrPath menuLeft;
            if (Ok(xrStringToPath_(instance, pb.menuLeftPath, &menuLeft),
                   "xrStringToPath(menu left)")) {
                bindings[countBindings++] = { g_menu, menuLeft };
            }
        }
        if (pb.buttonAPath && pb.buttonBPath && pb.buttonXPath && pb.buttonYPath) {
            const XrAction actions[4] = { g_button[0], g_button[1], g_button[2], g_button[3] };
            const char* paths[4] = { pb.buttonAPath, pb.buttonBPath, pb.buttonXPath,
                                     pb.buttonYPath };
            for (int i = 0; i < 4; ++i) {
                if (actions[i] == XR_NULL_HANDLE) {
                    continue;
                }
                XrPath p;
                if (Ok(xrStringToPath_(instance, paths[i], &p), "xrStringToPath(face button)")) {
                    bindings[countBindings++] = { actions[i], p };
                }
            }
        }
        // FINGER TRACKING touches go LAST, after countBase: a runtime that refuses ANY path fails the whole suggestion, so a refusal
        // with them is retried WITHOUT them below - the touches can never cost the existing input (spec 5.1).
        const uint32_t countBase = countBindings;
        if (g_trigTouch != XR_NULL_HANDLE && pb.trigTouchLeftPath && pb.trigTouchRightPath) {
            XrPath l, r;
            if (Ok(xrStringToPath_(instance, pb.trigTouchLeftPath, &l), "xrStringToPath(trigger touch left)") &&
                Ok(xrStringToPath_(instance, pb.trigTouchRightPath, &r), "xrStringToPath(trigger touch right)")) {
                bindings[countBindings++] = { g_trigTouch, l };
                bindings[countBindings++] = { g_trigTouch, r };
            }
        }
        if (g_thumbTouch != XR_NULL_HANDLE) {
            for (int s = 0; s < 2; ++s) {
                const char* const* paths = s ? pb.thumbTouchRight : pb.thumbTouchLeft;
                for (int k = 0; k < 4; ++k) {
                    XrPath p;
                    if (paths[k] && countBindings < 40 && Ok(xrStringToPath_(instance, paths[k], &p), "xrStringToPath(thumb touch)")) {
                        bindings[countBindings++] = { g_thumbTouch, p };
                    }
                }
            }
        }
        XrInteractionProfileSuggestedBinding suggest{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
        };
        suggest.interactionProfile = profilePath;
        suggest.countSuggestedBindings = countBindings;
        suggest.suggestedBindings = bindings;
        bool accepted = Ok(xrSuggestInteractionProfileBindings_(instance, &suggest), "xrSuggestInteractionProfileBindings");
        const uint32_t touchCount = countBindings - countBase;
        if (!accepted && touchCount > 0) {
            // [PROBE: FINGERTRACK] gate=finger_track  site=HEADSET  log-only=yes  (the BIND lines log whatever the setting says)
            LOG("controllers: [FINGERTRACK] BIND REFUSED %s with its %u touch paths - retrying WITHOUT them; fingers stay as today on "
                "this profile, every other input is bound as before", pb.profile, touchCount);
            suggest.countSuggestedBindings = countBase;
            accepted = Ok(xrSuggestInteractionProfileBindings_(instance, &suggest), "xrSuggestInteractionProfileBindings(no touch)");
            countBindings = countBase;
        } else if (accepted && touchCount > 0) {
            g_ftBoundProfile = pb.profile;
            LOG("controllers: [FINGERTRACK] BIND %s touch paths %u (trigger + thumb surfaces)", pb.profile, touchCount);
        } else if (touchCount == 0) {
            LOG("controllers: [FINGERTRACK] BIND %s - no touch paths on this profile (fingers stay as today on it)", pb.profile);
        }
        if (accepted) {
            ++boundProfiles;
            LOG("controllers: suggested bindings ACCEPTED for %s (%u bindings)", pb.profile, countBindings);
        } else {
            LOG("controllers: suggested bindings REFUSED for %s (%u bindings) - this profile has no input",
                pb.profile, countBindings);
        }
    }
    if (boundProfiles == 0) {
        LOG("controllers: no interaction profile accepted bindings - giving up");
        return false;
    }

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    if (!Ok(xrAttachSessionActionSets_(session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo spc{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spc.action = g_aimPose;
        spc.subactionPath = g_handPath[h];
        spc.poseInActionSpace = Identity();
        if (!Ok(xrCreateActionSpace_(session, &spc, &g_handSpace[h]), "xrCreateActionSpace")) {
            g_handSpace[h] = XR_NULL_HANDLE;
        }
        // The grip pose's own space. Same shape, identity offset, separate
        // handle - a space is created from ONE action, so the second pose
        // cannot share g_handSpace.
        if (g_gripPose != XR_NULL_HANDLE) {
            XrActionSpaceCreateInfo gspc{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            gspc.action = g_gripPose;
            gspc.subactionPath = g_handPath[h];
            gspc.poseInActionSpace = Identity();
            if (!Ok(xrCreateActionSpace_(session, &gspc, &g_gripSpace[h]),
                    "xrCreateActionSpace(grip)")) {
                g_gripSpace[h] = XR_NULL_HANDLE;
            }
        }
    }

    LOG("controllers: ready (%d profile(s) bound, left space %s, right space %s, thumbstick "
        "action %s, thumbstick click action %s, squeeze action %s, menu action %s, face buttons "
        "%s/%s/%s/%s)", boundProfiles,
        g_handSpace[kLeftHand] ? "ok" : "FAILED", g_handSpace[kRightHand] ? "ok" : "FAILED",
        g_stick ? "created" : "FAILED", g_stickClick ? "created" : "FAILED",
        g_squeeze ? "created" : "FAILED", g_menu ? "created" : "FAILED",
        g_button[0] ? "A" : "-", g_button[1] ? "B" : "-", g_button[2] ? "X" : "-",
        g_button[3] ? "Y" : "-");
    // GRIP POSE, reported separately and unconditionally. A probe must log its
    // first sample even when nothing is wrong, or silence is ambiguous between
    // "it came up fine" and "it never ran" - the rule this file has been bitten
    // by repeatedly. If the action or either space says FAILED, the rig falls
    // back to the aim pose and NO baked grip constant is valid.
    LOG("controllers: grip pose - action %s, left space %s, right space %s. The rig uses "
        "this for the HAND when rig_hand_grip_pose=1; the aim pose is unchanged and still "
        "drives aiming. This is input/grip/pose (a transform), NOT input/squeeze/value "
        "(the L1/R1 button), which is untouched.",
        g_gripPose ? "created" : "FAILED",
        g_gripSpace[kLeftHand] ? "ok" : "FAILED",
        g_gripSpace[kRightHand] ? "ok" : "FAILED");
    LOG("controllers: haptic - vibration action %s (bound to output/haptic on every profile). Pulses are "
        "queued by PulseController and applied from ControllersSync on the frame thread.",
        g_haptic ? "created" : "FAILED");
    // [PROBE: CTLIN] gate=diag_ctl_input  site=HEADSET  log-only=yes
    LOG("controllers: [CTLIN] ARMED (logged unconditionally) - Index actions: squeeze_force %s, trackpad_force %s, "
        "index profile path %s | index_grip_force %d (1 = squeeze/force, 0 = squeeze/value), Index press 0.50 on / "
        "0.30 off | diag_ctl_input %d (the 2 s per-hand input lines)",
        g_squeezeForce ? "created" : "FAILED", g_trackpadForce ? "created" : "FAILED",
        g_indexProfilePath != XR_NULL_PATH ? "ok" : "FAILED", IndexGripForce ? 1 : 0, DiagCtlInput ? 1 : 0);
    // [PROBE: FINGERTRACK] gate=finger_track  site=HEADSET  log-only=yes
    LOG("controllers: [FINGERTRACK] ARMED (logged unconditionally) - actions trigger_touch %s, thumb_touch %s; touch paths accepted on "
        "%s | finger_track %d, finger_track_ease_ms %.0f, finger_track_fist_at %.2f%s",
        g_trigTouch ? "created" : "FAILED", g_thumbTouch ? "created" : "FAILED",
        g_ftBoundProfile ? g_ftBoundProfile : "NO profile (fingers stay as today)", FingerTrack ? 1 : 0, FingerTrackEaseMs,
        FingerTrackFistAt, FingerTrack ? "" : " - OFF: no finger is written and the 1 s lines do not print");
    return true;
}

namespace {

// One Index press through the hysteresis: on at >= kIndexPressOn, off below kIndexPressOff.
bool IndexLatch(bool& latch, float v)
{
    latch = latch ? (v >= kIndexPressOff) : (v >= kIndexPressOn);
    return latch;
}

// What GetControllerSqueeze hands out on an Index hand. Every consumer thresholds at 0.5 (ovr2xr.cpp kGripThreshold,
// holster_logic.h, twohand_logic.h), so a held latch is reported >= 0.5 and a released one < 0.5 - the consumers see
// the hysteresis without being touched, and the number they log stays the real one where the two agree.
float ConditionIndexSqueeze(int h)
{
    // squeeze/value not active -> squeeze/force whatever the setting (this branch runs only while one of them is).
    const bool force = (IndexGripForce || !g_squeezeRawActive[h]) && g_squeezeForceActive[h];
    const float v = force ? g_squeezeForceValue[h] : g_squeezeRaw[h];
    return IndexLatch(g_gripLatch[h], v) ? (v > kIndexPressOn ? v : kIndexPressOn)
                                         : (v < kIndexPressOn ? v : 0.49f);
}

// [CTLIN] window, per hand. Frame thread.
struct CtlWin {
    long long frames = 0;
    float trigMin = 2.0f, trigMax = -1.0f, sqMin = 2.0f, sqMax = -1.0f;
    float fMin = 2.0f, fMax = -1.0f, tpMin = 2.0f, tpMax = -1.0f;
    long long gripPress = 0;   // frames the hand's GetControllerSqueeze reads >= 0.5
};
CtlWin g_cw[2];
ULONGLONG g_cwStart = 0;

void CtlMinMax(float v, float& lo, float& hi)
{
    if (v < lo) { lo = v; }
    if (v > hi) { hi = v; }
}

void CtlInputProbe()
{
    if (!DiagCtlInput) {
        g_cwStart = 0;
        g_cw[0] = CtlWin{};
        g_cw[1] = CtlWin{};
        return;
    }
    for (int h = 0; h < 2; ++h) {
        CtlWin& w = g_cw[h];
        ++w.frames;
        if (g_triggerActive[h]) { CtlMinMax(g_triggerValue[h], w.trigMin, w.trigMax); }
        if (g_squeezeRawActive[h]) { CtlMinMax(g_squeezeRaw[h], w.sqMin, w.sqMax); }
        if (g_squeezeActive[h] && g_squeezeValue[h] >= 0.5f) { ++w.gripPress; }
        if (g_squeezeForceActive[h]) { CtlMinMax(g_squeezeForceValue[h], w.fMin, w.fMax); }
        if (g_trackpadForceActive[h]) { CtlMinMax(g_trackpadForceValue[h], w.tpMin, w.tpMax); }
    }
    const ULONGLONG now = GetTickCount64();
    if (g_cwStart == 0) { g_cwStart = now; }
    if (now - g_cwStart < 2000) { return; }
    char prof[2][128] = {};
    for (int h = 0; h < 2; ++h) {
        uint32_t written = 0;
        if (!g_profileSeen[h]) {
            strncpy_s(prof[h], "NOT READ", _TRUNCATE);
        } else if (g_lastLoggedProfile[h] == XR_NULL_PATH) {
            strncpy_s(prof[h], "XR_NULL_PATH", _TRUNCATE);
        } else if (XR_FAILED(xrPathToString_(g_instance, g_lastLoggedProfile[h], sizeof(prof[h]), &written,
                                             prof[h]))) {
            strncpy_s(prof[h], "UNRESOLVED (xrPathToString failed)", _TRUNCATE);
        }
    }
    for (int h = 0; h < 2; ++h) {
        const CtlWin& w = g_cw[h];
        // An inactive input reads "-" rather than a min/max, so "never active" and "rested at 0" differ (INV-6).
        char trig[32], sq[32], f[32], tp[32];
        auto range = [](char* out, size_t n, bool any, float lo, float hi) {
            if (any) { sprintf_s(out, n, "%.2f..%.2f", lo, hi); } else { strncpy_s(out, n, "-", _TRUNCATE); }
        };
        range(trig, sizeof(trig), w.trigMax >= 0.0f, w.trigMin, w.trigMax);
        range(sq, sizeof(sq), w.sqMax >= 0.0f, w.sqMin, w.sqMax);
        range(f, sizeof(f), w.fMax >= 0.0f, w.fMin, w.fMax);
        range(tp, sizeof(tp), w.tpMax >= 0.0f, w.tpMin, w.tpMax);
        // [PROBE: CTLIN] gate=diag_ctl_input  site=HEADSET  log-only=yes
        LOG("controllers: [CTLIN] %s profile %s | aim flags 0x%lX grip flags 0x%lX (-1 = never located, 0xF = valid + "
            "tracked) | 2 s window, frames %lld: trigger %s, squeeze/value %s, squeeze/force %s, trackpad/force %s "
            "(- = input not active) | grip pressed %lld frames (index_grip_force %d) | stick %s %.2f %.2f",
            h == 0 ? "LEFT " : "RIGHT", prof[h], static_cast<long>(g_aimFlags[h]), static_cast<long>(g_gripFlags[h]),
            w.frames, trig, sq, f, tp, w.gripPress, IndexGripForce ? 1 : 0, g_stickActive[h] ? "on" : "-",
            g_stickX[h], g_stickY[h]);
    }
    // [PROBE: CTLIN] gate=diag_ctl_input  site=HEADSET  log-only=yes
    LOG("controllers: [CTLIN] buttons (active/value) A %d/%d B %d/%d X %d/%d Y %d/%d | menu: boolean action %d/%d, "
        "Index trackpad latch %d | L3 %d/%d R3 %d/%d",
        g_buttonActive[0], g_buttonValue[0], g_buttonActive[1], g_buttonValue[1], g_buttonActive[2], g_buttonValue[2],
        g_buttonActive[3], g_buttonValue[3], g_menuActive, g_menuValue, g_menuLatch ? 1 : 0,
        g_stickClickActive[0], g_stickClickValue[0], g_stickClickActive[1], g_stickClickValue[1]);
    g_cwStart = now;
    g_cw[0] = CtlWin{};
    g_cw[1] = CtlWin{};
}

}  // namespace

void ControllersSync(XrSession session)
{
    if (session == XR_NULL_HANDLE || g_actionSet == XR_NULL_HANDLE) {
        return;
    }
    XrActiveActionSet active{ g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (!Ok(xrSyncActions_(session, &si), "xrSyncActions")) {
        // finger tracking: no state this frame -> not live, so FingerTrackApply leaves the authored hand (never a stale curl)
        InterlockedExchange(&g_ftActive[0], 0);
        InterlockedExchange(&g_ftActive[1], 0);
        return;
    }

    LogActiveInteractionProfiles(session);

    // Queued haptic pulses (PulseController), applied HERE so xrApplyHapticFeedback shares the thread
    // xrSyncActions runs on. Before the trigger early-return below, so a missing trigger action cannot
    // silently swallow them.
    if (g_haptic != XR_NULL_HANDLE && xrApplyHapticFeedback_) {
        for (int h = 0; h < 2; ++h) {
            LONG64 packed = InterlockedExchange64(&g_hapticPending[h], 0);
            if (packed == 0) {
                // No pulse of ours this frame: the game's rumble, if it is vibrating the pad (SetGameRumble).
                const LONG rumble = InterlockedCompareExchange(&g_gameRumbleMilli, 0, 0);
                if (rumble <= 0) {
                    continue;
                }
                packed = (static_cast<LONG64>(rumble) << 32) | static_cast<LONG64>(kGameRumblePulseMs);
            }
            XrHapticVibration vib{ XR_TYPE_HAPTIC_VIBRATION };
            vib.amplitude = static_cast<float>(packed >> 32) / 1000.0f;
            vib.duration = static_cast<XrDuration>(packed & 0xFFFFFFFF) * 1000000;  // ms -> ns
            vib.frequency = XR_FREQUENCY_UNSPECIFIED;
            XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO };
            hi.action = g_haptic;
            hi.subactionPath = g_handPath[h];
            const XrResult r = xrApplyHapticFeedback_(
                session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
            InterlockedExchange(&g_hapticLast[h], static_cast<LONG>(r));
            if (XR_SUCCEEDED(r)) {
                InterlockedIncrement(&g_hapticOk[h]);
            } else {
                InterlockedIncrement(&g_hapticFail[h]);
            }
        }
    }

    if (g_trigger == XR_NULL_HANDLE) {
        InterlockedExchange(&g_ftActive[0], 0);   // finger tracking: the finger block below is never reached -> not live
        InterlockedExchange(&g_ftActive[1], 0);
        return;
    }
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = g_trigger;
        gi.subactionPath = g_handPath[h];
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat_(session, &gi, &state)) && state.isActive) {
            const float prev = g_triggerValue[h];
            g_triggerValue[h] = state.currentState;
            g_triggerActive[h] = true;
            if (h == kRightHand && prev < kAimLatchThreshold &&
                g_triggerValue[h] >= kAimLatchThreshold) {
                InterlockedIncrement(&g_latchGeneration);
            }
        } else {
            g_triggerActive[h] = false;
        }
    }

    if (g_stick != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; ++h) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = g_stick;
            gi.subactionPath = g_handPath[h];
            XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
            if (XR_SUCCEEDED(xrGetActionStateVector2f_(session, &gi, &state)) && state.isActive) {
                g_stickX[h] = state.currentState.x;
                g_stickY[h] = state.currentState.y;
                g_stickActive[h] = true;
            } else {
                g_stickActive[h] = false;
            }
        }
    }

    if (g_stickClick != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; ++h) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = g_stickClick;
            gi.subactionPath = g_handPath[h];
            XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &state)) && state.isActive) {
                g_stickClickValue[h] = state.currentState != XR_FALSE;
                g_stickClickActive[h] = true;
            } else {
                g_stickClickActive[h] = false;
            }
        }
    }

    if (g_squeeze != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; ++h) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = g_squeeze;
            gi.subactionPath = g_handPath[h];
            XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(xrGetActionStateFloat_(session, &gi, &state)) && state.isActive) {
                g_squeezeRaw[h] = state.currentState;
                g_squeezeRawActive[h] = true;
            } else {
                g_squeezeRawActive[h] = false;
            }
        }
    }

    // Valve Index scalars - see g_squeezeForce. Inactive on every other profile (nothing bound), so they cost two
    // state reads per hand and change nothing there.
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.subactionPath = g_handPath[h];
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        if (g_squeezeForce != XR_NULL_HANDLE) {
            gi.action = g_squeezeForce;
            state = XrActionStateFloat{ XR_TYPE_ACTION_STATE_FLOAT };
            g_squeezeForceActive[h] = XR_SUCCEEDED(xrGetActionStateFloat_(session, &gi, &state)) && state.isActive;
            g_squeezeForceValue[h] = g_squeezeForceActive[h] ? state.currentState : 0.0f;
        }
        if (g_trackpadForce != XR_NULL_HANDLE) {
            gi.action = g_trackpadForce;
            state = XrActionStateFloat{ XR_TYPE_ACTION_STATE_FLOAT };
            g_trackpadForceActive[h] = XR_SUCCEEDED(xrGetActionStateFloat_(session, &gi, &state)) && state.isActive;
            g_trackpadForceValue[h] = g_trackpadForceActive[h] ? state.currentState : 0.0f;
        }
        // What GetControllerSqueeze hands out: Touch and every other profile = the runtime's value unchanged.
        if (g_profileIsIndex[h] && (g_squeezeRawActive[h] || g_squeezeForceActive[h])) {
            g_squeezeValue[h] = ConditionIndexSqueeze(h);
            g_squeezeActive[h] = true;
        } else {
            // Not Index: exactly the old behaviour - the runtime's value, active when it is.
            g_squeezeValue[h] = g_squeezeRaw[h];
            g_squeezeActive[h] = g_squeezeRawActive[h];
            g_gripLatch[h] = false;
        }
    }
    // FINGER TRACKING (2026-11-11): the touches, then the eased weights, once per frame on this (the frame) thread.
    // COST: 2 x xrGetActionStateBoolean per hand per frame + a few float ops; one QPC read.
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float dt = g_ftLastQpc.QuadPart ? static_cast<float>(now.QuadPart - g_ftLastQpc.QuadPart) / static_cast<float>(freq.QuadPart)
                                        : 0.0f;
        if (dt > 0.1f) { dt = 0.1f; }   // a stall must not snap the fingers more than a tenth of a second's worth
        g_ftLastQpc = now;
        // [LOADBEARING: pause_freeze] the game paused for a menu: the eased weights and their liveness are HELD (Jay after RUN P1: "the
        // finger tracking on pause menus, that was still moving" -> "Yes"; trace docs/traces/20261113_1655_finger_pause_freeze.md).
        const bool ftHold = ::dlvr::uipin::GamePausedForMenu();
        for (int h = 0; h < 2; ++h) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.subactionPath = g_handPath[h];
            XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
            bool trigOk = false, thumbOk = false;
            if (g_trigTouch != XR_NULL_HANDLE) {
                gi.action = g_trigTouch;
                trigOk = XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &st)) && st.isActive;
                g_trigTouchValue[h] = trigOk && st.currentState != XR_FALSE;
            }
            if (g_thumbTouch != XR_NULL_HANDLE) {
                gi.action = g_thumbTouch;
                st = XrActionStateBoolean{ XR_TYPE_ACTION_STATE_BOOLEAN };
                thumbOk = XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &st)) && st.isActive;
                g_thumbTouchValue[h] = thumbOk && st.currentState != XR_FALSE;
            }
            // active = the hand's touch actions AND its squeeze are live this frame; otherwise the hand eases OPEN (spec: never a
            // stale curl - a controller put down or lost)
            g_fingerInputsActive[h] = trigOk && thumbOk && g_squeezeRawActive[h];
            const ft::Inputs in{ g_fingerInputsActive[h], g_trigTouchValue[h], g_thumbTouchValue[h], g_squeezeRaw[h] };
            if (!ftHold) {
                const ft::Weights prev{ g_ftW[h][0], g_ftW[h][1], g_ftW[h][2] };
                const ft::Weights w = ft::Ease(prev, ft::Target(in, FingerTrackFistAt), dt, FingerTrackEaseMs);
                g_ftW[h][0] = w.index;
                g_ftW[h][1] = w.thumb;
                g_ftW[h][2] = w.lower;
                InterlockedExchange(&g_ftActive[h], g_fingerInputsActive[h] ? 1 : 0);
            }
            // the 1 s window for [FINGERTRACK] (frame thread only): raw squeeze min / max and touch edges
            FtWin& fw = g_ftWin[h];
            if (g_squeezeRawActive[h]) {
                if (g_squeezeRaw[h] < fw.sqMin) { fw.sqMin = g_squeezeRaw[h]; }
                if (g_squeezeRaw[h] > fw.sqMax) { fw.sqMax = g_squeezeRaw[h]; }
            }
            if (g_trigTouchValue[h] != fw.lastTrig) { ++fw.trigEdges; fw.lastTrig = g_trigTouchValue[h]; }
            if (g_thumbTouchValue[h] != fw.lastThumb) { ++fw.thumbEdges; fw.lastThumb = g_thumbTouchValue[h]; }
            ++fw.frames;
        }
        // [PROBE: FINGERTRACK] gate=finger_track  site=HEADSET  log-only=yes
        // Once a second while finger_track is on, per hand: live?, touches now, edges, raw squeeze min / max (spec 8's open question:
        // does a light REST move squeeze/value off 0?), the eased weights, and the writer's passes + reason (boneblend.cpp).
        const ULONGLONG nowMs = GetTickCount64();
        if (FingerTrack && nowMs - g_ftWinStartMs >= 1000) {
            for (int h = 0; h < 2; ++h) {
                FtWin& fw = g_ftWin[h];
                long long passes = 0;
                long reason = -1;
                FingerTrackWriteStats(h == kRightHand ? 0 : 1, &passes, &reason);
                LOG("controllers: [FINGERTRACK] %s %u frames | live %s | trigger touch %d (%d edges) thumb touch %d (%d edges) | squeeze "
                    "raw min %.3f max %.3f | weights index %.2f thumb %.2f lower %.2f | writer passes %lld, reason %ld (0 writing, 1 holds "
                    "an item, 2 waiting latch, 3 refused, 4 open, 5 another writer owns it)", h == kRightHand ? "RIGHT" : "LEFT", fw.frames,
                    g_fingerInputsActive[h] ? "yes" : "NO", g_trigTouchValue[h] ? 1 : 0, fw.trigEdges, g_thumbTouchValue[h] ? 1 : 0,
                    fw.thumbEdges, fw.sqMin <= 1.0f ? fw.sqMin : -1.0f, fw.sqMax >= 0.0f ? fw.sqMax : -1.0f,
                    g_ftW[h][0], g_ftW[h][1], g_ftW[h][2], passes, reason);
                const bool keepTrig = fw.lastTrig, keepThumb = fw.lastThumb;   // a touch held across the boundary is not an edge
                fw = FtWin{};
                fw.lastTrig = keepTrig;
                fw.lastThumb = keepThumb;
            }
            g_ftWinStartMs = nowMs;
        }
    }

    if (g_trackpadForceActive[kLeftHand] && g_profileIsIndex[kLeftHand]) {
        IndexLatch(g_menuLatch, g_trackpadForceValue[kLeftHand]);
    } else {
        g_menuLatch = false;
    }

    if (g_menu != XR_NULL_HANDLE) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = g_menu;
        gi.subactionPath = XR_NULL_PATH;
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &state)) && state.isActive) {
            g_menuValue = state.currentState != XR_FALSE;
            g_menuActive = true;
        } else {
            g_menuActive = false;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (g_button[i] == XR_NULL_HANDLE) {
            g_buttonActive[i] = false;
            continue;
        }
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = g_button[i];
        gi.subactionPath = XR_NULL_PATH;
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &state)) && state.isActive) {
            g_buttonValue[i] = state.currentState != XR_FALSE;
            g_buttonActive[i] = true;
        } else {
            g_buttonActive[i] = false;
        }
    }

    CtlInputProbe();
}

namespace {

// One pose through its filter. Switched off, it returns the raw pose unchanged and drops the filter's state, so
// switching it back on starts from the hand's current pose rather than one from minutes ago.
XrPosef SteadyStep(steady::PoseFilter& f, const XrPosef& raw, float dt, const steady::Params& k, bool enabled)
{
    if (!enabled) {
        f.Reset();
        return raw;
    }
    steady::V3 op;
    steady::Q oq;
    f.Update({ raw.position.x, raw.position.y, raw.position.z },
             { raw.orientation.x, raw.orientation.y, raw.orientation.z, raw.orientation.w }, dt, k, op, oq);
    XrPosef o;
    o.position = { op.x, op.y, op.z };
    o.orientation = { oq.x, oq.y, oq.z, oq.w };
    return o;
}

// OpenXR forward (0,0,-1) rotated by q.
void Fwd(const XrQuaternionf& q, float out[3])
{
    out[0] = -2.0f * (q.x * q.z + q.w * q.y);
    out[1] = -2.0f * (q.y * q.z - q.w * q.x);
    out[2] = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
}

float AngDeg(const float a[3], const float b[3])
{
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return acosf(c) * 57.2957795f;
}

// One located grip frame into the [STEADY] window. STILL is decided from the RAW step (< 20 deg/s), not from the
// filter, so the classification is the same with the filter on or off.
void SteadyProbe(int h, const XrPosef& raw, const XrPosef& st, const XrPosef& ml, float dt)
{
    SteadyWin& w = g_sw[h];
    float fr[3], fs[3], fm[3];
    Fwd(raw.orientation, fr);
    Fwd(st.orientation, fs);
    Fwd(ml.orientation, fm);
    ++w.frames;
    const float lag = AngDeg(fr, fs);
    if (lag > w.maxLag) { w.maxLag = lag; }
    const float mlag = AngDeg(fr, fm);
    if (mlag > w.meleeMaxLag) { w.meleeMaxLag = mlag; }
    if (w.havePrev && dt > 0.0f) {
        const float rs = AngDeg(fr, w.prevRaw);
        if (rs / dt < kStillRadPerSec * 57.2957795f) {
            ++w.still;
            w.rawStep += rs;
            w.steadyStep += AngDeg(fs, w.prevSteady);
            w.meleeStep += AngDeg(fm, w.prevMelee);
        }
    }
    for (int i = 0; i < 3; ++i) { w.prevRaw[i] = fr[i]; w.prevSteady[i] = fs[i]; w.prevMelee[i] = fm[i]; }
    w.havePrev = true;
}

void SteadyReport()
{
    if (!DiagSteady) {
        g_swStart = 0;
        g_sw[0] = SteadyWin{};
        g_sw[1] = SteadyWin{};
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_swStart == 0) { g_swStart = now; }
    if (now - g_swStart < 1000) { return; }
    for (int h = 0; h < 2; ++h) {
        const SteadyWin& w = g_sw[h];
        const char* side = h == 0 ? "LEFT " : "RIGHT";
        if (w.frames == 0) {
            // [PROBE: STEADY] gate=diag_steady  site=HEADSET  log-only=yes
            LOG("controllers: [STEADY] %s grip: frames 0 - NOT TRACKED this window - NO VERDICT", side);
        } else if (w.still == 0 || w.rawStep <= 0.0) {
            // [PROBE: STEADY] gate=diag_steady  site=HEADSET  log-only=yes
            LOG("controllers: [STEADY] %s grip: frames %lld, still 0 - the hand never held still this window, NO VERDICT on "
                "jitter | worst raw-vs-steady %.2f deg, raw-vs-melee %.2f deg (the lag while moving) | on %d strength %d "
                "response %d melee %d",
                side, w.frames, w.maxLag, w.meleeMaxLag, ComfortHandSteady ? 1 : 0, ComfortHandSteadyStrength,
                ComfortHandSteadyResponse, ComfortMeleeSteady);
        } else {
            // [PROBE: STEADY] gate=diag_steady  site=HEADSET  log-only=yes
            LOG("controllers: [STEADY] %s grip: frames %lld, still %lld | still-frame forward-axis step raw %.4f -> steady "
                "%.4f deg/frame (ratio %.2f), melee %.4f (ratio %.2f) | worst raw-vs-steady %.2f deg, raw-vs-melee %.2f deg "
                "| on %d strength %d response %d melee %d",
                side, w.frames, w.still, w.rawStep / w.still, w.steadyStep / w.still, w.steadyStep / w.rawStep,
                w.meleeStep / w.still, w.meleeStep / w.rawStep, w.maxLag, w.meleeMaxLag, ComfortHandSteady ? 1 : 0,
                ComfortHandSteadyStrength, ComfortHandSteadyResponse, ComfortMeleeSteady);
        }
    }
    g_swStart = now;
    for (int h = 0; h < 2; ++h) {
        g_sw[h].frames = g_sw[h].still = 0;
        g_sw[h].rawStep = g_sw[h].steadyStep = g_sw[h].meleeStep = 0.0;
        g_sw[h].maxLag = g_sw[h].meleeMaxLag = 0.0f;
    }
}

}  // namespace

// v' = q v q* (unit quaternion): t = 2 (q.xyz x v); v' = v + w t + q.xyz x t.
static void RotateByQuat(const XrQuaternionf& q, const float v[3], float out[3])
{
    const float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    const float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    const float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

void ControllersLocate(XrTime predictedDisplayTime, XrSpace appSpace)
{
    if (appSpace == XR_NULL_HANDLE || !predictedDisplayTime) {
        return;
    }
    constexpr XrSpaceLocationFlags kNeeded =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    // Hand steadiness: the filter's time step is the gap between the display times the poses are predicted for.
    // First call, or time running backwards: a step the filter reads as a gap (> kMaxDt), i.e. start from raw.
    // The same display time located twice gives 0, which the filter treats as "nothing new" (holds its output).
    float steadyDt = 1.0f;
    if (g_lastLocateTime != 0 && predictedDisplayTime >= g_lastLocateTime) {
        steadyDt = static_cast<float>(static_cast<double>(predictedDisplayTime - g_lastLocateTime) * 1e-9);
    }
    g_lastLocateTime = predictedDisplayTime;
    const steady::Params steadyK = steady::FromSliders(ComfortHandSteadyStrength, ComfortHandSteadyResponse);
    const steady::Params meleeK = steady::MeleeFromSlider(ComfortMeleeSteady);
    const bool meleeOn = ComfortMeleeSteady > 0;
    bool gripSeen = false;   // any grip located valid in this call - the HANDOFFSET line waits for one
    for (int h = 0; h < 2; ++h) {
        if (g_handSpace[h] == XR_NULL_HANDLE) {
            continue;
        }
        // The grip is located FIRST (same display time, same app space) so the hand offset's world delta - the grip-frame
        // offset turned by THIS locate's grip orientation - can shift the aim pose too (trace 20261116_1750_grip_offset_sliders).
        // Publishing order and every skip below are as before: a failed aim locate still skips the grip's publish.
        XrSpaceLocation gloc{ XR_TYPE_SPACE_LOCATION };
        const bool gripLocated = g_gripSpace[h] != XR_NULL_HANDLE &&
                                 XR_SUCCEEDED(xrLocateSpace_(g_gripSpace[h], appSpace, predictedDisplayTime, &gloc));
        // The hand offset: the shifted controller point every grip / aim getter returns. 0 = skipped (exact).
        // COST: per hand with a non-zero offset, one quaternion rotate + 6 adds.
        float hoD[3] = { 0.0f, 0.0f, 0.0f };
        bool hoShift = false;
        if (gripLocated && (gloc.locationFlags & kNeeded) == kNeeded) {
            float off[3];
            if (RigHandOffsetLocalM(static_cast<Hand>(h), off)) {
                RotateByQuat(gloc.pose.orientation, off, hoD);
                hoShift = true;
            }
        }
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace_(g_handSpace[h], appSpace, predictedDisplayTime, &loc))) {
            continue;
        }
        InterlockedIncrement64(&g_poseFrames[h]);
        InterlockedExchange(&g_aimFlags[h], static_cast<LONG>(loc.locationFlags));
        if ((loc.locationFlags & kNeeded) == kNeeded) {
            if (hoShift) {
                loc.pose.position.x += hoD[0];
                loc.pose.position.y += hoD[1];
                loc.pose.position.z += hoD[2];
            }
            const XrPosef aimSteady = SteadyStep(g_steadyAim[h], loc.pose, steadyDt, steadyK, ComfortHandSteady);
            const XrPosef aimMelee = SteadyStep(g_meleeAim[h], loc.pose, steadyDt, meleeK, meleeOn);
            InterlockedIncrement(&g_poseSeq[h]);   // odd: write in progress
            MemoryBarrier();
            g_pose[h] = loc.pose;
            g_poseSteady[h] = aimSteady;
            g_poseMelee[h] = aimMelee;
            MemoryBarrier();
            InterlockedIncrement(&g_poseSeq[h]);   // even: settled
            g_valid[h] = true;
            // End of a dropout: publish the run length, then reset it. Counting the
            // BOUT separately from the FRAMES matters - 600 single-frame misses and
            // one 600-frame miss are different defects and a total cannot tell them
            // apart (INV-5's family: one number, two populations).
            InterlockedExchange(&g_poseMissNow[h], 0);
        } else {
            // Controller went out of tracking - hold the last-known pose
            // rather than snap it to identity, same reasoning as the head
            // pose's own invalid-bit handling in ovr2xr.cpp.
            g_valid[h] = false;
            g_steadyAim[h].Reset();   // re-acquisition starts from the raw pose, never blends across the gap
            g_meleeAim[h].Reset();
            InterlockedIncrement64(&g_poseMissRun[h]);
            const LONG now = InterlockedIncrement(&g_poseMissNow[h]);
            if (now == 1) { InterlockedIncrement64(&g_poseMissBouts[h]); }
            if (now > g_poseMissMax[h]) { InterlockedExchange(&g_poseMissMax[h], now); }
        }
        // The grip pose, located for the SAME predicted display time and app
        // space as the aim pose above, so the two describe the same instant.
        // Its own seqlock for the same torn-read reason documented on
        // g_poseSeq - this is written by the frame thread and read by the rig
        // on a different one.
        if (g_gripSpace[h] == XR_NULL_HANDLE) {
            continue;
        }
        if (!gripLocated) {   // located above, before the aim block
            continue;
        }
        InterlockedExchange(&g_gripFlags[h], static_cast<LONG>(gloc.locationFlags));
        if ((gloc.locationFlags & kNeeded) == kNeeded) {
            gripSeen = true;
            if (hoShift) {
                gloc.pose.position.x += hoD[0];
                gloc.pose.position.y += hoD[1];
                gloc.pose.position.z += hoD[2];
            }
            const XrPosef gripSteady = SteadyStep(g_steadyGrip[h], gloc.pose, steadyDt, steadyK, ComfortHandSteady);
            const XrPosef gripMelee = SteadyStep(g_meleeGrip[h], gloc.pose, steadyDt, meleeK, meleeOn);
            InterlockedIncrement(&g_gripSeq[h]);   // odd: write in progress
            MemoryBarrier();
            g_gripPoseVal[h] = gloc.pose;
            g_gripSteady[h] = gripSteady;
            g_gripMelee[h] = gripMelee;
            MemoryBarrier();
            InterlockedIncrement(&g_gripSeq[h]);   // even: settled
            g_gripValid[h] = true;
            if (!g_steadyArmedSaid) {
                g_steadyArmedSaid = true;
                // [PROBE: STEADY] gate=comfort_hand_steady  site=HEADSET  log-only=no
                LOG("controllers: [STEADY] ARMED (logged unconditionally, first located grip) - comfort_hand_steady %d | "
                    "strength %d -> still cutoff %.2f Hz | response %d -> beta %.1f Hz per m/s, %.2f Hz per rad/s | "
                    "comfort_melee_steady %d -> still cutoff %.2f Hz (max-response curve%s) | diag_steady %d | steadied: the "
                    "rig's hand input (the melee copy while melee is held), aimhook's controller aim, grappleaim's hand "
                    "aim; RAW: swing detector, holster, probes",
                    ComfortHandSteady ? 1 : 0, ComfortHandSteadyStrength, steadyK.minCutoffHz, ComfortHandSteadyResponse,
                    steadyK.betaPos, steadyK.betaRot, ComfortMeleeSteady, meleeK.minCutoffHz,
                    meleeOn ? "" : " - OFF, the melee copy is raw", DiagSteady ? 1 : 0);
            }
            if (DiagSteady) {
                SteadyProbe(h, gloc.pose, gripSteady, gripMelee, steadyDt);
            }
        } else {
            g_gripValid[h] = false;
            g_steadyGrip[h].Reset();
            g_meleeGrip[h].Reset();
            InterlockedIncrement64(&g_gripMissRun[h]);
        }
    }
    // The HANDOFFSET line: logged unconditionally at the first located grip, then when a value changes (at most once a second; a value
    // changed inside that second is logged by the next call after it). COST: a 6-float compare per call.
    if (gripSeen) {
        static bool      s_hoSaid = false;
        static float     s_hoLast[6] = { 0, 0, 0, 0, 0, 0 };
        static ULONGLONG s_hoMs = 0;
        const float cur[6] = { RigHandOffsetRFwdCm, RigHandOffsetRUpCm, RigHandOffsetRRightCm,
                               RigHandOffsetLFwdCm, RigHandOffsetLUpCm, RigHandOffsetLRightCm };
        const bool changed = !s_hoSaid || memcmp(cur, s_hoLast, sizeof(cur)) != 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (changed && (!s_hoSaid || nowMs - s_hoMs >= 1000)) {
            // [PROBE: HANDOFFSET] gate=none (always)  site=HEADSET  log-only=yes
            LOG("controllers: [HANDOFFSET] %s - controller grip offset (cm, grip frame) RIGHT fwd %+.1f up %+.1f right %+.1f | "
                "LEFT fwd %+.1f up %+.1f right %+.1f | applied to the raw grip + aim positions before steadiness (0 = not applied)",
                s_hoSaid ? "CHANGED" : "FIRST (logged unconditionally)", static_cast<double>(cur[0]), static_cast<double>(cur[1]),
                static_cast<double>(cur[2]), static_cast<double>(cur[3]), static_cast<double>(cur[4]), static_cast<double>(cur[5]));
            memcpy(s_hoLast, cur, sizeof(cur));
            s_hoSaid = true;
            s_hoMs = nowMs;
        }
    }
    SteadyReport();
}

// The readout for the line BoneBlendTick prints. Returns false and fills nothing if
// the hand index is out of range, rather than handing back zeros that would read as
// "no dropouts" - INV-6, fail closed.
bool GetPoseMissStats(int hand, PoseMissStats& out)
{
    if (hand < 0 || hand > 1) { return false; }
    out.frames   = g_poseFrames[hand];
    out.misses   = g_poseMissRun[hand];
    out.bouts    = g_poseMissBouts[hand];
    out.maxRun   = g_poseMissMax[hand];
    out.nowRun   = g_poseMissNow[hand];
    out.gripMiss = g_gripMissRun[hand];
    return true;
}

bool GetControllerPose(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_valid[hand]) {
        return false;
    }
    // Seqlock read - see the note on g_poseSeq. Bounded retries: losing this
    // many races in a row means the writer is running hot, and holding the
    // previous pose for one tick is far better than returning a torn one.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_poseSeq[hand];
        if (s0 & 1) {
            continue;   // mid-write
        }
        MemoryBarrier();
        const XrPosef p = g_pose[hand];
        MemoryBarrier();
        if (g_poseSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

bool ControllerPoseValid(Hand hand)
{
    return (hand == kLeftHand || hand == kRightHand) && g_valid[hand];
}

// The grip pose. Identical seqlock contract to GetControllerPose above, on its
// own counter - see controllers.h for what distinguishes grip from aim, and why
// this is NOT the squeeze button.
bool GetControllerGripPose(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_gripValid[hand]) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_gripSeq[hand];
        if (s0 & 1) {
            continue;   // mid-write
        }
        MemoryBarrier();
        const XrPosef p = g_gripPoseVal[hand];
        MemoryBarrier();
        if (g_gripSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

// Hand steadiness - the filtered copies, same seqlocks and same contract as the raw getters above (false, output
// untouched, when the hand has no valid pose). See controllers.h for who may read them.
bool GetControllerPoseSteady(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_valid[hand]) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_poseSeq[hand];
        if (s0 & 1) {
            continue;
        }
        MemoryBarrier();
        const XrPosef p = g_poseSteady[hand];
        MemoryBarrier();
        if (g_poseSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

bool GetControllerGripPoseSteady(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_gripValid[hand]) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_gripSeq[hand];
        if (s0 & 1) {
            continue;
        }
        MemoryBarrier();
        const XrPosef p = g_gripSteady[hand];
        MemoryBarrier();
        if (g_gripSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

// The melee copies (comfort_melee_steady) - same seqlocks, same contract.
bool GetControllerPoseMelee(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_valid[hand]) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_poseSeq[hand];
        if (s0 & 1) {
            continue;
        }
        MemoryBarrier();
        const XrPosef p = g_poseMelee[hand];
        MemoryBarrier();
        if (g_poseSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

bool GetControllerGripPoseMelee(Hand hand, XrPosef& outPose)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_gripValid[hand]) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_gripSeq[hand];
        if (s0 & 1) {
            continue;
        }
        MemoryBarrier();
        const XrPosef p = g_gripMelee[hand];
        MemoryBarrier();
        if (g_gripSeq[hand] == s0) {
            outPose = p;
            return true;
        }
    }
    return false;
}

bool ControllerGripPoseValid(Hand hand)
{
    return (hand == kLeftHand || hand == kRightHand) && g_gripValid[hand];
}

long ControllerGripFlags(Hand hand)
{
    return (hand == kLeftHand || hand == kRightHand) ? g_gripFlags[hand] : -1;
}

XrSpace ControllerGripSpace(Hand hand)
{
    return (hand == kLeftHand || hand == kRightHand) ? g_gripSpace[hand] : XR_NULL_HANDLE;
}

bool GetControllerTrigger(Hand hand, float& outValue)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_triggerActive[hand]) {
        return false;
    }
    outValue = g_triggerValue[hand];
    return true;
}

bool GetControllerStick(Hand hand, float& outX, float& outY)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_stickActive[hand]) {
        return false;
    }
    outX = g_stickX[hand];
    outY = g_stickY[hand];
    return true;
}

bool GetControllerStickClick(Hand hand, bool& outPressed)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_stickClickActive[hand]) {
        return false;
    }
    outPressed = g_stickClickValue[hand];
    return true;
}

bool GetControllerSqueeze(Hand hand, float& outValue)
{
    if ((hand != kLeftHand && hand != kRightHand) || !g_squeezeActive[hand]) {
        return false;
    }
    outValue = g_squeezeValue[hand];
    return true;
}

bool GetControllerFingerInputs(int h, ft::Inputs* out)
{
    if (!out) { return false; }
    if (h != kLeftHand && h != kRightHand) { out->active = false; return false; }
    out->active = g_fingerInputsActive[h];
    out->triggerTouch = g_trigTouchValue[h];
    out->thumbTouch = g_thumbTouchValue[h];
    out->squeeze = g_squeezeRaw[h];
    return out->active;
}

bool GetControllerFingerWeights(int h, ft::Weights* out)
{
    if (!out || (h != kLeftHand && h != kRightHand)) { return false; }
    out->index = g_ftW[h][0];
    out->thumb = g_ftW[h][1];
    out->lower = g_ftW[h][2];
    return g_ftActive[h] != 0;
}

const char* FingerTrackBoundProfile() { return g_ftBoundProfile; }

bool GetControllerMenuButton(bool& outPressed)
{
    // Valve Index: the left trackpad press, through the hysteresis (g_menuLatch, ControllersSync). The Index row binds
    // nothing to the boolean menu action, so the two never both apply.
    if (g_trackpadForceActive[kLeftHand] && g_profileIsIndex[kLeftHand]) {
        outPressed = g_menuLatch;
        return true;
    }
    if (!g_menuActive) {
        return false;
    }
    outPressed = g_menuValue;
    return true;
}

bool ControllerLeftIsIndexProfile() { return g_profileIsIndex[kLeftHand]; }

bool GetControllerButton(FaceButton button, bool& outPressed)
{
    if (button < 0 || button > kButtonY || !g_buttonActive[button]) {
        return false;
    }
    outPressed = g_buttonValue[button];
    return true;
}

long GetAimTriggerGeneration()
{
    return g_latchGeneration;
}

void SetGameRumble(float amplitude)
{
    if (!(amplitude > 0.0f)) { amplitude = 0.0f; }
    if (amplitude > 1.0f) { amplitude = 1.0f; }
    InterlockedExchange(&g_gameRumbleMilli, static_cast<LONG>(amplitude * 1000.0f + 0.5f));
}

bool PulseController(Hand hand, float amplitude, int durationMs)
{
    if ((hand != kLeftHand && hand != kRightHand) || g_haptic == XR_NULL_HANDLE) {
        return false;
    }
    if (amplitude < 0.0f) { amplitude = 0.0f; }
    if (amplitude > 1.0f) { amplitude = 1.0f; }
    if (durationMs < 1) { durationMs = 1; }
    const LONG64 milli = static_cast<LONG64>(amplitude * 1000.0f + 0.5f);
    InterlockedExchange64(&g_hapticPending[hand], (milli << 32) | static_cast<LONG64>(durationMs));
    return true;
}

bool GetHapticStats(Hand hand, long& outOk, long& outFail, int& outLastResult)
{
    if (hand != kLeftHand && hand != kRightHand) {
        return false;
    }
    outOk = g_hapticOk[hand];
    outFail = g_hapticFail[hand];
    outLastResult = static_cast<int>(g_hapticLast[hand]);
    return true;
}

void ControllersShutdown()
{
    for (int h = 0; h < 2; ++h) {
        if (g_handSpace[h] != XR_NULL_HANDLE && xrDestroySpace_) {
            xrDestroySpace_(g_handSpace[h]);
            g_handSpace[h] = XR_NULL_HANDLE;
        }
        if (g_gripSpace[h] != XR_NULL_HANDLE && xrDestroySpace_) {
            xrDestroySpace_(g_gripSpace[h]);
            g_gripSpace[h] = XR_NULL_HANDLE;
        }
        g_gripValid[h] = false;
        InterlockedExchange(&g_gripFlags[h], -1);
        g_valid[h] = false;
        g_triggerActive[h] = false;
        g_triggerValue[h] = 0.0f;
        g_stickActive[h] = false;
        g_stickX[h] = 0.0f;
        g_stickY[h] = 0.0f;
        g_stickClickActive[h] = false;
        g_stickClickValue[h] = false;
        g_squeezeActive[h] = false;
        g_squeezeValue[h] = 0.0f;
        g_squeezeRawActive[h] = false;
        g_squeezeRaw[h] = 0.0f;
        g_squeezeForceActive[h] = false;
        g_squeezeForceValue[h] = 0.0f;
        g_trackpadForceActive[h] = false;
        g_trackpadForceValue[h] = 0.0f;
        g_gripLatch[h] = false;
        g_trigTouchValue[h] = false;
        g_thumbTouchValue[h] = false;
        g_fingerInputsActive[h] = false;
        g_ftW[h][0] = g_ftW[h][1] = g_ftW[h][2] = 0.0f;
        InterlockedExchange(&g_ftActive[h], 0);
        g_profileIsIndex[h] = false;
        g_profileSeen[h] = false;
        g_profileFailSaid[h] = false;
        InterlockedExchange(&g_aimFlags[h], -1);
        g_hapticPending[h] = 0;
    }
    g_menuActive = false;
    g_menuValue = false;
    g_menuLatch = false;
    g_squeezeForce = XR_NULL_HANDLE;
    g_trackpadForce = XR_NULL_HANDLE;
    g_indexProfilePath = XR_NULL_PATH;
    for (int i = 0; i < 4; ++i) {
        g_buttonActive[i] = false;
        g_buttonValue[i] = false;
        g_button[i] = XR_NULL_HANDLE;
    }
    if (g_actionSet != XR_NULL_HANDLE && xrDestroyActionSet_) {
        xrDestroyActionSet_(g_actionSet);
        g_actionSet = XR_NULL_HANDLE;
    }
    g_aimPose = XR_NULL_HANDLE;
    g_gripPose = XR_NULL_HANDLE;
    g_trigger = XR_NULL_HANDLE;
    g_stick = XR_NULL_HANDLE;
    g_stickClick = XR_NULL_HANDLE;
    g_squeeze = XR_NULL_HANDLE;
    g_haptic = XR_NULL_HANDLE;
    g_menu = XR_NULL_HANDLE;
    g_lastLoggedProfile[0] = g_lastLoggedProfile[1] = XR_NULL_PATH;
    g_instance = XR_NULL_HANDLE;
}

}  // namespace dlvr
