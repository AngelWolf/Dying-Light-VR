// Motion-controller pose tracking, on our own OpenXR action set.
//
// The base game predates Touch-style controllers entirely (2015) and has no
// concept of a hand-tracked aim source to redirect into - LibOVR's own
// ovr_GetInputState()/hand poses are stubbed to report "no controllers" (see
// ovr2xr.cpp). This is us adding controller awareness from scratch, so the
// aim-redirection hook (see CLAUDE.md's "The full chain, traced to an actual
// engine raycast" section) has a real pose to read.
#pragma once

#include <openxr/openxr.h>

#include "fingertrack_logic.h"   // ft::Inputs / ft::Weights (finger tracking, 2026-11-11)

namespace dlvr {

enum Hand { kLeftHand = 0, kRightHand = 1 };

// Creates the action set, the aim-pose action (with left/right subaction
// paths), suggests bindings for the generic khr/simple_controller profile
// (universal fallback) and oculus/touch_controller (what Virtual Desktop and
// Quest present as), attaches the action set to the session, and creates the
// two action spaces. Call once, right after appSpace exists - mirrors where
// xrCreateReferenceSpace() is called for it in EnsureSession().
bool ControllersInit(XrInstance instance, XrSession session, XrSpace appSpace);

// xrSyncActions() is a frame-boundary call, like xrWaitFrame/xrBeginFrame -
// call this once per frame, right after xrBeginFrame succeeds, on whichever
// thread owns the frame boundary (BeginFrameIfNeeded in ovr2xr.cpp).
void ControllersSync(XrSession session);

// Locating a space does not block and is safe from any thread, same as
// LocateViews() for the head - call this wherever the view pose is located,
// with the same predicted display time and app space, so the controller pose
// is predicted for the same instant as the frame being built.
void ControllersLocate(XrTime predictedDisplayTime, XrSpace appSpace);

// hand is kLeftHand/kRightHand. False if this hand has never been reported
// active (space create failed, or the runtime has never located it) - the
// output pose is left untouched in that case, not zeroed, so a caller that
// ignores the return value does not get a snap-to-origin.
bool GetControllerPose(Hand hand, XrPosef& outPose);
bool ControllerPoseValid(Hand hand);

// THE GRIP POSE - a SECOND, DIFFERENT pose for the same physical controller.
// Added 2026-09-10 for the rig; the aim pose above is unchanged and still the
// right thing for aiming.
//
// OpenXR reports two standard poses per hand and they are not interchangeable:
//   aim  - the POINTING RAY. Deliberately angled off the hand and tuned PER
//          CONTROLLER MODEL so pointing feels natural. Correct for shooting.
//   grip - ANATOMY. Origin at the palm centroid, orientation tied to how the
//          hand wraps the grip. Correct for placing a virtual hand.
//
// Why the rig needs this specifically: the fixed rotation from controller frame
// to hand-bone frame is only a UNIVERSAL constant if the controller frame is
// itself anatomy-defined. Fed by `aim`, that constant silently absorbs each
// controller's pointing-ray tilt - which differs between Touch, Index and Vive
// BY DESIGN - so no single baked value can be correct for every user, and the
// grip ends up needing per-user calibration. Fed by `grip`, it is one constant
// for everyone. That is the whole reason this exists.
//
// NOT TO BE CONFUSED WITH GetControllerSqueeze() BELOW. OpenXR overloads the
// word: the physical grip BUTTON is `input/squeeze/value` (a float, mapped to
// Sony's L1/R1 bits in dlvr_FilterScePad), while this is `input/grip/pose` (a
// transform, no button semantics at all). They are separate subpaths and do
// not interact.
//
// Same contract as GetControllerPose: false, output untouched, if the space
// was never created or the runtime has never located it. Callers that need a
// pose no matter what should fall back to the aim pose rather than an identity.
bool GetControllerGripPose(Hand hand, XrPosef& outPose);
bool ControllerGripPoseValid(Hand hand);
// The grip's raw XrSpaceLocationFlags from the latest locate (valid or not), -1 = never located / bad hand.
// Diagnostic only - read separately from the pose, so it can belong to a different locate than a pose read beside it.
long ControllerGripFlags(Hand hand);
// The hand's grip action space itself (XR_NULL_HANDLE = never created / bad hand) - for a composition layer the runtime holds on
// the controller (the wrist HUD quads, wristquad.cpp). Made at session start, destroyed at session teardown.
XrSpace ControllerGripSpace(Hand hand);

// HAND STEADINESS (VR Comfort tab, SPEC-VR-COMFORT-2026-09-30 §6, C1). The same two poses through a 1-euro filter
// (steady_logic.h): tremor and tracking noise removed while the hand is still, almost no lag on a deliberate move.
// Filtered once per frame by the writer, published under the SAME seqlock as the raw pose, so a steady read is exactly
// as consistent as a raw one. With comfort_hand_steady=0 the steady copy IS the raw pose.
// USE THEM ONLY WHERE A STEADY HAND IS WANTED - the rig's hand input (the drawn gun, and so the shot under
// fire_aim_from_barrel, the reticle and the grapple-from-gun) and the controller-aim shot paths. NEVER for anything
// that measures speed: the swing detector and the holster read the raw getters above.
bool GetControllerPoseSteady(Hand hand, XrPosef& outPose);
bool GetControllerGripPoseSteady(Hand hand, XrPosef& outPose);
extern bool ComfortHandSteady;          // comfort_hand_steady
extern int  ComfortHandSteadyStrength;  // comfort_hand_steady_strength 0-100
extern int  ComfortHandSteadyResponse;  // comfort_hand_steady_response 0-100
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSteady = false;
#else
extern bool DiagSteady;                 // diag_steady - the [STEADY] per-second windows
#endif
extern bool IndexGripForce;             // index_grip_force - Valve Index grips from squeeze/force (1) or squeeze/value (0)
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagCtlInput = false;
#else
extern bool DiagCtlInput;               // diag_ctl_input - the 2 s per-hand controller input lines
#endif
// The VERY slight melee copies (comfort_melee_steady, 0 = OFF = raw): the max-response filter, ~2 mm / ~1 deg at swing
// speed. Read by the rig while the right hand holds melee; the swing DETECTOR still reads the raw getters.
bool GetControllerPoseMelee(Hand hand, XrPosef& outPose);
bool GetControllerGripPoseMelee(Hand hand, XrPosef& outPose);
extern int  ComfortMeleeSteady;         // comfort_melee_steady 0-100

// Analog trigger pull, 0..1. False (outValue untouched) if the action isn't
// active for this hand - e.g. no controller bound yet. Backed by the same
// action set as the pose above; read after ControllersSync() each frame.
bool GetControllerTrigger(Hand hand, float& outValue);

// Thumbstick, -1..1 per axis (OpenXR's own Vector2f convention: +x right,
// +y forward/up). False (outputs untouched) if the action isn't active for
// this hand. Added 2026-09-02 for left-stick movement / right-stick smooth
// turning (see dlvr_FilterScePad in ovr2xr.cpp) - same action set, read the
// same way as the trigger above.
bool GetControllerStick(Hand hand, float& outX, float& outY);

// Thumbstick click (L3/R3 equivalent). False (outPressed untouched) if the
// action isn't active for this hand. Confirmed 2026-09-02: left thumbstick
// click is genuinely Dying Light's sprint binding (Sony's standard L3 bit,
// 0x2, in the scePad buttons mask - see dlvr_FilterScePad). Right hand (R3)
// added 2026-09-03 - same action, same accessor, just a second bound path.
bool GetControllerStickClick(Hand hand, bool& outPressed);

// Analog grip/squeeze pull, 0..1 - same shape and contract as the trigger
// above (backed by the same action set, read after ControllersSync()).
// Touch's squeeze is analog-only (no boolean click sub-path exists on the
// real oculus/touch_controller profile), so callers that want a discrete
// press should threshold this themselves, the same way `vr_sprint`'s stick
// click already comes in as a clean boolean instead. Added 2026-09-03 for
// the still-unbound grip buttons - see dlvr_FilterScePad for where this
// lands in the scePad buttons mask.
// VALVE INDEX (2026-10-14): on an Index hand the value is conditioned - squeeze/force (or squeeze/value with
// index_grip_force=0) through a 0.5 on / 0.3 off hysteresis, reported >= 0.5 while pressed and < 0.5 while not, so
// a caller's own 0.5 threshold sees the hysteresis. Every other profile: the runtime's value, unchanged.
bool GetControllerSqueeze(Hand hand, float& outValue);

// FINGER TRACKING (2026-11-11, spec 2026-11-11-controller-finger-tracking-design). h is a Hand (kLeftHand 0 / kRightHand 1 -
// NOTE the rig's side index is the other way round, 0 = right). Inputs: this frame's trigger touch, any-thumb touch and the raw
// squeeze/value; returns false (out->active false) unless the hand's touch actions and squeeze are all live. Weights: the eased
// index / thumb / lower weights (0 OPEN .. 1 CLOSED), computed once per frame in ControllersSync; returns whether the inputs are
// live (the weights ease to 0 while they are not). FingerTrackBoundProfile: the profile whose suggestion accepted the touch paths,
// or nullptr.
bool GetControllerFingerInputs(int h, ft::Inputs* out);
bool GetControllerFingerWeights(int h, ft::Weights* out);
const char* FingerTrackBoundProfile();
extern bool  FingerTrack;        // finger_track - write the tracked fingers on an empty hand
extern float FingerTrackEaseMs;  // finger_track_ease_ms
extern float FingerTrackFistAt;  // finger_track_fist_at

// Left controller's menu button. Not a Hand-parameterized action like the
// pair above - Touch's menu/click input only exists on the LEFT controller
// at all (the right controller's system button is reserved by the runtime,
// never exposed to an app), so there is no right-hand equivalent to ask
// for. False (outPressed untouched) if the action isn't active. Added
// 2026-09-03, previously flagged as "not bound to anything yet" in
// CLAUDE.md.
// VALVE INDEX (2026-10-14): the left trackpad press (trackpad/force through the 0.5 on / 0.3 off hysteresis).
bool GetControllerMenuButton(bool& outPressed);

// Face buttons - not symmetric across hands like the stick/trigger above, so
// a single enum rather than a Hand parameter: A/B are the right Touch
// controller's pair, X/Y the left's, matching Touch's real physical layout.
// Bound to touch_controller only (khr/simple_controller has no face buttons
// at all, same reasoning as thumbstick above). False (outPressed untouched)
// if the action isn't active. See dlvr_FilterScePad for where these land in
// the scePad buttons mask (Sony's Cross/Circle/Square/Triangle bits, the
// same "genre convention" bet already confirmed correct for L3/R2).
enum FaceButton { kButtonA = 0, kButtonB = 1, kButtonX = 2, kButtonY = 3 };
bool GetControllerButton(FaceButton button, bool& outPressed);

// Bumped by one every time the right trigger crosses the fire threshold - a
// burst-boundary marker, not a captured direction (an earlier version also
// pre-captured a pose here; removed, see CLAUDE.md's aim-redirection
// section - capturing at the raw trigger-crossing instant risked catching
// the hand still mid-motion into the aim, before it had actually settled).
// 0 before the first ever crossing. A consumer that wants ONE shot's whole
// burst of hits to use the SAME frozen direction - captured fresh at the
// first hit of the burst, not re-read on every hit, which can drift mid-
// burst if the burst runs long enough in real time (confirmed happening in
// practice 2026-09-02) - should remember the generation it last saw and only
// treat a change as "a new shot has begun", not on every hit or a timer.
long GetAimTriggerGeneration();

// ===== [POSEMISS] - B-1 of docs/PROPOSAL-FIX-SHAKE-AND-SNAP-2026-09-24.md =====
// A "miss" is one frame where xrLocateSpace returned WITHOUT both the position and
// orientation valid bits, so GetControllerPose returns false, SolveArmIk returns
// early (boneblend.cpp:6909) and the rig keeps writing LAST frame's rotations.
// `bouts` counts dropout EVENTS, `maxRun` the longest one in frames: 600 one-frame
// misses and one 600-frame miss are different defects and a total cannot separate
// them. Counted unconditionally; only the LOG line is gated on diag_pose_miss.
struct PoseMissStats {
    long long frames   = 0;   // frames located, this session, this hand
    long long misses   = 0;   // frames the aim pose was NOT valid
    long long bouts    = 0;   // dropout events (transitions into a miss)
    long      maxRun   = 0;   // longest consecutive miss run, frames
    long      nowRun   = 0;   // consecutive misses right now (0 = tracking)
    long long gripMiss = 0;   // same for the grip pose
};
bool GetPoseMissStats(int hand, PoseMissStats& out);

// ===== HAPTICS - docs/SPEC-SHOULDER-HOLSTER-2026-09-29.md §3 =====
// Queue ONE vibration pulse on this hand. It is applied by the NEXT ControllersSync() on the frame thread
// (<= 1 frame late), never from the caller's thread, so xrApplyHapticFeedback always runs on the same
// thread as xrSyncActions and no OpenXR call is made from the scePad poll. A newer pulse replaces one still
// pending. amplitude 0..1 (clamped), durationMs >= 1 (clamped). False, nothing queued, if the vibration
// action was never created.
bool PulseController(Hand hand, float amplitude, int durationMs);

// The GAME's rumble (spec 2026-10-25-game-rumble-to-touch-haptics-design): the stronger of the pad's two motors, 0..1 (0 = stop).
// Applied to BOTH hands by every ControllersSync while non-zero, where no pulse of ours is pending. Any thread.
void SetGameRumble(float amplitude);

// What ControllersSync got back from the pulses it applied this session: ok / fail counts and the last
// XrResult (0 = none applied yet). A pulse the runtime silently ignores reads as ok - the result code is
// the only thing this can see. False if hand is out of range.
bool GetHapticStats(Hand hand, long& outOk, long& outFail, int& outLastResult);

void ControllersShutdown();

}  // namespace dlvr
