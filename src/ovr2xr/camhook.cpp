#include "camhook.h"

#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>      // streambudget: the engine's ID3D11Device (rd3d11 ppDevice)
#include <dxgi1_4.h>    // streambudget: IDXGIAdapter3::QueryVideoMemoryInfo (stream_budget_mb = -1)
#include <objbase.h>    // shadowstagger: CoTaskMemFree (the Documents path for the video.scr PCSS check)
#include <psapi.h>      // shadowstagger: GetProcessMemoryInfo (process commit in the 30 s line - the queue-leak control, INV-587)

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/buildrva.h"   // EngRva - the allowlisted EGL rvas (afwalt, jitvr, rastocc, clipfar, eyeaxis, ...)
#include "../common/log.h"
#include "../common/pattern.h"
#include "boneblend.h"   // DiagBoneBlendProbe - the damage watch must refuse to arm while the bone probe holds the exec-watch slot
#include "controllers.h"
#include "headpose.h"
#include "afwgate_logic.h"   // the AFW rebuild gate's stale-camera / eye-position checks (afwcam::GateImage)
#include "rtticam.h"
#include "settings.h"
#include "swing.h"
#include "meleethrow.h"
#include "lockpitch.h"  // LockPitchInstall - lockpick position P1 (the lockpick look asks for the head's pitch)
#include "pelvisturn.h" // PelvisTurnInstall - the lower body turns with the stick when looking down (INV-749)
#include "dlgview.h"    // DlgViewInstall - a dialogue's view gets the cam_eye_* offset it gets on foot (INV-619)
#include "boltwheel.h"  // BoltWheelInstall - the crossbow bolt wheel from the bolt holster (INV-678)
#include "invkeep.h"    // InvKeepInstall - the inventory keeps its built list between unchanged opens (INV-526 addendum 2026-11-14)
#include "buggywheel.h" // BuggyWheelInstall - the buggy's key + gripped wheel (INV-716)
#include "vehpick_logic.h"   // RdVehPick's held entry (tests/vehpick_logic_test.cpp)
#include "cinlook.h"    // CinLookInstall - a cutscene's wanted look stops feeding the head yaw back (INV-620 (a))
#include "cinbars.h"    // CinBarsInstall - no cutscene side bars in VR (INV-620 (b))
#include "cinyaw_logic.h"   // SplitYawPitchRoll - the cutscene camera's yaw / pitch (INV-620 (c) ADDENDUM 2)
// INV-620 (c) CamHookCutsceneTick: the scene view's engine rva, translated on the Epic build in the BETA tree (buildrva_table.h
// "current scene view"); the DEV tree has no buildrva.h - Steam rvas only (bodygrab.cpp's pattern).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_CIN_EN_RVA(rva) ::dlvr::buildrva::Eng(rva)
#define DLVR_VEH_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_CIN_EN_RVA(rva) (rva)
#define DLVR_VEH_GD_RVA(rva) (rva)
#endif
#include "watchwrite.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs

// The assembly thunk, and the two symbols it needs from us.
extern "C" {
void dlvr_CamCopyThunk();
void* dlvr_CamCopyTrampoline = nullptr;
void dlvr_OnCameraCopied(void* dst, const void* src);
}

namespace dlvr {

// An engine rva as the LOADED build has it: unchanged on Steam and unknown builds, the allowlisted EGL rva on the
// Epic build (common/buildrva.h). Cost: one cached-build test, plus a ~40-entry binary search on EGL only.
inline uintptr_t EngRva(uintptr_t steamRva)
{
    return ::dlvr::buildrva::Rva(::dlvr::buildrva::kEngine, steamRva);
}

// Reached from the thunk's callback, so these live outside the anonymous
// namespace.
volatile uintptr_t g_targets[kMaxHookTargets] = {};
volatile LONG64 g_targetHits[kMaxHookTargets] = {};
volatile LONG64 g_calls = 0;
volatile LONG64 g_levelled = 0;
volatile LONG g_faulted = 0;
void LevelCamera(uintptr_t obj);

// What the last camera we touched actually looked like. Written without a lock
// from an engine thread: a torn read only ever misdraws one frame of a readout,
// and taking a lock in here would be far worse than that.
CamProbe g_probe = {};

// How often the levelling used a head from the previous frame, and how often it
// ran at all. Plain interlocked counters - nothing in here may take a lock.
volatile LONG64 g_staleHead = 0;
volatile LONG64 g_levelCount = 0;

namespace {

// RVA of the CCamera assignment, from the write-watch plus the disassembly.
// Only a first guess: it is right for the Epic build and wrong for the Steam
// one, which is a separate compile of the same version. When it does not fit,
// the signature below finds the function instead.
constexpr uintptr_t kCamCopyHintRva = 0x3E9C0;

// The copy is a long unrolled run of `mov eax,[rdx+n] / mov [rcx+n],eax`, which
// on its own appears two dozen times in the module - the compiler inlines the
// same shape all over. What makes it findable is the int3 padding in front of
// it: the pattern has to start at a function boundary, and only this one does.
const dlvr::SitePattern kCamCopyPats[] = {
    { "CC CC 8B 42 10 89 41 10 8B 42 14 89 41 14 8B 42 18 89 41 18", 2 },
};
uintptr_t g_camCopyRva = 0;
constexpr size_t kStolen = 15;      // five whole three-byte instructions
const char* kEngine = "engine_x64_rwdi.dll";

typedef void*(__fastcall* PFN_CamCopy)(void* dst, const void* src);
uint8_t* g_trampoline = nullptr;

uintptr_t g_patched = 0;
uint8_t g_saved[kStolen] = {};
bool g_installed = false;
char g_status[256] = "not installed";
char g_reject[128] = "";
}  // namespace

bool CamHookWriteView = false;
// [DEP: comfort_cutscene_pitch] required-for=INV-620 (c) ADDENDUM 2 - in a cutscene every levelled camera keeps the cutscene camera's own
//      pitch under the head's (the bunk shot looks up at the bunk). Menu "Flatscreen Cutscene Animations" (VR Comfort; Jay 2026-10-25:
//      "We'll do it as 1, with a toggle in VR Comfort for it"). (CutscenePitch) tree=LOADBEARING when on. LIVE (read per copy).
//      DEFAULT ON: Jay's option 1 after the run ("That worked") - OFF = the level-pitch cutscene of the earlier build (head pitch only).
bool CutscenePitch = true;
bool CamHookKeepHead = true;
int CamHookHeadPitchAxis = 0;
int CamHookHeadRollAxis = 2;
bool CamHookHeadPitchFlip = false;
bool CamHookHeadRollFlip = false;
bool CamHookWriteCombined = true;
bool CamHookCombinedPre = false;
bool CamHookDerive = true;
bool CamHookAuto = true;
bool CamHookTransposed = false;
HookOp CamHookOp = HookOp::Observe;
float CamHookDeltaDeg = 20.0f;
// How many times the bone probe arms the exec watch looking for a held item
// before it gives up. DEFAULT 20, WHICH IS THE SHIPPED BEHAVIOUR UNCHANGED.
// 20 attempts x ~5.6 s = a window that closes ~114 s after PROCESS START, not
// after gameplay starts - measured 2026-09-22: a run that reached gameplay at
// ~90 s had 24 s of margin and a run that did not reach it at all logged
// "gave up after 20 attempts" at 21:25:39, 114 s after its 21:23:45 launch.
// The loop exits on success (!g_done), so a larger value costs nothing on a
// run that captures - the live run of the same evening caught on attempt 10.
int BoneDumpAttempts = 60;   // DEFAULT 60 2026-10-01 (Tier D: Jay's live value)
int CamHookAxis = 0;
bool CamHookPreMultiply = false;
bool CamHookLookBack = false;  // DEFAULT OFF 2026-10-03 (Jay: "disable as shipped default aswell, it needs to be worked on"); was ON 2026-10-01 as Tier B
float CamHookLookBackEaseMs = 400.0f;   // DEFAULT 400 2026-10-01 (Tier D: Jay's live tuning)
// A full 180 turned out to expose real streaming/culling holes on the far
// side (see CLAUDE.md) - defaulting to something closer to how a person
// actually glances behind themselves while running (head rotation plus a
// little torso twist) rather than a full reversal.
float CamHookLookBackAngleDeg = 140.0f;
bool DiagPlayerMovementModule = false;
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagKillPhysicsDebugRender = false;
bool DiagFeedSimCamera = false;
bool DiagEyeRotationCompare = false;
#endif
// [DEP: diag_eye_axis] required-for=[EYEAXIS] (does the STEREO BASELINE follow the rendered view? INV-260)
//      tree=PROBE-ONLY. Log-only: reads the two eye cameras' 3x4 world matrices once per frame at the
//      frame boundary (CamHookEyeAxisTick), writes nothing, installs nothing, takes no WatchExec slot.
//      DEFAULT OFF. HEADSET: the eye cameras (helper+88/+96) and LastReportedHead only exist on the VR
//      path, and the question is answered by HEAD ROLL, which needs a headset. Also needs camhook
//      levelling the eye cameras (cam_auto=1, horizon_lock=1, cam_keep_head=1) for the view to roll at all.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagEyeAxis = false;
#endif
// [DEP: diag_view_pos] required-for=[VIEWPOS] (design D run 2, Jay: something still feels wrong whilst turning the head -
//      does the view's YAW lag the head differently from its PITCH, and does the camera's POSITION follow the tracked head?)
//      2026-09-30 (head-motion fidelity step 1): + POS-H / POS-V per window - the camera POSITION's lag behind the tracked
//      head, and the mm it moves that the head did not (path and drift), with no 20 mm gate.
//      2026-09-30 (attribution of INV-308): + STAB per window - the head stabiliser's L / O / p / w / DI+4464 (INV-306) read
//      through the player's PlayerFppVis. ALSO INSTALLS the gamedll BFF080 pass-through detour (boneblend BonesAnimInstall)
//      for its `this` latch; with every anim_* setting and diag_bones_anim off, that detour only latches and calls the original.
//      tree=PROBE-ONLY. Log-only: once per frame at the frame boundary (CamHookViewPosTick) reads the real head pose, the view
//      yaw/pitch/position camhook's anchor published, the rig mapping and both sticks; writes nothing. DEFAULT OFF.
//      HEADSET: head poses, a chosen yaw anchor (VR camera path) and the controller sticks.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagViewPos = false;
#endif
// [DEP: diag_render_pose] required-for=[RENDPOSE] (SPEC-RENDERED-ORIENTATION-2026-09-30 Stage 1: can each submitted frame
//      be paired with the head yaw design D put into its camera, and how far does that rendered yaw trail the pose we
//      declare? INV-266's 2-frame yaw lag, the turn "swim" candidate, D-164 (2))
//      tree=PROBE-ONLY. Log-only: camyaw::Hook pushes {+4460 after the add, the yaw added, the stripped-yaw quaternion} to an
//      8-entry ring each LOCAL player update; LevelCamera's anchor block snapshots the ring with the engine camera's yaw and
//      the reported head; the submit pairs the latest snapshot BY VALUE and compares it with the declared pose. Writes
//      nothing, installs nothing (rides the existing camyaw detour, which only design D installs for yaw). DEFAULT OFF.
//      HEADSET + head_yaw_decouple=1: the ring is fed only when D adds the head yaw; refuses BY NAME when D is off or
//      use_render_pose=1.
//      2026-10-01 (INV-325 (a)/(b)): + [RENDTAG]. ALSO INSTALLS a PASS-THROUGH detour on rd3d11's pose recorder
//      (sub_18003B9B0, slot 164; boneblend rendtag::Install, from BoneBlendTick → needs diag_bone_blend_probe) - arguments
//      unchanged, it only records which capture is newest when the frame's pose is recorded. Installed only if this is 1.
bool DiagRenderPose = false;
// [DEP: declare_rendered_orientation] required-for=SPEC-RENDERED-ORIENTATION Stage 2 (R3) - the turn "swim" fix candidate:
//      declare to OpenXR, per submitted frame, the orientation it was RENDERED with, so the compositor removes INV-266's
//      yaw lag (INV-323: the rendered yaw trails the declared pose ~1-3 deg on fast turns).
//      tree=INERT. Was DEFAULT OFF, LIVE (overlay, "VR Comfort" tab since 2026-10-01 - Jay: a master enable there for ease of
//      access; the tab shows CamHookRendDeclStatus). When ON and every check passes it is LOADBEARING for the
//      VIEW: ovr_SubmitFrame's projection views carry q_rendered (x) the eye's own offset instead of the located views'
//      orientation; position unchanged. Pairing (CamHookRendPoseSubmit, Stage 2b, INV-326): the submit's frameIndex -> its
//      OWN pose record (rd3d11 recorder detour, boneblend rendtag) -> the capture that record named; its ring entry at
//      position 1, checked by value against INV-323's mapping; any failure -> today's pose for that frame, counted by
//      reason in [RENDDECL]. (Stage 2 paired on the game's RenderPose echo - wrong ~99%, D-165/INV-326.) Needs
//      head_yaw_decouple=1 (latched) and use_render_pose=0 (refuses by name), the camyaw detour live AND the rendtag
//      recorder detour (both install from BoneBlendTick → diag_bone_blend_probe; rendtag installs on the first tick this
//      or diag_render_pose is 1), and the camhook yaw anchor. HEADSET.
//      DEFAULT ON since 2026-10-01 (Jay: loadbearing on his disk -> ships on). Refuses by name while head_yaw_decouple=0.
//      LOCKED ON 2026-10-07 (Jay: "an absolute neccessity to be on ... force enabled and hide the setting toggle for it, that is now
//      considered loadbearing"): S_BOOL_LOCKED - the file value is ignored, the menu toggle is gone, this compiled true is the value.
bool DeclareRenderedOrientation = true;
// [DEP: head_yaw_decouple] required-for=R10 head-yaw decouple (SPEC-HEAD-YAW-DECOUPLE C1-C4)
//      tree=INERT. Was DEFAULT OFF. When ON it is LOADBEARING for the VIEW: C1 strips yaw from the head
//      reported to the engine (ovr2xr.cpp ovr_GetTrackingState), and C2 below is then the ONLY thing
//      that turns the rendered view with the head - with C1 on and C2 wrong, view yaw freezes to the
//      body. Latched once at startup (HeadYawDecoupleActive(), ovr2xr.cpp); refused while
//      use_render_pose=1. Consumers: camhook C2, boneblend C3 (SolveArmIk root-yaw block), aimhook C4.
// LOCKED ON 2026-10-07 (S_BOOL_LOCKED; Jay: "Yes" - lock declare_rendered_orientation's prerequisites): file value ignored, no toggle.
bool HeadYawDecouple = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: head_yaw_decouple_flip] required-for=C2's yaw SIGN in the engine's convention ([UNKNOWN], spec C2)
//      tree=INERT. Was DEFAULT OFF. Only read while head_yaw_decouple is active. Decided by [HEADTURN]'s
//      "view yaw" slope: +1 = correct, -1 = flip this. Latched with head_yaw_decouple.
bool HeadYawDecoupleFlip = true;   // DEFAULT ON 2026-10-01 (the measured sign, BUILDS 5b96897f; Tier A)
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagWatchLineAdd = false;
#endif
// Defaults TRUE, unlike every other diag_* flag here, and deliberately so.
// LoadLayerConfig() is only ever called from GameEyeFov()/EyeRenderSize(),
// both of which are VR-only paths - so in a headset-less run the settings
// file is never read at all and every setting keeps its compiled default.
// This probe exists specifically to compare a VR run against a FLATSCREEN
// run, so defaulting it off made the flatscreen half impossible to collect
// (two runs wasted proving exactly that). Read-only and one-shot, so being
// on by default costs nothing. Set back to false once the comparison is done.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagVisibilityProbe = false;  // off: flatscreen comparison deprioritised, and it shares the single exec-watch slot
bool DiagDamageWatch     = false;  // off: takes the single exec-watch slot. See namespace damagewatch.
#endif
// ---- R5 step 3: the melee HIT-RECORD probe (INV-124..127). READ-ONLY. ----
// Takes NO exec-watch slot, NO debug register, NO detour and is NOT on any
// per-frame path - it samples from its own thread off a pointer the melee input
// thunk already latches. It therefore does NOT conflict with diag_bone_dump,
// diag_bone_blend_probe, diag_melee_elem_probe or diag_damage_watch, and it does
// not need diag_damage_watch on: it installs the trigger itself, ABOVE that gate.
// >>> FLAT-VALID. Everything it asks is structural, temporal or a constant read
// (INV-11). Do not spend a headset run on it. <<<
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagMeleeRecordProbe = false;
#endif
// ---- R5 step 4: the melee POSE observation hook (INV-129/130). READ-ONLY. ----
// An inline detour at gamedll+0xDC1C90 - the function every non-seed vertex of
// the melee sweep comes from, and the one Jay chose to substitute later. It
// OBSERVES four arguments and returns; it writes nothing and substitutes
// nothing. Independent of the element hook at 0xC4D920: different address, its
// own trampoline, no shared state, so the two can be on together. FLAT-VALID -
// it asks about a matrix's FORMAT and a call count, not about world values.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagMeleePoseProbe = false;
#endif

// Which gamedll RVA diag_damage_watch traps. 0 = the default,
// 0x2A2830 = HealthModule::AddDamageToBody. Settable because there is more than
// one candidate override and a rebuild per candidate wastes a session - see the
// long note at namespace damagewatch for the measured negative on the default
// and for why 0x279AF0 is next.
float DamageWatchRvaOverride = 0.0f;
// ---- the melee element probe / override (U-24). BOTH default to OFF. ----
// diag_melee_elem_probe moves the single WatchExec slot from SetLastHitBone to
// the SLastHit builder, so it REPLACES [BONEWATCH] rather than joining it.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool  DiagMeleeElemProbe = false;
#endif
// >= 0 arms the WRITE. -1 (the default) observes only. Read as a float because
// that is what the settings layer carries; used as an integer element index.
float MeleeForceElem = -1.0f;
// ---- R5 step 2: the return-address census on the BONE argument (2026-09-17) --
// DESIGN_REGWRITE.md section 8. INV-105 established that the element override at
// the SLastHit builder cannot satisfy R5 on its own - the reported location is
// fixed upstream of it - and that the site which CAN is the bone argument to
// IModelObject::GetMeshElemFromBoneID, an ENGINE export (INV-104), because
// INV-64 has GetElementWorldPos called immediately after it in the same caller.
//
// That argument is register-only, so it needs the write-back (INV-102). Before
// any of that: this census. It takes the SAME WatchExec client 0 slot the melee
// trap uses, so it REPLACES [BONEWATCH] exactly as diag_melee_elem_probe does -
// option (a) of DESIGN_REGWRITE.md section 7, chosen because aimhook's DR2/DR3
// carry four requirements (R3/R4/R6/R9) against this one.
//
// IT WRITES NOTHING. Two questions only: what does this site COST, and which
// return addresses are the melee ones.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool  DiagMeleeBoneCensus = false;
#endif
// How long to hold the census armed, in milliseconds, before disarming and
// reporting. 0 = stay armed for the session.
//
// >>> WHY A WINDOW RATHER THAN WatchExecSetMaxHits. <<< A hit cap disarms
// silently mid-run and everything after it measures an instrument that is no
// longer there - that is what killed the 2026-09-16 10:52 run. A cap also
// destroys the measurement being taken here: frame pacing after a silent
// self-disarm is the pacing of an UNARMED game, which would read as "the site
// costs nothing". The window bounds the exposure in TIME, says when it opened
// and closed, and the hit cap is explicitly lifted to 0 for the duration.
float MeleeCensusMs = 0.0f;   // DEFAULT 0 2026-10-01 (Tier D: Jay's live value; probe off)
// ---- [LOCREAD]: who READS m_LastHitLocation? (2026-09-17) -------------------
// INV-106 found, statically, that +0x688 has a SETTER AND NO GETTER while its
// siblings have both, that the two getters a displacement search turns up belong
// to ItemDescFirearm, and that the only function touching the whole block is a
// field-by-field copy. What it could NOT rule out: a consumer that reads ONLY
// +0x688 (74 candidates in gamedll, indistinguishable by displacement), or one
// in engine reaching the field through an accessor gamedll never sees.
//
// A DR0 data breakpoint in READ-OR-WRITE mode answers both in one run, in
// process, and watchwrite.h's own header names this exact use: "for when the
// writer is already known and the interesting question is who else touches the
// address - a reader that never appears as a writer."
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool  DiagLastHitLocReadWatch = false;
#endif
// How long to hold the read-watch before reporting and disarming, ms.
// 0 = until the 400-hit cap disarms it, which it WILL report.
float LocReadWindowMs = 0.0f;   // DEFAULT 0 2026-10-01 (Tier D: Jay's live value; probe off)
// The OpenXR -> game world yaw, published from LevelCamera. See the comment at
// the point it is written for why this is the anchor the rigging work uses
// rather than anything derived from the skeleton.
volatile float g_openxrToGameYaw     = 0.0f;
volatile LONG  g_haveOpenxrToGameYaw = 0;
// rig_yaw_stick_gate=3 (INV-292): the raw publish, pre-filter, with QPC time - [0] previous, [1] latest. Seqlock: odd =
// writing. Written at the one publish site (once per frame), read by the rig's tail solve.
volatile LONG    g_ryxSeq   = 0;
volatile float   g_ryxY[2]  = {};
volatile LONG64  g_ryxT[2]  = {};
volatile LONG64  g_ryxCount = 0;

// PUBLISHING FROM EXACTLY ONE CAMERA.
//
// The first version published from whichever tracked camera called most
// recently, and it measurably chattered: about 6 degrees of per-tick jump while
// turning, against a true turn rate that could only account for 0.2 to 0.7. Quiet
// when standing still, jumpy when turning - the signature of several objects
// disagreeing and taking turns writing the same global. CamHookAuto tracks as
// many as six at once, and some of them are documented as NOT the cameras
// actually being rendered.
//
// Averaging or smoothing does not solve this; explicit identity checks do. So:
// pick one object and ignore the rest.
//
// WHICH one is chosen by ACTIVITY, not by steadiness. Steadiness alone is a trap:
// a stale or frozen camera scores a perfect zero and would win outright, which is
// exactly the wrong answer. The rendered eyes are distinguished by being hit
// roughly twice as often as anything else - already documented in CLAUDE.md - so
// the busiest object over a short settling window is the one to trust. Steadiness
// is still measured, and logged, so the choice can be audited rather than taken
// on trust.
//
// Both eyes carry identical orientation (the eye-pose comparison measured
// angleBetween at 0.0000 degrees), so anchoring on either is equivalent.
struct YawCandidate {
    uintptr_t obj;
    LONG64    hits;
    float     prevYawDeg;
    float     movementDeg;   // accumulated |change|, for the audit line
    ULONGLONG lastSeenMs;
};
constexpr int kMaxYawCandidates = 8;
YawCandidate  g_yawCand[kMaxYawCandidates] = {};
volatile LONG g_yawCandCount   = 0;
uintptr_t     g_yawAnchor      = 0;

// fire_reticle_3d (grappleaim.cpp): the RENDERED view's full camera matrix (the anchor camera's 12 floats AFTER the
// rotation write below - engine yaw x HEAD pitch/roll, engine position), seqlock-published for the gun dot. The engine's
// look camera (CameraFPPDI, INV-272) carries the drifting LOOK pitch instead; placing by it put the dot off vertically
// (run reticle3d_run1). One 48-byte copy per anchor levelling.
volatile LONG g_rvSeq = 0;
// Every written camera matrix, a ring (main-menu latch). Writers: whichever thread runs the hook; each entry is its own
// seqlock (odd = being written, 0 = never written).
constexpr int kWvRing = 32;
struct WrittenView { volatile LONG seq; ULONGLONG ms; uintptr_t obj; float m[12]; };
WrittenView   g_wv[kWvRing] = {};
volatile LONG g_wvHead = 0;
volatile bool g_wvWanted = false;
// ui_panel_main_menu_turn: the ONE camera object (the view's cached render camera) whose levelling also gets the head's
// yaw; 0 = none. Set by uipin.cpp each submit. The two floats are for its log (radians).
volatile uintptr_t g_mmTurnRoot = 0;
// PAGE SWITCH CATCH (2026-11-15, trace docs/traces/20261115_1700_mm_view_switch_yaw.md; Jay: "Go for it"): each main-menu page is its
// own CLevelView with its own cached camera, and uipin names the root one submit late - so the frame the engine switches view was
// drawn with no head yaw but declared with it (run 4: declared - added = +1 at every switch, 0 on still pages) = the 1-frame shake.
// sub_1802B4EF0 sets AADDA8 = this frame's view, then copies its cache into AAABF0 (cull) and then AAAE40 (eyes) (ida mmo1 / mmo2),
// so the copy hook catches the switch at the AAABF0 copy and gives the new cache the yaw before the eyes' copy is made.
// Addresses resolved once by CamHookSetMainMenuTurnRoot (0 = no catch). Counters read by uipin's 1 s block.
volatile uintptr_t g_mmEngCull = 0;      // engine+AAABF0 (EGL AA62D0): the cull copy, an object
volatile uintptr_t g_mmEngView = 0;      // engine+AADDA8 (EGL AA9278): pointer to the view being set up
constexpr size_t   kMmViewCache = 0x90398;   // CLevelView -> its cached render camera (INV-537 add. 3)
volatile LONG g_mmViewCaught = 0, g_mmViewRefused = 0;
volatile LONG g_mmTurnHits = 0;
volatile LONG g_mmTurnForced = 0;    // root copies levelled ONLY because it is the root (AutoShouldLevel said "still")
float g_mmTurnAdded = 0.0f, g_mmTurnEngineYaw = 0.0f;
// v4 (INV-537 add. 8): the stripped head-yaw quaternion (OpenXR) the root was last given, and when. The anchor's capture
// (taken downstream of the root, the same frame) copies it, and the submit declares the main-menu frame with it.
float g_mmTurnSy[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
volatile ULONGLONG g_mmTurnSyMs = 0;     // 0 = never / not under the decouple
// submit thread: main-menu submits declared with the root's yaw / with no tagged capture / tagged capture without the yaw
volatile LONG g_mmDeclOk = 0, g_mmDeclNoTag = 0, g_mmDeclNoYaw = 0;
float g_mmDeclMaxDeg = 0.0f;             // largest |declared yaw - located yaw| among them (the correction handed over)
// CUTSCENE HEAD TURN (INV-620 (c), spec 2026-10-25-cutscene-camera-design): the main-menu route above, with a second source of the
// root. boneblend's camyaw::Hook says when CinematicController owns the look and the raw OpenXR yaw of the stripped head at its start
// (g_cinActive / g_cinY0Bits, game thread); CamHookCutsceneTick (submit thread, once per submit) then names the root (the scene view's
// cached render camera, as uipin does for the menu) in g_cinRoot. ONE writer each. The root gets (head yaw - y0): the snap. The
// declaration keeps the FULL stripped yaw (rendered = (anim - y0) + head, so the image is right at the real head).
volatile LONG      g_cinActive = 0;
volatile LONG      g_cinY0Bits = 0;
volatile uintptr_t g_cinRoot = 0;
volatile LONG      g_cinAdds = 0, g_cinDeclOk = 0, g_cinDeclNoTag = 0, g_cinDeclNoYaw = 0, g_cinRelFix = 0;
float              g_cinAddedDeg = 0.0f;
// The root camhook gives the head's yaw to: the main menu's (uipin) or, with none, the cutscene's. One compare per camera copy.
inline uintptr_t TurnRoot() { return g_mmTurnRoot ? g_mmTurnRoot : g_cinRoot; }
inline bool TurnRootIsCutscene(uintptr_t root) { return root != 0 && !g_mmTurnRoot && root == g_cinRoot; }
// BUGGY STEADY COCKPIT (A) - spec docs/superpowers/specs/2026-10-29-buggy-steady-cockpit-design.md, Jay: "Yes, build A". In a
// vehicle the camera's yaw = the look (+4460: the car's actual heading + the head, VehicleController +D97050) PLUS CarBuggyCamera's
// own term (spring / travel blend / drift / stick look, +34A5D0, ida bq2 / bq10 / bq11) - the leftover judder (run B13). That term
// is taken out ONCE, at the scene view's cached render camera (the cutscene / main-menu root, INV-537 add. 3-7, INV-620 (c)), so the
// cull copy and the eyes' copy inherit it. CamHookCutsceneTick names the root while the local player is in a vehicle and no
// cutscene / menu root is set; the copy hook levels it on every copy; LevelCamera rotates it by -term (VehCockpitTerm). The stick
// look and possibly look-back go with it (spec, item 3a). A compile-time switch, no settings key - for the test build.
// [LOADBEARING: veh_cockpit] kVehCockpit - without it the buggy's view swings with CarBuggyCamera's own term again (B13's judder):
// it gates the root naming (CamHookCutsceneTick), the forced levelling (dlvr_OnCameraCopied) and the cancel (LevelCamera ->
// VehCockpitTerm). false = INV-649's behaviour exactly. NOT verified in the headset.
// BETA false since 2026-10-30 (8) (Jay, tester package: "Switch both off") - B14 "Still judder", the judder work is paused.
constexpr bool     kVehCockpit = false;
volatile uintptr_t g_vehRoot = 0;   // submit thread writes (CamHookCutsceneTick); 0 = not in a vehicle / another root owns it
// THE BUGGY LEAN (2026-11-10, RUN BK1 - Jay: "to lean forward towards the wheel, I had to lean backwards in real life" -> "Yes, build
// both"; vehlean_logic.h). In a vehicle +C08100 (head -> eye) is off and the buggy camera is rigid to the car, so the view never
// carries the head. The rig maps the head's offset into the world (its own controller map) and publishes it; the root below gets it
// added to its translation once per copy - the scene view's cached render camera, which the cull and eye copies are made FROM, so
// they all inherit it and it cannot stack (the main menu's yaw precedent, INV-537 add. 3). Named whenever the local player is in a
// vehicle, kVehCockpit or not. COST: one compare per camera copy; on the root's copy one seqlock read + three adds.
volatile uintptr_t g_leanRoot = 0;   // submit thread writes (CamHookCutsceneTick)
volatile LONG      g_localInVeh = 0; // ditto: RdLocalPlayerInVehicle() == 1 at the last submit
volatile LONG      g_vlSeq = 0;      // seqlock over g_vlOff / g_vlMs (the rig writes, the copy hook reads)
float              g_vlOff[3] = {};
volatile LONG64    g_vlMs = 0;
volatile LONG64    g_vlApplied = 0, g_vlStale = 0;
bool CamHookLocalInVehicle() { return g_localInVeh != 0; }
void CamHookPublishVehicleHeadOffset(const float d[3])
{
    InterlockedIncrement(&g_vlSeq);
    MemoryBarrier();
    g_vlOff[0] = d[0]; g_vlOff[1] = d[1]; g_vlOff[2] = d[2];
    g_vlMs = static_cast<LONG64>(GetTickCount64());
    MemoryBarrier();
    InterlockedIncrement(&g_vlSeq);
}
// false = nothing fresh (no publish within 150 ms, or every read raced a write) - never a zero offset.
bool VehLeanRead(float d[3])
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG s0 = g_vlSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        const float a = g_vlOff[0], b = g_vlOff[1], c = g_vlOff[2];
        const LONG64 ms = g_vlMs;
        MemoryBarrier();
        if (g_vlSeq != s0) { continue; }
        if (static_cast<LONG64>(GetTickCount64()) - ms > 150) { return false; }
        if (!(a == a && b == b && c == c)) { return false; }
        d[0] = a; d[1] = b; d[2] = c;
        return true;
    }
    return false;
}
volatile LONG      g_vcForced = 0;  // vehicle-root copies levelled only because they are the root (AutoShouldLevel had dropped them)
// The yaw a cutscene's root gets (radians, about the engine's up axis, the menu's sign rule applied): the stripped head yaw minus the
// one latched at the cutscene's start. *sy = the full stripped yaw (what the frame is declared with). False without the decouple.
// COST: one 16-byte copy, one atan2, a wrap - once per root copy.
static bool CinRootYaw(float* yawRad, XrQuaternionf* sy)
{
    if (!HeadYawDecoupleActive()) { return false; }
    *sy = LastStrippedHeadYaw();
    float y0 = 0.0f;
    const LONG bits = g_cinY0Bits;
    memcpy(&y0, &bits, sizeof(y0));
    float r = 2.0f * atan2f(sy->y, sy->w) - y0;
    while (r > 3.14159265f) { r -= 6.28318531f; }
    while (r < -3.14159265f) { r += 6.28318531f; }
    if (!HeadYawDecoupleFlipLatched()) { r = -r; }
    *yawRad = r;
    return true;
}
// ADDENDUM 2 (cutscene PITCH, Jay: "Go pitch only then please"): while a cutscene turn is on every levelled copy keeps the cutscene
// camera's pitch - latched from the ROOT's arriving camera (the engine's own, no head in it) - under the head's pitch / roll, and takes its
// yaw from a proper split (cinyaw::SplitYawPitchRoll, exact near +-90 deg where YawOnly is not). Roll is NOT kept (horizon level).
float         g_cinAnimPitch = 0.0f;      // radians, about CamHookHeadPitchAxis
volatile LONG g_cinAnimPitchOk = 0;       // 1 once a root copy latched it this cutscene
volatile LONG g_cinPitchCopies = 0, g_cinSplitRefused = 0;
inline bool CinPitchOn() { return g_cinActive != 0 && TurnRootIsCutscene(TurnRoot()); }
// The arriving camera's yaw and pitch in camhook's own axes (up, CamHookHeadPitchAxis, CamHookHeadRollAxis). False = the axes are not
// distinct (a settings mistake) - counted, and the copy keeps YawOnly. COST: one 3x3 build, one asin, two atan2.
static bool CinSplit(const float q[4], float* yaw, float* pitch)
{
    float roll = 0.0f;
    const int up = CamUpAxis() == 1 ? 2 : 1;
    if (!cinyaw::SplitYawPitchRoll(q, up, CamHookHeadPitchAxis, CamHookHeadRollAxis, yaw, pitch, &roll)) {
        InterlockedIncrement(&g_cinSplitRefused);
        return false;
    }
    return true;
}
static void AxisQuat(int ax, float ang, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = cosf(ang * 0.5f);
    out[ax] = sinf(ang * 0.5f);
}
volatile LONG64 g_wvCount = 0;
inline void WrittenViewRecord(const float* cam, uintptr_t obj)
{
    const LONG i = InterlockedIncrement(&g_wvHead);
    WrittenView& w = g_wv[static_cast<unsigned>(i) & (kWvRing - 1)];
    LONG s = InterlockedIncrement(&w.seq);
    if (!(s & 1)) { s = InterlockedIncrement(&w.seq); }   // make it odd whatever it was
    MemoryBarrier();
    for (int k = 0; k < 12; ++k) { w.m[k] = cam[k]; }
    w.obj = obj;
    w.ms = GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement(&w.seq);                          // even = stable
    InterlockedIncrement64(&g_wvCount);
}
float         g_rvMtx[12] = {};
ULONGLONG     g_rvMs = 0;

// PAIRING A FRESH HEAD WITH THE CAMERA - the cause of the yaw jitter.
//
// The frame-phase counter is unambiguous: "100998 camera levelings used a head
// from the previous frame, of 100998". The camera is levelled more often than
// the head pose is refreshed, so camYaw - headYaw pairs a MOVING camera against
// a FROZEN head. While turning, the camera advances between samples and the head
// does not, and the difference jitters by exactly that inter-sample motion -
// near zero at rest, growing with turn rate, which is the 3 to 8.5 deg/tick
// signature that was measured.
//
// Gating on HeadSampledThisFrame() is the obvious fix and would publish NEVER,
// since it is stale 100% of the time. Publishing once per HEAD CHANGE instead
// pairs each fresh head with the camera as it stands at that instant.
//
// A light exponential filter on top, because body yaw is a slowly varying
// quantity and the residual is not worth chasing further. Same technique as the
// aim path''s head smoothing, which is confirmed working in the headset at 60ms.
float         g_lastHeadQ[4]   = {0,0,0,0};
volatile LONG g_haveLastHeadQ  = 0;
// One publish per FRAME, not per head change - see the long note at the publish
// site. Cleared while the engine has this frame's head (g_headSampled == 1) and
// set by whichever levelling call publishes first once it goes back to 0.
volatile LONG g_publishedThisFrame = 0;
float         g_yawFiltered    = 0.0f;
volatile LONG g_haveYawFilter  = 0;
// QPC TICKS, not milliseconds - the name is kept so the diff stays readable, but
// GetTickCount64's ~15.625 ms resolution was coarser than a frame and froze the
// filter. See the publication site.
LONGLONG      g_yawLastMs      = 0;
ULONGLONG     g_yawSettleStart = 0;

float CamHookYawSmoothingMs = 40.0f;   // 0 disables; see the yaw publication site
// [LOADBEARING: ROOT-1] diag_bone_dump. NOT a read-only dump switch, despite the name.
// [DEP: DiagBoneDump] required-for=the player model capture  tree=LOADBEARING
//   :7499  if (!DiagBoneDump && !DiagRigStage1..4) { Sleep(1000); continue; }
//   -> the WatchExec capture loop never arms -> BoneProbeCapturedModel() stays NULL
//   -> boneblend.cpp:6953 returns false every tick. The rig is dead, silently.
//   ⚠ It is also a WatchExec CLIENT, and there is ONE global slot - see rig_exec_client2
//   on the next line, and docs/RIG_LOADBEARING.md §4 CONSTRAINT-1.
bool DiagBoneDump = true;       // rigging stage 0 - DEFAULT ON 2026-10-01: a rig master enable (INV-1), Tier B
bool RigExecClient2 = false;    // bone probe -> exec client 1 (DR0). See camhook.h.
// Logging left over from investigations that have since CONCLUDED - the look-back
// mechanism, the yaw-source hunt, the det<0 rejects. It was unconditional and
// produced 18,000+ of the 24,425 lines in a single run: 54% from the look-back
// baseline line alone, on a feature that was finished days ago.
//
// That is not free. LogTo holds a critical section across fopen/fprintf/fclose
// per line, so on a per-camera-per-frame path it is a real cost and, worse, a
// moving variable in any performance measurement. Off by default; turn it on if a
// camera problem needs the reject reasons back.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagCamhookVerbose = false;
bool DiagRigStage1 = false;     // rigging stage 1 - null round-trip write test
#endif
bool DiagRigStage2 = false;     // rigging stage 2 - static pose write on the engine thread
float RigStage2AngleDeg = 60.0f;
bool  RigNotifyAnimBind = true;   // the engine's own post-write blend notify
bool  RigStage2UseWorld = true;   // narrow test: world matrices, not local
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool  DiagRigStage3 = false;      // rigging stage 3 - bone-array numbering dump (read-only)
#endif
bool  DiagRigStage4 = false;      // rigging stage 4 - block + write, the engine's own idiom
float RigNotifyWeight = 1.0f;
// SHIPS ON - Tier A (Jay 2026-10-15 "Yes", after culliso_run2 "That's done it"): the screen-size cull at 0 brings back the far
// buildings that vanished at the centre of the view (INV-578; the visibility range alone did nothing, D-231). Launching with it on
// beside the rig's capture watch: 43 of 43 in-level launches acquired the ILevel AND latched both shoulders (STATE, lodon.py tally).
bool LodOverride = true;
// Range + cull size: Tier A, CONDITIONAL on lod_override (Jay 2026-10-15: "Range & Cull_size are now TIER A aswell, but conditional.
// We will be looking for another way to fix things at a later point in time"). 1700 = his disk = the engine's own range; the cull
// size 0 is the fix itself (INV-578). -1 = leave alone.
float LodObjectsRange = 1700.0f;
float LodMeshCullSize = 0.0f;
float LodMeshCullDist = -1.0f;     // -1 = leave alone
float LodFxRange = -1.0f;          // -1 = leave alone
// [DEP: diag_no_rast_occlusion] required-for=the raster-occlusion A/B (DiagNoRastOcclusion) - sets the scene view's own ForceViewNoRastOcclusion byte; renders more, diagnostic only.  tree=INERT
bool DiagNoRastOcclusion = false;
// [DEP: diag_jitter_vr_exec] required-for=the VR texture / decal / shadow flashing FIX (DiagJitterVrExec, INV-562) - NOPs the COculusHelper test in CmdPPFX_SMAAScreenJitter::Execute (engine+0x75E010) so VR Execute writes what the job snapshot writes (INV-561 (f)-(j)). Jay-verified 2026-10-14 "That's done it. That's the fix." - Tier A, LOCKED ON (S_BOOL_LOCKED: not drawn, file value ignored; the worker re-applies the bytes if anything reverts them). Off = the flashing returns in VR with engine jitter on.  tree=LOADBEARING
bool DiagJitterVrExec = true;
// [DEP: diag_clip_far] required-for=[CLIPFAR] 1 Hz read of the scene camera's clip near/far + centre/edge reach (DiagClipFar), INV-368.  tree=PROBE-ONLY
bool DiagClipFar = false;
// [DEP: clip_far_scale] required-for=the clip-far A/B (ClipFarScale) - multiplies the scene camera's clip far via IBaseCamera::SetClipFar; 1.0 = untouched.  tree=INERT
float ClipFarScale = 1.0f;
// [DEP: vr_near_clip_cm] required-for=the VR first-person NEAR PLANE (VrNearClipCm), INV-549 - Jay 2026-10-12 "Do it" (option 1: fix the
//      see-through at its source so the view offsets can go). The game uses 9.76 cm in VR and 5 cm in flat (gamedll sub_180C017A0 by a flag);
//      this writes the one float both VR readers load (gamedll +1926BEC, readers +C017C0 / +343295, verified first). 9.76 = the game's own
//      value, nothing written. Live (a 250 ms worker). tree=INERT at 9.76. HEADSET for the effect. Was DEFAULT 9.76 - Tier C until Jay verifies.
//      SHIPPED 5 since 2026-10-12 - Tier A (INV-550; Jay: "No see-through at all" / "Set the new values to ship"). tree=LOADBEARING at 5:
//      cam_eye_up_mm 0 / cam_eye_forward 1.15 rely on it to keep the chest and shoulders solid.
float VrNearClipCm = 5.0f;
// [DEP: afw_enable] required-for=D-184's ONE user switch "Alternate Frame Warp" (VR Comfort, AfwEnable; Jay 2026-10-08: default enabled). Drives afw_alt_eye and afw_rebuild (S_BOOL_FOLLOWS - hidden, file value never read); every AFW fix under them is LOCKED on. Off = both off = no AFW at all.  tree=LOADBEARING
bool AfwEnable = true;

// Data3 check (Jay 2026-10-18, "Md5 + refuse AFW if wrong"): AFW runs only on this build's Data3.pak. A stock / other pak has no
// keep-RIFT, so the held eye is cleared to black every other frame (INV-375; a tester's, docs/BETA-FINDINGS-2026-10-18.md §2). The
// bootstrap hashes the whole pak at its early init (pakgen.cpp CheckData3Md5) and exports the verdict. Fails CLOSED: not
// checked, unreadable, not ours, or no export = AFW held off, named. Final verdicts (ours / not ours / unreadable) are cached;
// "not checked yet" is re-asked at most every 2 s. Called from SyncFollowers (config load + every overlay edit) - not per frame.
bool AfwData3Allowed()
{
    typedef int (*PFN_State)();
    static PFN_State s_fn = nullptr;
    static bool s_looked = false;
    static int s_final = 0, s_said = 99;
    static ULONGLONG s_next = 0;
    if (s_final != 0) { return s_final == 1; }
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) { return false; }
    s_next = now + 2000;
    if (!s_looked) {
        if (HMODULE b = GetModuleHandleA("xinput1_3.dll")) {
            s_looked = true;
            s_fn = reinterpret_cast<PFN_State>(GetProcAddress(b, "DlvrData3State"));
        }
    }
    const int st = s_fn ? s_fn() : (s_looked ? -9 : 0);
    if (st == 1 || st == -1 || st == -2 || st == -9) { s_final = st; }
    if (st != s_said) {
        s_said = st;
        // [PROBE: AFWDATA3] gate=afw_enable (read at config load + overlay edits)  site=FLAT  log-only=no
        LOG("[AFWDATA3] Data3 verdict from the bootstrap: %d - %s", st,
            st == 1   ? "this build's Data3.pak - AFW may run (afw_enable decides)"
            : st == -1 ? "*** NOT this build's Data3.pak *** - AFW is REFUSED this launch whatever afw_enable says (the held eye "
                         "would be cleared to black). Reinstall with the setup program."
            : st == -2 ? "*** Data3.pak UNREADABLE *** - no verdict, AFW is REFUSED this launch"
            : st == -9 ? "*** the bootstrap does not export DlvrData3State *** (an older xinput1_3.dll) - AFW is REFUSED"
                       : "not checked yet (bootstrap not loaded or pakgen not run) - AFW held off until it is");
    }
    return st == 1;
}

// [DEP: afw_alt_eye] required-for=the AFW alternate-eye A/B (AfwAltEye) - skips ONE eye's Scene replay (ExecLayer 21) per frame, alternating, INV-370; no warp, the skipped eye keeps its previous image. VR only since 2026-10-07 (U-51): held off while no eye-1 Scene replay is seen (flatscreen). Since 2026-10-08 FOLLOWS afw_enable (hidden; true here = afw_enable's default).  tree=LOADBEARING
bool AfwAltEye = true;
// [DEP: afw_held_eye_pose] required-for=AFW head-rotation correction (AfwHeldEyePose) - under afw_alt_eye, declare the HELD eye with the orientation it was last rendered with (from the previous submit) instead of this frame's, so the compositor rotates it to the current head; only acts while afw_alt_eye AND declare_rendered_orientation are on.  tree=INERT
bool AfwHeldEyePose = true;
// (diag_afw_cam / diag_afw_depth - finished AFW probes: REMOVED 2026-10-07, audit section 4 group 1. The matrix
//  push and the depth copy they shared with afw_rebuild stay, now on afw_rebuild alone.)
// [DEP: afw_rebuild] required-for=AFW step 4b, the held-eye REBUILD (world only): at each submit under afw_alt_eye, when this image's GPU camera equals its captured camera (the [AFWRB] gate) and the previous image was the other eye, the bootstrap (DlvrAfwRebuild) redraws the held eye's half of the eye image from its own last image + depth and the fresh eye's, at the fresh image's camera; the held eye is then declared with the fresh orientation. Any refusal = today's held eye. Also makes the bootstrap copy the depth per frame (dlvr_AfwDepthWanted bit 1) and defers the eye swapchain's release from commit to submit. Hands, weapon and HUD are NOT handled (plan steps 5-6). Since 2026-10-08 FOLLOWS afw_enable (hidden; true here = afw_enable's default).  tree=LOADBEARING
bool AfwRebuild = true;
// [DEP: afw_rebuild_tint] required-for=the rebuild's debug tint (guide §14.2): colours each held-eye pixel by the rule that drew it - untinted = the held eye's own world, blue = fresh world, green = fresh because the held point was stale, orange = drawn through a rescued edge, red = held fallback, cyan = near miss, magenta = fill. Only acts with afw_rebuild.  tree=PROBE-ONLY
bool AfwRebuildTint = false;
// [DEP: diag_afw_rebuild_check] required-for=[AFWRB] CHECK - is the rebuilt world RIGHT? Passed to the bootstrap as DlvrAfwRebuild flag bit 1: on every 4th rebuild it predicts the FRESH eye's image (which really was rendered) from the same eye's image two frames back and from the other eye's image one frame back, carried by depth + the game matrices to the fresh image's real matrix, and logs the mean miss against the real image beside the unmoved baseline, binned by camera motion. Needs afw_rebuild + afw_alt_eye. Log-only; costs two more seed passes on check frames.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwRebuildCheck = false;
#endif
// [DEP: afw_rb_far_rule] required-for=the rebuild's FAR RULE (DlvrAfwRebuild flag bit 2): the nearer-than and stale depth comparisons clamp both depths at 250 m, so the sky's flat far plane (which faces each image's own camera) cannot read nearer / farther after a turn - the candidate cause of the blue sky flash (INV-387). Only acts with afw_rebuild; the mapping is not clamped.  tree=EXPERIMENT
bool AfwRbFarRule = true;
// [DEP: afw_rb_stale] required-for=the rebuild's STALE TEST (DlvrAfwRebuild flag bit 3, guide 7.3): a held-eye point the fresh eye sees PAST (nearest fresh depth over 5x3 depth texels farther by 1.5 % + 1 cm) has moved and gives way to the fresh eye - moving objects stop trailing in the held eye. No foreground mask: hands and weapon meet it as world. Only acts with afw_rebuild.  tree=EXPERIMENT
bool AfwRbStale = true;
// [DEP: afw_rb_edge_rescue] required-for=the rebuild's EDGE RESCUE (DlvrAfwRebuild flag bit 4, guide 6 Pass 3 step 2): where a seed was a stretched sheet or the refined depth slid off the near side of an edge, refine again from the nearest neighbouring seed. Only acts with afw_rebuild.  tree=EXPERIMENT
bool AfwRbEdgeRescue = true;
// [DEP: diag_afw_rebuild_rules] required-for=[AFWRB] RULES - the share of the held eye each ladder rule draws (DlvrAfwRebuild flag bit 5): every 7th rebuild the bootstrap runs the ladder at 256x256 points into two class images and reads their 1x1 mips back, binned camera STILL / MOVING. Needs afw_rebuild + afw_alt_eye. Log-only.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwRebuildRules = false;
#endif
// [DEP: diag_afw_fg_motion] required-for=[AFWMOT] - AFW item 2 stage 2b-0 (INV-406 / 407 / 415): does the engine's motion buffer (1_RG16F_MV) carry the player's body / weapon motion under AFW, and does following it one tick back land on the held image's hand. The bootstrap copies the motion buffer beside each depth copy (dlvr_AfwDepthWanted bit 2) and the RULE COUNTER's pass (DlvrAfwRebuild flag bit 7) adds, in the FRESH image: share of foreground texels moving, mean motion, the same for non-foreground, and of the moving foreground texels the share that lands on the held image's foreground with the motion / without it / only with it / only without it / only with the opposite sign. Needs diag_afw_rebuild_rules (the pass it rides), afw_rb_fg_mask (both masks), afw_rebuild + afw_alt_eye. Log-only: nothing drawn changes. Costs one full motion-texture GPU copy per frame while on.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwFgMotion = false;
#endif
// [DEP: diag_afw_heldmap] required-for=[AFWHM] - AFW item 2 stage 2b-1 (INV-418): the bootstrap redraws each skinned body draw the AFW mask matches (VS hash s2910 / s3204 / s1938 - the bootstrap's shader hashing, unconditional since 2026-10-06) once more with its own VS: the PREVIOUS palette rows (the held image's pose) through the HELD image's matrix into its own RGBA32F target + D32, each texel carrying where that point is NOW (the current rows, fresh camera). dlvr_AfwDepthWanted bit 3 + DlvrAfwRebuild flag bit 8; the rule counter's pass compares that map's depth with the held image's own mask depth. Needs afw_rb_fg_mask, diag_afw_rebuild_rules, afw_rebuild + afw_alt_eye. Log-only: nothing drawn on screen changes. Costs one extra draw per matched skinned draw while on.  tree=PROBE-ONLY
bool DiagAfwHeldMap = false;   // a real setting in BETA too since 2026-10-29 (it gates the [AFWVEH] buggy probe, bit 6)
// [DEP: afw_rb_fg_move] required-for=AFW item 2 stage 2b (INV-419): the rebuild carries each HELD-image body texel (held mask set AND the held-pose map covers it) from the map's CURRENT position instead of as static world - inside afwrb mapH, so the seed mesh, the refine and the stale test all see the hand where it is now. DlvrAfwRebuild flag bit 9; also turns on the held-pose redraw (dlvr_AfwDepthWanted bit 3; the bootstrap's shader hashing it needs is unconditional since 2026-10-06 - no marker). Needs afw_rb_fg_mask, afw_rebuild + afw_alt_eye. Changes the held eye's picture. Not the weapon (not redrawn). DEFAULT ON since 2026-10-06 (Tier A, INV-432 - Jay: 'Block B is a significant improvement'; inert while afw_rebuild / afw_alt_eye are off).  tree=PROBE-ONLY
bool AfwRbFgMove = true;
// [DEP: afw_rb_fg_move_weapon] required-for=INV-464 - the HELD WEAPON in afw_rb_fg_move's held-pose map. The weapon's VS (s4832, hash 02512a50e871248a) takes its world matrix from the vertex stream (TEXCOORD4..6) and has no previous pose, so the bootstrap SAVES those rows on the GPU at every matched weapon draw (one column per mask range and instance) and, at the next image's draw, redraws the weapon with the SAVED rows through the HELD image's matrix and the current rows through the current camera - the rigid form of INV-419's map. dlvr_AfwDepthWanted bit 4 + DlvrAfwRebuild flag bit 18 (log only: mapH reads the map either way). Needs afw_rb_fg_move, afw_rb_fg_mask, afw_rebuild + afw_alt_eye. Changes the held eye's picture where the weapon is. Covers (INV-465 / 466) rigid form A s4832 / s5552 / s0228, form B s4236 / s1221, skinned s4870 / s2118 / s4917 / s2543. DEFAULT ON since 2026-10-06 (Tier A, INV-466 - Jay: 'The trailing was less in Block 6'; on his disk; inert while afw_rebuild / afw_alt_eye are off). Live.  tree=EXPERIMENT
bool AfwRbFgMoveWeapon = true;
// [DEP: afw_rb_fg_edge_ring] required-for=INV-465 (1) - the stick-turn hand CONTOUR: a held-image texel that is not the body / weapon but touches one (8 neighbours) that afw_rb_fg_move carried more than the sample tolerance from its static place is the hand's anti-aliased edge colour under a background depth texel; carried as world it draws the hand's OLD outline. With this on the held image gives no candidate there (afwrb ringH -> eH = 1e9) and the other eye or the fill supplies the pixel. DlvrAfwRebuild flag bit 19 -> cbuffer rul3.x. Needs afw_rb_fg_move (mot.w), afw_rb_fg_mask (sw.z), afw_rebuild + afw_alt_eye. Hands still relative to the world -> nothing moved -> unchanged. DEFAULT ON since 2026-10-06 (Tier A, INV-466 - Jay: 'looked better in block 2 & 3', then 'Set edge_ring and fill_near to A tier'; inert while afw_rebuild / afw_alt_eye / afw_rb_fg_move are off). Live.  tree=EXPERIMENT
bool AfwRbFgEdgeRing = true;
// [DEP: afw_rb_fill_near] required-for=INV-465 (2) - the stick-turn FILL SMEAR: the fill takes, per side along the row, the NEAREST covered seed, drops a side whose nearest seed is the body / weapon (or the hand's old edge), and extends the FARTHER of the remaining sides - the background next to the hole, instead of the farthest seed up to 128 texels away. DlvrAfwRebuild flag bit 20 -> cbuffer rul3.y. Needs afw_rebuild + afw_alt_eye; the hand test needs afw_rb_fg_mask. Changes every fill pixel (head turns too). DEFAULT ON since 2026-10-06 (Tier A, INV-466 - Jay: 'looked better in block 2 & 3', then 'Set edge_ring and fill_near to A tier'; inert while afw_rebuild / afw_alt_eye are off). Known residue: a row 'barcode' on patterned surfaces (INV-466 (2)). Live.  tree=EXPERIMENT
bool AfwRbFillNear = true;
// [DEP: afw_rb_seed_indexed] required-for=A1a (PERF-ENGINE-RESEARCH-2026-10-16 §1d, Jay "A1a"): flag bit 21 to the bootstrap's DlvrAfwRebuild - the AFW rebuild's two seed-mesh draws (11.6 M vertices each at DLAA) are drawn INDEXED from a static index buffer (afwrb::EnsureIb, built once per grid size, 46.4 MB at 1344x1440) through VSMeshIdx, which computes each grid point exactly as VSMesh does - same triangles, same order, same vertex values, so the image is meant to be IDENTICAL; only the vertex shading (6x per point -> ~1x per row) changes. Off = the plain Draw, unchanged. Read by the rebuild only. VERIFIED 2026-10-16 (backups/20261016_a1a_run1, Jay: "Couldn't see a change"; rebuild GPU 1.221-1.318 -> 1.009-1.060 ms, seed rows 0.70 -> 0.50 ms/frame) -> TIER A, SHIPPED ON + LOCKED under afw_enable (Jay: "Yes, ship it on"; D-184).  tree=EXPERIMENT
bool AfwRbSeedIndexed = true;
// [DEP: afw_rb_fg_mask_engine] required-for=U-54 / INV-486 - the Volkatronix barrel seen THROUGH the sight glass, broken / gone in every rebuilt right-eye frame. Two halves, one switch: (1) the bootstrap's mask redraw keeps the ENGINE's pixel shader bound (the alpha-tested sight glass discards there as in the scene, so the barrel behind it is masked - INV-479) and does not draw a matched draw whose engine depth state writes no depth; (2) a matched draw whose engine PS DISCARDS (vsclass::HasDiscard, flagged at CreatePixelShader; offline 3790 / 3790 PS agree with fxc's text) is left OUT of the held-pose map, so the glass no longer sits solid in front of the barrel there (INV-486 static). dlvr_AfwDepthWanted bit 5 -> Redraw / EngineNote ([AFWFG] MASK ENGINE lines) / HmDraw + RwDraw (b) ([AFWHM] GLASS lines). Needs afw_rb_fg_mask, afw_alt_eye (and the rebuild + afw_rb_fg_move to matter). Changes the mask and the held-pose map -> every foreground rule, the move and the edge ring read them. Restored 2026-10-08 from D-197's revert (not ruled out) + half (2) new. DEFAULT ON since 2026-10-08 (Tier A, INV-487 - Jay: 'That was perfect'; capture 104113 right-eye rebuilt barrel fg 0.982, was 0.562; inert while afw_rb_fg_mask / afw_alt_eye are off). Live.  tree=EXPERIMENT
bool AfwRbFgMaskEngine = true;
// [DEP: afw_mv_object_scale] required-for=INV-453 option 1 - the motion vectors DLSS / FSR are told under AFW: the bootstrap's MV pass (kMvFixShader) multiplies the ENGINE's object vector (one tick, INV-406 / 407) by this value while the layer's AFW eye stamp is set (dlvr_AfwRenderedEye >= 0), so an NPC's vector spans the eye's TWO-tick history like the camera term already does (INV-444). 1.0 = the old sum, unchanged; AFW off = always 1. Read by the bootstrap through dlvr_AfwMvObjectScale (RunMvFix). Covers DLSS and FSR, both AA paths. DEFAULT 2.0 since 2026-10-06 (Tier A, INV-454 - Jay: '2.0 is much better'; inert while afw_alt_eye is off). Live.  tree=EXPERIMENT
float AfwMvObjectScale = 2.0f;
// (afw_rb_nearmiss_world / afw_rb_fill_world (D-192), dlss_tell_frame_delta (D-193), afw_rb_bicubic (D-194): REMOVED 2026-10-07,
// audit section 2 - each ruled out in the headset; their code, settings and DlvrAfwRebuild bits 12-14 are gone.)
// [DEP: afw_rb_edge_hold] required-for=INV-461, the rebuild-made edge shimmer: at outlines the "fresh (other eye) is nearer" choice flips between rebuilds of the same eye (captures: 2.75-3.16 % of the plant box, own <-> blue). With this on, before that choice draws the other eye, compose checks the held sample's 8 neighbouring depth texels (mapH): if one maps to the fresh depth within the nearer tolerance, the held image has that surface right there and the held eye's own pixel is kept. DlvrAfwRebuild flag bit 15 -> cbuffer rul2.w. Needs afw_rebuild + afw_alt_eye; no mask needed. Changes the held eye's picture at outlines. DEFAULT ON since 2026-10-06 (Tier A, INV-462 - Jay: 'It's definitely less with that turned on.'; inert while afw_rebuild / afw_alt_eye are off). Live.  tree=EXPERIMENT
bool AfwRbEdgeHold = true;
// [DEP: afw_rb_edge_hold_radius] required-for=INV-462 refinement: how far (in depth texels) the edge hold searches the held image for the fresh surface's depth - 1 = the 8 neighbours (the version Jay verified), 2 = 24, 3 = 48. DlvrAfwRebuild flag bits 16-17 -> rul2.w (the radius; 0 = the hold off). Only acts with afw_rb_edge_hold. Default 2 since 2026-10-06 (Tier D tuning = Jay's disk; INV-463: 'Set it to 2 since it has less on the percentage' - the captures' lowest flip share). Live.  tree=EXPERIMENT
int AfwRbEdgeHoldRadius = 2;
// [DEP: diag_afw_trail_src] required-for=[AFWTRAIL] - where the pixels of the trail that stays with the hands moved come from (INV-421 / 423): in the rule counter's pass (DlvrAfwRebuild flag bit 10) the ladder records its source texel, and o3 / o4 count, for the fresh class (blue), the fill (purple) and the near miss (cyan), the share whose source texel lies in the hand's silhouette BAND (not body / weapon, within 2 depth texels of it, in that source image). Needs diag_afw_rebuild_rules, afw_rb_fg_mask (both masks), afw_rebuild + afw_alt_eye; never with diag_afw_fg_motion (it owns those targets). Log-only.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwTrailSrc = false;
#endif
// [DEP: diag_afw_capture] required-for=[AFWCAP] - frame captures of the rebuild (INV-425; the AFW guide §14.3): on this switch's rising edge the bootstrap arms, and 5 s later writes 4 consecutive rebuilds to <game>\dlvr_capture as PNG (held / fresh source, rebuilt, rebuilt with the class tint, held / fresh foreground) + a text header. DlvrAfwRebuild flag bit 11. Needs afw_rebuild + afw_alt_eye (the masks need afw_rb_fg_mask). Log-only towards the picture; each captured frame STALLS (blocking readback). Untick and tick for another set.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwCapture = false;
#endif
// (diag_swap_order - the [SWAPORD] ring: REMOVED 2026-10-07, audit section 4 group 1; U-52 closed.)
// [DEP: submit_drawn_image] required-for=U-52 / INV-429 layer fix (ovr2xr.cpp SdHandOut / SdRelease, the eye loop's release): the eye swapchain holds up to two images and each ovr_SubmitFrame releases the OLDEST - the image the frame being replayed drew into (the renderer records one hand-out ahead of its replay) - instead of the newest, whose content was the composite of two frames earlier. Applies to all VR, AFW on or off. Switched only at a submit (live). VERIFIED BY JAY 2026-10-05 21:15-21:19 (build 7a59e640, 90 Hz): "Submit drawn eye was MUCH BETTER that time. Still hiccups in block one, but couldn't notice a single one in block 2. Block 3 was ghosted/doubled on everything, block 4 fixed it." -> Tier A, SHIPS ON (release framework: verified + on his disk). LOADBEARING for the displayed image (spine S-52).  tree=LOADBEARING
bool SubmitDrawnImage = true;
// [DEP: diag_hiccup] required-for=[HICCUP] (ovr2xr.cpp HiccupFrame): per submitted frame, events E (xrEndFrame > 1.5 periods), G (submit gap > 1.5 periods), S (predicted display time step >= 1.5 periods), J (view 0 declared-vs-located jump > 0.5 deg), each with the head yaw rate; every event counted, lines capped at 10/s, 5 s summary split turning / still. Log-only. COMPILED DEFAULT ON (Jay 2026-10-05: the reviewer package must log it without touching their ovr2xr.txt - a key their file lacks takes this default). Diagnostic, not a fix: its shipped default is Jay's call (release framework D).  tree=PROBE-ONLY
// COMPILED DEFAULT OFF since 2026-10-07, disk 0 (Jay: "Turn off hiccup for both" - 2,661 lines, 26% of the layer log in an
// 8.5 min run; its J event fires on any fast turn, U-52 (d)).
bool DiagHiccup = false;
// [DEP: diag_afw_fg_probe] required-for=[AFWFG] AFW item 2 stage 0 (the foreground mask, INV-395): which draws are the player's arms. boneblend (afwfgprobe::Tick, in BoneBlendTick - needs the rig armed and a captured player model) hides each player_* / watch mesh element for 2 s in turn with the engine's HideElement / UnhideElement; the bootstrap (afwfg::, dlvr_AfwFgProbePhase) counts every keyed draw per phase and logs the keys that vanish, how often the draw hooks were found overwritten (INV-15), and whether the GBuffer-pass view-projection writes hold one projection (needs dlss_cbsnap.txt + dlss_mvfix.txt). NOT log-only: the body parts blink while on. Does not need afw_alt_eye.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagAfwFgProbe = false;
#endif
// (diag_afw_fg_ranges / diag_afw_prevbuf - finished AFW probes, INV-398 / INV-407: REMOVED 2026-10-07.)
// [DEP: afw_rb_fg_mask] required-for=AFW item 2 stage 1, THE FOREGROUND MASK (INV-396..399): boneblend (afwfgprobe::MaskTick - needs the rig armed and a captured player model) publishes every LOD-0 draw range of the player's mesh elements and of both held items (dlvr_AfwFgMaskRanges); the bootstrap (afwmask::) patches rd3d11's own DrawIndexed / DrawIndexedInstanced call sites (INV-397), and redraws each draw whose (index count, start, base vertex) is in the table - inside the GBuffer bind only - depth-only into its own D32 texture, one per image beside the depth copy. A texel is foreground where that mask equals the scene depth. LOADBEARING since 2026-10-07 (Tier B, compiled ON, Jay): afw_rb_fg_move, afw_rb_fg_move_weapon and afw_rb_fg_edge_ring (all Tier A) read it - without it they do nothing. With afw_rebuild_tint the rebuilt eye's foreground pixels are tinted yellow. Needs afw_alt_eye and the depth copy (afw_rebuild or diag_afw_depth).  tree=LOADBEARING
bool AfwRbFgMask = true;   // Tier B (2026-10-07, Jay): loadbearing - afw_rb_fg_move / _weapon / edge_ring (Tier A) read it
// (afw_rb_fg_rules - AFW item 2 stage 2's foreground rules, failed in the headset D-185, compiled 0: REMOVED 2026-10-08 (Jay: "Remove it"). Its only reader was DlvrAfwRebuild flag bit 6 -> the shader's sw.w, now never set.)
// Texture-streaming lowest-priority distance, in METRES. r_device.scr ships
// 100. Raise it so distant assets keep real streaming priority in VR.
float StreamPriorityDistanceM = 100.0f;
// [DEP: stream_budget_mb] required-for=F1 texture-streaming budget (INV-580, docs/PERF-ENGINE-RESEARCH-2026-10-16.md): ApplyStreamingBudgetOnce sends it with SRendererAPI slot 143 case 5 and stores it on the renderer with CRenderer slot 20 (the -texStreamMemMB store, so the engine's video-settings apply keeps it). -1 = AUTO (SHIPPED default since 2026-11-13, Jay: "we're putting it in for end users now"), sized from DXGI QueryVideoMemoryInfo (streaming used + local budget - process usage - the reserve, never below the engine's own value); 0 = send nothing; > 0 = that many MB (2048 = the fixed value shipped 2026-09-05 .. 2026-11-13; a file holding exactly 2048 is moved to -1 at load, LoadLayerConfig). Live: re-sent on change. Both slots RVA-checked before the first call.  tree=EXPERIMENT
int StreamBudgetMB = -1;     // compiled = AUTO (Jay 2026-11-13); docs/traces/20261113_1603_streambudget_auto_reserve.md
// [DEP: stream_budget_reserve_mb] required-for=stream_budget_mb=-1 (AUTO) only: the VRAM AUTO leaves free beyond what the process already uses (DLSS / AFW / our buffers allocated later, the VD encoder, other apps). -1 = AUTO (SHIPPED default, Jay 2026-11-13: "Reserve needs to be auto, not every card is 16gb") = kAutoReservePct of the card's DXGI local budget; >= 0 = that many MB (3072 = the fixed default before 2026-11-13; a file holding exactly 3072 is moved to -1 at load). Inert at any other stream_budget_mb.  tree=EXPERIMENT
int StreamBudgetReserveMB = -1;
// [DEP: diag_stream_budget] required-for=the STREAMBUDGET probe (LOG-ONLY): every 30 s the engine's streaming budget vs used (rd3d11+0x784B8 / +0x7BD18), how often used changed since the last line (stream-in / evict churn), and DXGI's local budget / process usage; the first line unconditionally. On the scePad poll (one 8-byte read per poll, one DXGI query per 30 s). ALSO installs (once, while on) two PASS-THROUGH detours for the STREAMAPPLY timer: rd3d11 +43320 (the per-frame streaming update) and +42C30 (one stream-in texture creation) - QPC around each, nothing changed, nothing logged on the render thread. Read by nothing else.  tree=PROBE-ONLY
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in camhook.h (docs/BETA_CENSUS.md)
bool DiagStreamBudget = false;
#endif
// [DEP: shadow_cascade_stagger] required-for=S1 SHADOW CASCADE STAGGER (INV-584 / INV-585, docs/PERF-ENGINE-RESEARCH-2026-10-16.md §1c): a pass-through detour on engine +2B7070 (renders ONE sun cascade, sole caller +2B9680). 0 or 1 = stock (every cascade every frame). N >= 2 = the cascades from shadow_stagger_first up refresh in turn, cascade i when (frame + i) % N == 0; a skipped cascade keeps its texture AND its HSM matrix (EvalSH_HSM_XFORM_* read the HSM's own slot-9 store, written only inside +2B7070), so they stay paired. Forced full refresh when the HSM pointer or the cascade count changes. REFUSED (stock) in HSM atlas mode (index 0 clears the whole atlas), with an active NvidiaEffects(1,...) in video.scr (PCSS records per cascade), or when video.scr cannot be read. Compiled 3 since 2026-11-24 (Jay, RUN SF3: "Done, everything seemed fine" -> "Yes", trace 20261124_1102; ~0.22-0.32 ms/frame over 2). Was Tier A, compiled 2 (2026-10-16, Jay: "Same in both eyes" on 6c68bdb9; "Ship on" after the 2e8a270e run - LIVE, skips balanced, render-queue pool flat at 22).  tree=EXPERIMENT
int ShadowCascadeStagger = 3;
// [DEP: texture_aniso] required-for=Jay's floor-blur fix (INV-684, RUN TX1: "That looks better"): the anisotropy level the BOOTSTRAP applies to the engine's mipped samplers (dlssflat.cpp reads this key from ovr2xr.txt as text before the layer loads). The layer only holds and saves the value; no layer code reads it except the menu. 0 / 1 = the game's own filtering, 2-16 = that level. Restart to apply (samplers are created at load). Tier A, compiled 16.  tree=EXPERIMENT
int TextureAniso = 16;
// [DEP: shadow_stagger_first] required-for=shadow_cascade_stagger >= 2 only: the first cascade index that is staggered (cascades below it render every frame). Default 1 = only cascade 0 every frame (Jay 2026-11-23, RUN SF2: "It all looked completely fine"; was 2). Inert at stagger 0 / 1.  tree=EXPERIMENT
int ShadowStaggerFirst = 1;
// [DEP: lens_mask] required-for=F2 LENS MASK (docs/PERF-ENGINE-RESEARCH-2026-10-16.md §1e): the bootstrap's ClearDepthStencilView hook writes depth 1.0 (the near plane, reversed-Z) + stencil 255 into every texel of the scene ZBUFFER the runtime cannot display (outside its per-eye FOV + lens_mask_margin_deg, after the declared pitch), re-stamps the stencil after each stencil clear, and sets those texels to depth 0 (far) at the tonemap draw before AFW's depth copy and the AA. The geometry comes from dlvr_LensMask (ovr2xr.cpp), stored per engine eye at each submit. VR only. Tier C, compiled 0.  tree=EXPERIMENT
bool LensMask = false;
// [DEP: lens_mask_margin_deg] required-for=lens_mask only: degrees added to each side of the runtime's per-eye FOV before masking - the room the runtime's reprojection to the newest head pose needs (more for AFW's held eye). Not measured; the tint check sets it. Inert with lens_mask off.  tree=EXPERIMENT
float LensMaskMarginDeg = 3.0f;
// [DEP: lens_mask_tint] required-for=lens_mask only: paints the masked texels bright red in the HDR scene colour at the tonemap draw, so any masked texel the headset shows is unmistakable (the margin / eye-label check). A visual diagnostic. Inert with lens_mask off.  tree=PROBE-ONLY
bool LensMaskTint = false;
// [DEP: dlss_lens_crop] required-for=the DLSS / FSR LENS CROP (docs/superpowers/specs/2026-11-11-dlss-lens-crop-design.md): the bootstrap's DLSS (DLAA + upscale) and FSR 3 evaluations reconstruct only the ROWS of each eye the runtime can display (+ dlss_lens_crop_margin_deg, after the declared pitch); the cropped top / bottom bands get a cheap fill. Geometry from dlvr_LensGeom (ovr2xr.cpp), the same per-eye record the lens mask uses - lens_mask itself is NOT needed and stays off. VR only, live. Tier C until Jay verifies, compiled 0.  tree=EXPERIMENT
bool DlssLensCrop = false;
// [DEP: dlss_lens_crop_margin_deg] required-for=dlss_lens_crop only: degrees added above and below the runtime's per-eye FOV before cropping - the room the compositor's turn to the newest head pose needs. Default 3 = Jay's lens_mask_margin_deg. Tier D. Inert with dlss_lens_crop off.  tree=EXPERIMENT
float DlssLensCropMarginDeg = 3.0f;
// [DEP: dlss_lens_crop_tint] required-for=dlss_lens_crop only: the crop's fill paints the cropped bands bright red instead of the cheap upscale, so a cropped texel the headset shows is unmistakable (the margin check). A visual diagnostic. Inert with dlss_lens_crop off.  tree=PROBE-ONLY
bool DlssLensCropTint = false;
// FIXED FOVEATED DLSS / FSR (docs/superpowers/specs/2026-11-11-foveated-dlss-design.md; plan docs/superpowers/plans/2026-11-11-foveated-dlss.md).
// Controls and defaults from CheekyFoveatedDLSS (github.com/ClarkCheekyKent/CheekyFoveatedDLSS, USAGE.md), Jay: "Use his defaults for
// now". The bootstrap reads all of them through dlvr_FovGeom (ovr2xr.cpp) - no other reader.
// [DEP: fov_dlss] required-for=FIXED FOVEATED DLSS / FSR: each eye's DLSS (DLAA + upscale) / FSR 3 evaluation becomes a full-quality CENTRE feature on a rectangle straight ahead of the eye (Jay 2026-11-11 "Straight ahead") + a cheaper PERIPHERY feature (DLAA, preset fov_periphery_preset) on a downsampled eye, blended by one composite pass. While on, dlss_lens_crop is ignored for that eval. Geometry + every fov_* value reach the bootstrap through dlvr_FovGeom. VR only, live. Tier C until Jay verifies, compiled 0.  tree=EXPERIMENT
bool FovDlss = false;
// [DEP: fov_width] required-for=fov_dlss only: the fovea's width as a fraction of the eye image (Cheeky "Fovea width", 0.55). Recreates the centre feature (debounced 300 ms). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovWidth = 0.55f;
// [DEP: fov_height] required-for=fov_dlss only: the fovea's height as a fraction of the eye image (Cheeky "Fovea height", 0.45). Recreates the centre feature (debounced 300 ms). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovHeight = 0.45f;
// [DEP: fov_height_offset] required-for=fov_dlss only: moves the fovea up (-) / down (+), both eyes alike, in Cheeky's unit (offset * (1 - height) / 2 of the image; "Height offset", 0). Recreates (debounced). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovHeightOffset = 0.0f;
// [DEP: fov_x_offset] required-for=fov_dlss only: OURS, not Cheeky's (Jay 2026-11-11 "Yes, add the horizontal slider"): moves the fovea toward the temple (+) / nose (-), mirrored per eye (label 0 = LEFT, INV-719), in Cheeky's offset unit (offset * (1 - width) / 2 of the image). 0 = straight ahead. Recreates (debounced). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovXOffset = 0.0f;
// [DEP: fov_roundness] required-for=fov_dlss only: the fovea's shape, rectangle (0) to ellipse (1) (Cheeky "Roundness", 0). Composite only - never recreates. Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovRoundness = 0.0f;
// [DEP: fov_transition] required-for=fov_dlss only: the feather of the fovea's edge (Cheeky "Transition width", 0.040). Composite only - never recreates. Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovTransition = 0.040f;
// [DEP: fov_supersample] required-for=fov_dlss only: the centre feature's output scale per dimension, 1-2 (Cheeky "Center supersampling", 1.00x); the composite area-downsamples it. Recreates (debounced). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovSupersample = 1.0f;
// [DEP: fov_periphery] required-for=fov_dlss only: the periphery DLSS / FSR pass (Cheeky "Peripheral DLSS", on). OFF = a plain bilinear stretch outside the fovea - a DEBUG COMPARISON only (it breaks Jay's anti-goal "shimmering or blur at the edges while turning"). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
bool FovPeriphery = true;
// [DEP: fov_periphery_preset] required-for=fov_dlss + fov_periphery only: the periphery DLAA feature's NGX preset id, 5 = E (Cheeky's default "Fastest"), 11 = K, 12 = L, 13 = M (nvsdk_ngx_defs.h); anything else is sent as 5 with a log line (dlvr_FovGeom). E is deprecated, not removed, in our 310.9.1: NGX falls back to K by itself and logs it (INV-719 (A)). Recreates (debounced). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
int FovPeripheryPreset = 5;
// [DEP: fov_periphery_scale] required-for=fov_dlss + fov_periphery only: the periphery's working size as a fraction of the render size per dimension, 0.20-1.00 (Cheeky "Periphery scale", 0.75). Recreates (debounced). Tier D. Inert with fov_dlss off.  tree=EXPERIMENT
float FovPeripheryScale = 0.75f;
// [DEP: fov_border] required-for=fov_dlss only: the composite paints a 5 px red line on the fovea's edge (Cheeky "Show 5 px red alignment border") - where the fovea sits, per eye. A visual diagnostic. Composite only. Inert with fov_dlss off.  tree=PROBE-ONLY
bool FovBorder = false;

namespace {




// ------------------------------------------------------------------ maths ---

struct Mat3 {
    float m[9];   // row-major: m[r*3+c]
};

void QuatMul(const float a[4], const float b[4], float o[4])
{
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
}

void QuatFromMat(const float m[9], float q[4])
{
    const float t = m[0] + m[4] + m[8];
    if (t > 0.0f) {
        float s = sqrtf(t + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[7] - m[5]) / s;
        q[1] = (m[2] - m[6]) / s;
        q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25f * s;
    }
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-6f) {
        q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n;
    }
}

void MatFromQuat(const float q[4], float m[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1.0f - 2.0f * (y * y + z * z);
    m[1] = 2.0f * (x * y - w * z);
    m[2] = 2.0f * (x * z + w * y);
    m[3] = 2.0f * (x * y + w * z);
    m[4] = 1.0f - 2.0f * (x * x + z * z);
    m[5] = 2.0f * (y * z - w * x);
    m[6] = 2.0f * (x * z - w * y);
    m[7] = 2.0f * (y * z + w * x);
    m[8] = 1.0f - 2.0f * (x * x + y * y);
}

// Keeps only the turn about the engine's up axis.
void YawOnly(const float q[4], int upAxis, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (upAxis == 1) {
        const float yaw = atan2f(2.0f * (q[3] * q[2] + q[0] * q[1]),
                                 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]));
        out[2] = sinf(yaw * 0.5f);
        out[3] = cosf(yaw * 0.5f);
        return;
    }
    const float yaw = atan2f(2.0f * (q[3] * q[1] + q[0] * q[2]),
                             1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    out[1] = sinf(yaw * 0.5f);
    out[3] = cosf(yaw * 0.5f);
}

// Rewrites the camera's orientation with pitch and roll removed, keeping its
// position and keeping the view matrix consistent with it.

// Diagnostic-only: is Look back's own button (Y / Triangle) physically held
// right now. Was previously also used to gate an un-mirror "fix" for det<0
// cameras - removed, see the det<0 branch's own comment below for why. Kept
// purely to timestamp diagnostic log lines against the button state while
// the real Look-back mechanism is still being searched for.
bool IsYHeld()
{
    bool pressed = false;
    dlvr::GetControllerButton(dlvr::kButtonY, pressed);
    return pressed;
}

// Diagnostic, 2026-09-03: the actual "am I in a vehicle" check the game's own
// scripting system uses, found the reliable way this time - not by scanning
// for and reading a live object's raw memory (which kept landing in
// unrelated allocator/driver heap churn for PlayerFitnessModule/
// VolatileChaseModule earlier this session), but by tracing
// CScriptConditionInVehicle's own RTTI vtable to its one class-specific
// virtual slot (gamedll+0x3FFE70, found via type-descriptor ->
// CompleteObjectLocator -> vtable, the reverse of the usual
// RttiClassNameOf direction) and hand-decoding it. It navigates a global
// singleton (gamedll+0x1C16348) through two more pointers with a null check
// at every step, then makes one virtual call and returns its bool result -
// genuinely a pure query, not something with side effects. Notably it never
// touches its own "this" argument at all, so it is safe to call directly
// with no real object of our own to pass in.
//
// Confirmed live, 2026-09-03: a real test session (walk around, get in a
// car, drive briefly, get out) showed exactly one clean false->true->false
// transition, landing on the user's own confirmed enter/exit moments - real
// game state, not guessed-at memory.
bool IsPlayerInVehicle()
{
    static uintptr_t base = 0;
    if (!base) {
        HMODULE h = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!h) {
            return false;
        }
        base = reinterpret_cast<uintptr_t>(h);
    }
    typedef unsigned char(__fastcall * PFN)(void*);
    PFN fn = reinterpret_cast<PFN>(base + 0x3FFE70);
    __try {
        return fn(nullptr) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// Sprint state, 2026-09-03 - quick fix, not the authoritative one.
//
// Dying Light's own sprint binding is confirmed (static string in
// gamedll_x64_rwdi.dll) to be `_ACTION_SPRINT_TOGGLE` - a press-to-toggle
// action, not hold-to-sprint. That means "is the left stick currently
// clicked in" (what this used to check directly) is the wrong question:
// a player can tap the stick once, take their thumb off it entirely, and
// the character keeps sprinting in-game - exactly the symptom reported.
//
// Real fix would read the game's own authoritative state - there is a
// genuine reflected field for it (`EMoveStyle::Sprint`, tied to
// `PlayerFitnessModule` per the same class the CriticalFitnessSprintLookAccMul
// strings point at) - but `PlayerFitnessModule` is the exact class that
// burned three separate live-verification attempts earlier this session
// (RTTI-scanned instances kept landing on stale/reused heap memory), so
// pinning its real field offset or a clean getter needs a proper live
// watch-writes session, deferred to next time.
//
// This is the stopgap: track sprint as a TOGGLE ourselves, flipped on each
// rising edge of the stick click - the same shape of signal the game's own
// toggle handler reacts to - rather than treating "currently held" as
// "currently sprinting." Good enough to fix the reported symptom; not
// immune to the game silently cancelling sprint on its own (stamina
// depletion, stopping movement, etc.) without our toggle knowing - this
// mod's state can drift from the real one until the authoritative check
// replaces it.
bool IsPlayerSprintingToggle()
{
    static bool toggledOn = false;
    static bool wasClicked = false;

    bool clicked = false;
    dlvr::GetControllerStickClick(dlvr::kLeftHand, clicked);

    if (clicked && !wasClicked) {
        toggledOn = !toggledOn;
    }
    wasClicked = clicked;
    return toggledOn;
}

// Diagnostic, 2026-09-04 - the real sprint-state chase, live-correlation
// approach. Static offset-guessing from PlayerDI (its own confirmed outer
// pointer, all 24 known base-class sub-object offsets in both directions,
// and a full 0x3000-byte field scan of its own data members) all failed to
// find a path to the modules[]-owning object - see CLAUDE.md for the full
// history. Traced one level further with Ghidra and found the real
// modules[]-owning object is reached through a GENERIC per-frame entity-
// update dispatch (FUN_180f09500 -> FUN_1805f1720 -> FUN_180179ff0),
// invoked identically for the player and every NPC alike - static tracing
// genuinely cannot distinguish "the player's" entity from any other by the
// nature of this architecture; every entity gets the same treatment.
//
// New approach: capture LIVE, at FUN_180179ff0's own entry - the function
// that actually loops modules[] starting at param_1+0xC10 (Ghidra typed
// its sole caller's own argument as IGSObject*, and passes it through
// unmodified, confirming +0xC10 applies directly here, no "outer"
// translation needed). This call fires every frame for every active
// entity, so a bounded capture naturally collects many different entities'
// own pointers - reusing watchwrite.cpp's existing execution-breakpoint
// tool as-is (no new VEH mechanism), which already deduplicates hits by
// (RCX, RDX) and keeps up to 64 distinct entries with a running hit count.
//
// To find WHICH of those many captured entities is the player specifically
// (the same problem that sank the earlier broad heap-scan approach in this
// same investigation), cross-reference by WORLD POSITION: each captured
// entity's own position is read via a direct call to the real
// IControlObject::GetWorldPosition (confirmed exported by
// engine_x64_rwdi.dll via dumpbin /exports - a plain function using the
// standard x64 hidden-return-pointer ABI for its vec3 return value, not a
// virtual dispatch, so a resolved function pointer is safe and sufficient
// on its own), then compared against the currently-tracked render
// camera's own position (rtticam.cpp's own scan+position-extraction,
// reused rather than duplicated) - whichever entity sits closest to the
// camera is, by construction, the player.
namespace weaponanchor {

constexpr size_t kModulesArrayOffset = 0xC10;
constexpr size_t kMovementModuleSlot = 52;
// $HEALTH_MODULE (i, 22) from data/ai/ai_modules.def. Published here rather than
// only inside damagewatch because THIS is where entities are enumerated, and the
// damage watch needs a damageable body - not specifically the player, whose
// identification is a separate and documented-fragile problem.
constexpr size_t kHealthModuleSlotWA = 22;
// $BODY_PARTS_HEALTH_MODULE (i, 23) - read from the SHIPPED EE Data0.pak's
// data/ai/ai_modules.def, not the 2020 dump. The per-limb variant, and the one
// this project's damage research names as what zombies use for melee.
constexpr size_t kBodyPartsHealthSlotWA = 23;
constexpr size_t kMoveStyleOff = 0x154;
constexpr size_t kCurrentMoveStyleOff = 0x184;
// FUN_180179ff0 - the function that actually loops modules[], confirmed via
// Ghidra (its sole caller, FUN_1805f1720, types its own argument as
// IGSObject* and passes it through unmodified).
constexpr uintptr_t kModulesLoopFnRva = 0x179ff0;
// >>> 8 -> 60, 2026-09-15. THIS COST A 9-MINUTE HEADSET RUN. <<<
// 8 attempts x (5000 retry + 2000 collect) is a ~68-SECOND budget measured from
// process start. The 15:47 run spent all of it on the load screen:
//     [15:47:42] attempt 1/8 - collected 0 distinct entity/module-loop call(s)
//     ... all eight zero ...
//     [15:48:36] weaponanchor: gave up after 8 attempts
// and the run then continued until 15:56 with the chain permanently dead - no
// health-module candidates, so damagewatch refused 102 times and never reached
// its poll or its trigger install.
//
// THE IDENTICAL BUG WAS FOUND AND FIXED IN THIS FILE ALREADY AND THIS PROBE
// NEVER GOT THE FIX - see visibilityprobe at :1305, "60 attempts, not 12: a
// real flatscreen run spent its entire 66-second budget still loading the
// save ... so the probe gave up before gameplay entities ever existed."
// visibilityprobe has covered five minutes since then. This covered 68 seconds.
// 60 x 7s is ~7 minutes, which survives a slow load AND a player who walks away.
//
// NOT TOUCHING g_started WHILE DOING THIS: :1358 records that clearing that
// latch on exit spawned 3,992 threads in two minutes. One scan per process.
constexpr int kMaxAttempts = 60;
// Raised by damagewatch when IT takes the single global WatchExec slot.
volatile LONG g_damageWatchArmed = 0;

// >>> RE-CAPTURE IS EVENT-DRIVEN, NOT TIMED. A TIMER TANKED THE FRAMERATE. <<<
// The 8-second timer added earlier the same day made the game unplayable on
// load, and the reason is what a capture COSTS: WatchExecStart arms a HARDWARE
// execution breakpoint on the modules loop across ~170 threads, and the collect
// window held it for 2 s. That is a ~25% duty cycle of debug-register traps on
// every thread in the process, forever. Before the change it ran 8 times and
// stopped; after it, never.
//
// So the trigger is now a REQUEST raised only when the current candidate list is
// demonstrably dead:
//   - the staleness gate fired (g_movModPtr is no longer a MovementModule), or
//   - a full detect window ended with NO m_HitCount movement AND no module change.
// That is typically one extra capture per fight instead of seven per minute, and
// the idle cost is back to zero.
volatile LONG g_recaptureReq = 0;
constexpr DWORD kRetryDelayMs = 5000;
// 750 ms, not 2000: the capture only needs the modules loop to tick once, and
// this window is exactly the interval during which every thread carries a
// hardware breakpoint. Shorter window, proportionally smaller cost.
constexpr DWORD kCollectMs = 750;

// Real bug, 2026-09-04: originally declared as (outVec3, this) - RCX=hidden
// return pointer, RDX=this - the "usual" x64 hidden-return-pointer ABI
// order. Every single live call faulted 13 bytes into the function, at
// what turned out (via Ghidra decompiling the real function, not guessing
// at the ABI further) to be the very first dereference of "this" - because
// the REAL order Ghidra's own decompiler recovered is (this, outVec3): RCX
// = this, RDX = the hidden return pointer, the reverse of what was
// assumed. Confirmed by the decompile itself: `IControlObject::
// GetWorldPosition(IControlObject *this)` with an `undefined4 *in_RDX`
// used only for the return-value writes - this is the same, correct
// convention that RCX=this, RDX=hidden-return-slot when they are shown in
// this specific order together, not the other way around.
typedef void(*GetWorldPositionFn)(void* controlObjectThis, float* outVec3);

// A real, clean test on the now-confirmed-correct MovementModule instance
// showed zero bytes changing across 4 genuine sprint-toggle edges -
// MovementModule itself does not hold the sprint state at all, a
// definitive negative (the object is known-good; nothing in it moves).
// This matches an earlier, separately-established lead from before this
// whole MovementModule investigation began: a genuine reflected field,
// `EMoveStyle::Sprint`, tied to `PlayerFitnessModule` - abandoned back then
// because heap-scanning for a live instance kept landing on stale/reused
// memory. `PlayerFitnessModule` is NOT part of the generic modules[] array
// (absent from data/ai/ai_modules.def entirely) - it's one of PlayerDI's
// own fixed-offset direct fields, stored at PlayerDI-outer+0x1348
// (confirmed via the same FUN_180be7000 decompile already used to rule out
// WeaponFireControllerRight's own back-pointer). Now that a confirmed-live
// player entity is finally in hand (via position correlation, not a
// guess), the stale-heap problem is sidestepped entirely: try +0x1348
// directly on the confirmed entity, and - since PlayerDI's own 25 known
// base-class sub-object offsets are the only established relationship
// between "the entity" and "PlayerDI's own outer pointer" - also try
// (entity +/- each known offset) + 0x1348, verifying each candidate via
// RttiClassNameOf == "PlayerFitnessModule" so a false positive can't slip
// through.
constexpr size_t kPlayerFitnessModuleOff = 0x1348;
constexpr size_t kPlayerDiOffsets[] = {
    0x18, 0x28, 0x30, 0x38, 0x58, 0x68, 0x70, 0x1a0, 0x1b8, 0x1c0,
    0x330, 0x340, 0x348, 0x350, 0x388, 0x8f0, 0x8f8, 0x900, 0x920,
    0x928, 0x930, 0x960, 0x968, 0x970,
};
constexpr int kNumPlayerDiOffsets =
    sizeof(kPlayerDiOffsets) / sizeof(kPlayerDiOffsets[0]);

volatile LONG g_started = 0;
volatile LONG g_havePlayerEntity = 0;
uintptr_t g_entityBase = 0;
// A damageable entity's HealthModule, published independently of whether the
// PLAYER was identified - the damage watch only needs a body whose health gets
// written, and player identification is a separate, documented-fragile problem.
uintptr_t     g_healthModulePtr = 0;
volatile LONG g_haveHealthModule = 0;
char          g_healthModuleClass[96] = {};

// ===== EVERY health module found, not just the nearest one (2026-09-11) =====
//
// MEASURED COST OF PUBLISHING ONLY ONE: two consecutive runs reported
// "0 distinct writer(s)". The 09-11 run settled why - the new window-close
// re-read showed the watched value FROZEN at 114.1406 across SEVEN windows and
// 2+ minutes while the user was both taking and dealing damage. The watch was
// working perfectly and pointed at an inert body: "closest entity carrying a
// HealthModule" picked some prop at publish time and never revisited it.
//
// Choosing a body by PROXIMITY is a guess. The fix is to stop choosing: publish
// every candidate, then let the damage watch poll them and pick the one whose
// value actually MOVES. That is a measurement, and it cannot be wrong the way a
// guess can.
//
// Slot 23 is included alongside 22 because ai_modules.def (verified against the
// SHIPPED EE Data0.pak, not the 2020 dump) gives
// $BODY_PARTS_HEALTH_MODULE (i, 23) - the per-limb variant, which is what
// zombies are documented to use for melee.
struct HealthCandidate {
    uintptr_t entity;
    uintptr_t module;
    unsigned  slot;
    char      cls[64];
};
constexpr int    kMaxHealthCandidates = 128;

// >>> THE SCAN'S START GATE. <<<
// Set by damagewatch's melee-input thunk the first time the player actually
// swings. ScanThread WAITS FOR THIS before it captures anything.
//
// WHY: the capture is a ONE-SHOT snapshot - ScanThread returns as soon as it
// latches, and g_healthCands is frozen for the rest of the process. The safe
// zone is full of damageable NPCs, so an ungated scan SUCCEEDS there, publishes
// a list of survivors who never take a scratch, exits, and every later window
// polls the wrong bodies. That negative looks exactly like a real one - which
// is the failure mode that has already burned three runs on this question.
// Gating on the first swing captures the entities that are around WHEN THE
// WEAPON IS BEING USED, which are the ones about to be hit.
volatile LONG    g_meleeUsed = 0;
HealthCandidate  g_healthCands[kMaxHealthCandidates] = {};
volatile LONG    g_healthCandCount = 0;
uintptr_t g_movModPtr = 0;
volatile LONG g_haveFitnessModule = 0;
uintptr_t g_fitnessModPtr = 0;

bool ReadPtr(uintptr_t addr, uintptr_t& out)
{
    bool ok = false;
    __try {
        out = *reinterpret_cast<volatile uintptr_t*>(addr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// >>> PUBLISH A BODY WE ACTUALLY HIT, 2026-09-18. THIS IS THE FIX FOR THE
//     FREEZE THAT HAS NOW COST FOUR RUNS. <<<
//
// THE FAILURE, measured 2026-09-18 12:56-13:01 and identical in shape to D-39:
//     12:58:01.994  entity list published and PARKED          (20 candidates)
//     12:58:06.249  damagewatch armed its exec watch -> re-capture OFF forever
//     12:58:15.124  *** STALE MODULE *** g_entityBase is DEAD and every entity
//                   captured alongside it is suspect
//     12:58:17.909  *** BONE HIT *** body#1 hit#1          <- 2.8 s TOO LATE
// 86 bone hits across 21 bodies were then polled against a list the probe had
// already declared dead, and could not rebuild because the re-capture needs
// the WatchExec slot that [BONEWATCH] now holds for the session.
//
// THE FIX IS NOT TO ARBITRATE THE SLOT. D-39 already rejected making
// [BONEWATCH] yield and re-arm - it costs missed hits every cycle and
// re-traps ~173 threads - and D-39's own resolution is the precedent worth
// copying: INVERTING A POINTER BEAT ARBITRATING A RESOURCE. The bone trap
// already holds the state vector in RCX, already sweeps it to its owning
// entity (INV-58), and already resolves that entity's health module and
// RTTI-checks it. So the body that was hit can PUBLISH ITSELF, at the instant
// it is hit, with no scan, no snapshot and no debug register.
//
// It is also strictly better than what the scan produced: a scanned candidate
// is a body that happened to be nearby, while one of these is BY CONSTRUCTION
// a body that just took a blow. That is the property three runs failed to get.
//
// THREADING: called from the damagewatch worker while it drains collected
// hits, never from inside the VEH handler - the RTTI resolve above it is an
// uncached both-DLL walk and has no business in an exception handler. The
// index is reserved with InterlockedIncrement so a concurrent scan-thread
// append cannot land on the same slot; duplicates are harmless because the
// poll de-dupes by module pointer when it rebuilds its target list.
bool PublishHitBody(uintptr_t entity, uintptr_t module, unsigned slot,
                    const char* cls)
{
    if (!module) { return false; }
    const LONG n = g_healthCandCount;
    for (LONG i = 0; i < n && i < kMaxHealthCandidates; ++i) {
        if (g_healthCands[i].module == module) { return false; }   // already known
    }
    const LONG idx = InterlockedIncrement(&g_healthCandCount) - 1;
    if (idx >= kMaxHealthCandidates) {
        InterlockedDecrement(&g_healthCandCount);
        return false;                      // full - the caller SAYS so, once
    }
    g_healthCands[idx].entity = entity;
    g_healthCands[idx].slot   = slot;
    strncpy_s(g_healthCands[idx].cls, sizeof(g_healthCands[idx].cls),
              cls ? cls : "<no rtti>", _TRUNCATE);
    // The module pointer LAST: it is what the poll de-dupes and reads on, so a
    // reader that sees it set is looking at an entry whose other fields are
    // already written.
    g_healthCands[idx].module = module;
    return true;
}

// Real test, 2026-09-04: this failed for all 64 captured entities in one
// session with no other evidence why - added exception code/faulting
// address capture (the SEH filter expression itself, since GetExceptionCode
// ()/GetExceptionInformation() are only valid there, not inside the handler
// block) so a repeat failure names the actual problem instead of leaving
// another silent dead end.
DWORD g_lastExceptionCode = 0;
uintptr_t g_lastExceptionAddr = 0;

bool SafeGetWorldPosition(GetWorldPositionFn fn, uintptr_t controlObjPtr, float out[3])
{
    bool ok = false;
    __try {
        float result[4] = {};
        fn(reinterpret_cast<void*>(controlObjPtr), result);
        out[0] = result[0];
        out[1] = result[1];
        out[2] = result[2];
        ok = true;
    } __except (
        g_lastExceptionCode = GetExceptionCode(),
        g_lastExceptionAddr = reinterpret_cast<uintptr_t>(
            GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
        EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

DWORD WINAPI ScanThread(LPVOID)
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("weaponanchor: engine_x64_rwdi.dll not mapped yet");
        return 0;
    }
    auto getWorldPos = reinterpret_cast<GetWorldPositionFn>(GetProcAddress(
        engine, "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ"));
    if (!getWorldPos) {
        LOG("weaponanchor: could not resolve IControlObject::GetWorldPosition export");
        return 0;
    }
    LOG("weaponanchor: IControlObject::GetWorldPosition resolved to %p (engine base %p, "
        "RVA 0x%zx)",
        reinterpret_cast<void*>(getWorldPos), reinterpret_cast<void*>(engine),
        reinterpret_cast<uintptr_t>(getWorldPos) - reinterpret_cast<uintptr_t>(engine));
    const uintptr_t movModVt = dlvr::RttiVtableOf("MovementModule");
    if (!movModVt) {
        LOG("weaponanchor: could not resolve MovementModule's vtable");
        return 0;
    }

    // Wait for the weapon to actually be in use before capturing anything. This
    // loop does NOT consume attempts - the kMaxAttempts budget exists to cover a
    // slow LOAD, and burning it while the player walks to a fight would recreate
    // the exact bug it was just raised from 8 to 60 to fix.
    {
        bool said = false;
        while (!weaponanchor::g_meleeUsed) {
            if (!DiagPlayerMovementModule) {
                LOG("weaponanchor: diagnostic disabled while waiting for first "
                    "melee swing, stopping scan thread");
                return 0;
            }
            if (!said) {
                said = true;
                LOG("weaponanchor: WAITING for the first melee swing before "
                    "capturing entities. This is deliberate and is NOT a stall: "
                    "the capture is one-shot, and scanning in a safe zone would "
                    "publish a list of NPCs who never take damage and freeze it "
                    "there. Swing the weapon where the fight is. Nothing is "
                    "consumed while waiting - the %d-attempt budget starts after "
                    "this.", kMaxAttempts);
            }
            Sleep(250);
        }
        LOG("weaponanchor: first melee swing seen - starting entity capture now.");
    }

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (!DiagPlayerMovementModule) {
            LOG("weaponanchor: diagnostic disabled, stopping scan thread");
            return 0;
        }

        HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!gamedll) {
            LOG("weaponanchor: attempt %d/%d - gamedll_x64_rwdi.dll not mapped yet",
                attempt, kMaxAttempts);
            Sleep(kRetryDelayMs);
            continue;
        }
        const uintptr_t addr = reinterpret_cast<uintptr_t>(gamedll) + kModulesLoopFnRva;
        dlvr::WatchExecSetStackFloatOffset(0);
        dlvr::WatchExecSetStackUseRsp(false);
        if (!dlvr::WatchExecStart(addr)) {
            LOG("weaponanchor: attempt %d/%d - WatchExecStart failed at gamedll+0x%zx",
                attempt, kMaxAttempts, kModulesLoopFnRva);
            Sleep(kRetryDelayMs);
            continue;
        }
        Sleep(kCollectMs);
        dlvr::WatchExecStop();

        dlvr::ExecHit hits[64] = {};
        const int hitCount = dlvr::WatchExecGet(hits, 64);
        LOG("weaponanchor: attempt %d/%d - collected %d distinct entity/module-loop "
            "call(s)",
            attempt, kMaxAttempts, hitCount);
        if (hitCount == 0) {
            Sleep(kRetryDelayMs);
            continue;
        }

        // Reference point: the currently-tracked render camera's own world
        // position - reuses rtticam.cpp's own scan+position-extraction
        // rather than duplicating it.
        dlvr::RttiSetClass("CRendererCamera");
        const int camFound = dlvr::RttiScanCameras();
        dlvr::RttiCamera cams[16] = {};
        const int gotCams = dlvr::RttiGetCameras(cams, 16);
        if (gotCams == 0) {
            LOG("weaponanchor: attempt %d/%d - no render camera found to use as a "
                "position reference (scan found %d) - cannot correlate",
                attempt, kMaxAttempts, camFound);
            Sleep(kRetryDelayMs);
            continue;
        }
        LOG("weaponanchor: attempt %d/%d - using %d tracked camera(s) as position "
            "reference",
            attempt, kMaxAttempts, gotCams);

        uintptr_t bestEntity = 0;
        float bestDistSq = 1e30f;
        // Closest entity that PASSES validation, tracked separately - see the
        // validate-first note in the loop below.
        uintptr_t bestValidEntity = 0;
        float bestValidDistSq = 1e30f;
        // And, independently, the closest entity carrying a HealthModule. The
        // damage watch needs a damageable body, not specifically the player.
        uintptr_t bestHealthEntity = 0;
        uintptr_t bestHealthModule = 0;
        float bestHealthDistSq = 1e30f;
        int posFailures = 0;
        // Rebuilt from scratch each attempt, so a retry cannot accumulate
        // duplicates or keep entities that have since gone away.
        InterlockedExchange(&g_healthCandCount, 0);
        for (int i = 0; i < hitCount; ++i) {
            const uintptr_t entity = hits[i].rcxValue;
            if (!entity) {
                continue;
            }
            const uintptr_t controlObjPtr = entity - 0x10;
            float pos[3] = {};
            if (!SafeGetWorldPosition(getWorldPos, controlObjPtr, pos)) {
                ++posFailures;
                if (posFailures <= 3) {
                    uintptr_t controlObjVtable = 0;
                    const bool vtOk = ReadPtr(controlObjPtr, controlObjVtable);
                    uintptr_t entityFirstQword = 0;
                    const bool entOk = ReadPtr(entity, entityFirstQword);
                    LOG("weaponanchor:   entity=%p (first qword=%s) controlObjPtr=%p "
                        "(would-be vtable=%s) - GetWorldPosition faulted: code=0x%08lX "
                        "at %p (offset from fn start = 0x%zx)",
                        reinterpret_cast<void*>(entity),
                        entOk ? "readable" : "UNREADABLE",
                        reinterpret_cast<void*>(controlObjPtr),
                        vtOk ? "readable" : "UNREADABLE", g_lastExceptionCode,
                        reinterpret_cast<void*>(g_lastExceptionAddr),
                        g_lastExceptionAddr - reinterpret_cast<uintptr_t>(getWorldPos));
                    if (entOk) {
                        LOG("weaponanchor:     entity first qword = %p",
                            reinterpret_cast<void*>(entityFirstQword));
                    }
                    if (vtOk) {
                        LOG("weaponanchor:     controlObjPtr would-be vtable = %p",
                            reinterpret_cast<void*>(controlObjVtable));
                    }
                }
                continue;
            }
            for (int c = 0; c < gotCams; ++c) {
                const float dx = pos[0] - cams[c].px;
                const float dy = pos[1] - cams[c].py;
                const float dz = pos[2] - cams[c].pz;
                const float distSq = dx * dx + dy * dy + dz * dz;
                LOG("weaponanchor:   entity=%p pos=(%.2f,%.2f,%.2f) hits=%llu "
                    "dist-to-cam[%d]=%.2f",
                    reinterpret_cast<void*>(entity), pos[0], pos[1], pos[2],
                    static_cast<unsigned long long>(hits[i].count), c, sqrtf(distSq));
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestEntity = entity;
                }
                // ===== VALIDATE FIRST, THEN TAKE THE CLOSEST =====
                //
                // The original logic took the single closest entity and THEN
                // checked its modules[] - so one bad nearest candidate ended the
                // whole attempt and the second-nearest was never tried. Measured
                // 2026-09-10: attempt 7 captured 64 entities and 8 cameras, chose
                // the nearest at distance 654 with a NULL modules[] slot, and
                // failed - having never looked at the other 63.
                //
                // So track the closest entity that ACTUALLY VALIDATES, per kind.
                // Same distance metric, same data; only the order of the filter
                // changes, which is why this cannot regress the movemod path -
                // any entity it would have accepted before still wins here.
                {
                    uintptr_t mm = 0;
                    uintptr_t mmVt = 0;
                    if (ReadPtr(entity + kModulesArrayOffset +
                                    kMovementModuleSlot * sizeof(uintptr_t), mm) &&
                        mm && ReadPtr(mm, mmVt) && mmVt == movModVt &&
                        distSq < bestValidDistSq) {
                        bestValidDistSq = distSq;
                        bestValidEntity = entity;
                    }
                }
                // AND, INDEPENDENTLY, the closest entity carrying a HEALTH module.
                //
                // The damage watch does NOT need the player - it needs any
                // damageable entity, because the writers of health are the same
                // pipeline whichever body takes the hit. Slot 22 is
                // $HEALTH_MODULE from data/ai/ai_modules.def, and the class is
                // confirmed by RTTI rather than trusted, since that .def is 2020
                // data and this is EE.
                {
                    // BOTH health slots, EVERY entity - not just the nearest.
                    // The nearest is still tracked for the legacy single-pointer
                    // publish, but the damage watch now polls the whole list and
                    // picks the body whose value actually moves.
                    const size_t healthSlots[2] = { kHealthModuleSlotWA,
                                                    kBodyPartsHealthSlotWA };
                    for (size_t si = 0; si < 2; ++si) {
                        uintptr_t hm = 0;
                        char hcls[96] = {};
                        if (!ReadPtr(entity + kModulesArrayOffset +
                                         healthSlots[si] * sizeof(uintptr_t), hm) ||
                            !hm || !RttiClassNameOf(hm, hcls, sizeof(hcls)) ||
                            !strstr(hcls, "HealthModule")) {
                            continue;
                        }
                        const LONG n = g_healthCandCount;
                        if (n < kMaxHealthCandidates) {
                            bool dup = false;
                            for (LONG k = 0; k < n; ++k) {
                                if (g_healthCands[k].module == hm) { dup = true; break; }
                            }
                            if (!dup) {
                                g_healthCands[n].entity = entity;
                                g_healthCands[n].module = hm;
                                g_healthCands[n].slot   = static_cast<unsigned>(healthSlots[si]);
                                strncpy_s(g_healthCands[n].cls,
                                          sizeof(g_healthCands[n].cls), hcls, _TRUNCATE);
                                InterlockedExchange(&g_healthCandCount, n + 1);
                            }
                        }
                        if (healthSlots[si] == kHealthModuleSlotWA &&
                            distSq < bestHealthDistSq) {
                            bestHealthDistSq = distSq;
                            bestHealthEntity = entity;
                            bestHealthModule = hm;
                            strncpy_s(g_healthModuleClass, sizeof(g_healthModuleClass),
                                      hcls, _TRUNCATE);
                        }
                    }
                }
            }
        }

        // Prefer a VALIDATED entity over merely the nearest one.
        if (bestValidEntity) {
            if (bestValidEntity != bestEntity) {
                LOG("weaponanchor: attempt %d/%d - the NEAREST entity (%p, dist %.2f) "
                    "failed validation, but a validated one exists at %p (dist %.2f) "
                    "- using that. The old logic would have failed this attempt here.",
                    attempt, kMaxAttempts, reinterpret_cast<void*>(bestEntity),
                    sqrtf(bestDistSq), reinterpret_cast<void*>(bestValidEntity),
                    sqrtf(bestValidDistSq));
            }
            bestEntity = bestValidEntity;
            bestDistSq = bestValidDistSq;
        }

        // Publish the health module regardless of whether the PLAYER was
        // identified - it is a separate question and the damage watch only needs
        // this one.
        if (bestHealthModule) {
            g_healthModulePtr = bestHealthModule;
            InterlockedExchange(&g_haveHealthModule, 1);
            LOG("weaponanchor: HEALTH MODULE FOUND on entity %p (dist %.2f): %p "
                "class '%s' - published for diag_damage_watch. This does NOT have "
                "to be the player; any damageable body exercises the same health "
                "writers.",
                reinterpret_cast<void*>(bestHealthEntity), sqrtf(bestHealthDistSq),
                reinterpret_cast<void*>(bestHealthModule), g_healthModuleClass);
            // The list is what the damage watch actually uses now; the single
            // pointer above is legacy and is NOT what gets watched.
            LOG("weaponanchor: %ld health-module CANDIDATE(S) published (slots %zu "
                "and %zu across all %d captured entities). diag_damage_watch polls "
                "ALL of them and watches whichever one's value MOVES - proximity is "
                "no longer used to choose.",
                g_healthCandCount, kHealthModuleSlotWA, kBodyPartsHealthSlotWA,
                hitCount);
        } else {
            LOG("weaponanchor: attempt %d/%d - NO captured entity had a valid "
                "HealthModule at modules[%zu]. Either the EE slot index differs "
                "from ai_modules.def's 22, or none of the 64 captured entities is "
                "damageable.",
                attempt, kMaxAttempts, kHealthModuleSlotWA);
        }

        if (!bestEntity) {
            LOG("weaponanchor: attempt %d/%d - could not compute a world position for "
                "any captured entity (%d of %d GetWorldPosition calls faulted)",
                attempt, kMaxAttempts, posFailures, hitCount);
            Sleep(kRetryDelayMs);
            continue;
        }
        LOG("weaponanchor: attempt %d/%d - closest entity to the camera = %p "
            "(distance %.2f) - verifying against modules[]...",
            attempt, kMaxAttempts, reinterpret_cast<void*>(bestEntity), sqrtf(bestDistSq));

        uintptr_t movModPtr = 0;
        if (!ReadPtr(bestEntity + kModulesArrayOffset + kMovementModuleSlot * sizeof(uintptr_t),
                     movModPtr) ||
            !movModPtr) {
            LOG("weaponanchor: attempt %d/%d - closest entity's own +0xC10 modules[] "
                "slot is null/unreadable - not a match",
                attempt, kMaxAttempts);
            Sleep(kRetryDelayMs);
            continue;
        }
        uintptr_t movModVtActual = 0;
        if (!ReadPtr(movModPtr, movModVtActual) || movModVtActual != movModVt) {
            LOG("weaponanchor: attempt %d/%d - closest entity's +0xC10 modules[] slot "
                "does not point to a real MovementModule - not a match",
                attempt, kMaxAttempts);
            Sleep(kRetryDelayMs);
            continue;
        }

        const bool firstLatch = (InterlockedExchange(&g_havePlayerEntity, 1) == 0);
        g_entityBase = bestEntity;
        g_movModPtr = movModPtr;
        if (firstLatch) {
            LOG("weaponanchor: CONFIRMED player entityBase=%p movMod=%p (found via live "
                "position correlation) - now watching m_MoveStyle (+0x154) / "
                "m_CurrentMoveStyle (+0x184)",
                reinterpret_cast<void*>(bestEntity), reinterpret_cast<void*>(movModPtr));
        }

        // >>> DO NOT RETURN. THE CAPTURE USED TO BE ONE-SHOT AND THAT VOIDED A RUN. <<<
        // Until 2026-09-15 this returned 0 here, so g_healthCands froze at whatever
        // 64 entities happened to be in the 2 s collect window. The weapon-out gate
        // fixed WHEN that snapshot is taken; it is still a SNAPSHOT. The 16:48 run
        // captured 17 HumanAI at the first swing and then polled those same 17 for
        // four minutes while the player fought and killed entirely different bodies -
        // zero hits, and no way to tell that from "the offsets are wrong".
        //
        // Now it keeps re-capturing, which re-publishes g_healthCands from live
        // entities each pass. Stops as soon as damagewatch arms its own exec watch:
        // SEVEN CLIENTS, ONE GLOBAL SLOT, and two of them fighting over it is what
        // invalidated the 2026-09-10 melee run.
        if (InterlockedCompareExchange(&g_damageWatchArmed, 0, 0) != 0) {
            LOG("weaponanchor: damagewatch has armed its exec watch - stopping the "
                "entity re-capture so the two do not contend for the single "
                "WatchExec slot. The candidate list is now frozen deliberately.");
            return 0;
        }
        // Park until something asks for a fresh capture. Costs nothing while
        // waiting - no breakpoints are armed here.
        {
            bool announced = false;
            for (;;) {
                if (!DiagPlayerMovementModule) { return 0; }
                if (InterlockedCompareExchange(&g_damageWatchArmed, 0, 0) != 0) {
                    LOG("weaponanchor: damagewatch has armed its exec watch - "
                        "entity re-capture is now OFF for the rest of this "
                        "process so the two cannot contend for the single "
                        "WatchExec slot.");
                    return 0;
                }
                if (InterlockedExchange(&g_recaptureReq, 0) != 0) {
                    LOG("weaponanchor: re-capture REQUESTED - the current entity "
                        "list looked dead. Arming the modules-loop watch for "
                        "%lu ms (this is the expensive part; it is deliberately "
                        "not on a timer).", kCollectMs);
                    break;
                }
                if (!announced) {
                    announced = true;
                    LOG("weaponanchor: entity list published and PARKED. No "
                        "breakpoints armed while parked - a re-capture happens "
                        "only when the list goes stale or a window sees nothing.");
                }
                Sleep(250);
            }
        }
        attempt = 0;
        continue;
    }

    LOG("weaponanchor: GAVE UP after %d attempts (~%d s of trying). THE WHOLE "
        "CHAIN IS NOW DEAD FOR THE REST OF THIS PROCESS: no health-module "
        "candidates will ever be published, so damagewatch will refuse "
        "forever and any probe downstream of it holds ZERO data - a negative "
        "from here is about THIS THREAD, not about the thing being probed. "
        "Every attempt collected 0 entity/module-loop calls, which means no "
        "live gameplay entities existed while it was looking - normally a "
        "load screen. RESTART THE GAME AND BE IN GAMEPLAY SOONER.",
        kMaxAttempts,
        (int)((kMaxAttempts * (kRetryDelayMs + kCollectMs)) / 1000));
    return 0;
}

// Called once, right after g_entityBase is confirmed - see the big comment
// on kPlayerFitnessModuleOff above for why this is worth trying and how it
// sidesteps the stale-heap-scan problem that killed this same lead 3 times
// before tonight's investigation even started.
void FindFitnessModuleOnce()
{
    char className[128] = {};
    struct Candidate { uintptr_t addr; char label[24]; };
    Candidate candidates[2 + 2 * kNumPlayerDiOffsets] = {};
    int n = 0;
    candidates[n].addr = g_entityBase;
    _snprintf_s(candidates[n].label, sizeof(candidates[n].label), _TRUNCATE, "entity itself");
    ++n;
    for (int i = 0; i < kNumPlayerDiOffsets; ++i) {
        const size_t off = kPlayerDiOffsets[i];
        candidates[n].addr = g_entityBase + off;
        _snprintf_s(candidates[n].label, sizeof(candidates[n].label), _TRUNCATE, "+0x%zx", off);
        ++n;
        candidates[n].addr = g_entityBase - off;
        _snprintf_s(candidates[n].label, sizeof(candidates[n].label), _TRUNCATE, "-0x%zx", off);
        ++n;
    }

    for (int i = 0; i < n; ++i) {
        uintptr_t fitnessPtr = 0;
        if (!ReadPtr(candidates[i].addr + kPlayerFitnessModuleOff, fitnessPtr) || !fitnessPtr) {
            continue;
        }
        if (!dlvr::RttiClassNameOf(fitnessPtr, className, sizeof(className))) {
            continue;
        }
        if (strcmp(className, "PlayerFitnessModule") == 0) {
            g_fitnessModPtr = fitnessPtr;
            InterlockedExchange(&g_haveFitnessModule, 1);
            LOG("weaponanchor: FOUND PlayerFitnessModule=%p via candidate [%s]+0x%zx - "
                "watching it for a byte-diff-confirmed sprint flag too",
                reinterpret_cast<void*>(fitnessPtr), candidates[i].label,
                kPlayerFitnessModuleOff);
            return;
        }
    }
    // (The second attempt through the bootstrap's captured-entity exports was
    // removed 2026-11-15, batch 4 - those exports went in batch 3.)
    LOG("weaponanchor: could not find PlayerFitnessModule via any of %d offset-based "
        "candidates from g_entityBase - giving up on PlayerFitnessModule for now",
        n);
}

}  // namespace weaponanchor

// Investigation, 2026-09-04: a VR-only ground-plane rendering artifact (a
// crisp straight line plus a scatter of point/dot primitives, reproducible
// across many different locations, angle/distance-dependent) was traced via
// static analysis of gamedll_x64_rwdi.dll to a real, dedicated debug-render
// subsystem (DebugRenderer, with RenderLineData/RenderPointData primitive
// classes, sitting right next to the game's navmesh/physics-debug strings).
// Working theory: something that normally suppresses this on the desktop
// path never reaches VR's eye viewports. ILevel::SetPhysicsDebugRenderMode
// (engine+0x29b590, confirmed via dumpbin /exports as a real, non-virtual,
// plain-callable export - no vtable slot needed) is the most direct lever
// found - a mode==0 call clears it and, per its own decompile, unconditionally
// runs the same cleanup path whenever the mode is actually changing. Getting
// a live ILevel* needs only a live IGSObject* (any one - the current level is
// shared by every entity), obtained via IGSObject::GetLevel (engine+0x2521d0,
// also a plain export). Reuses weaponanchor's own already-proven WatchExecStart
// technique at gamedll+kModulesLoopFnRva to grab a live IGSObject* - this
// namespace does not need weaponanchor's own player-identification machinery
// at all, since any entity's IGSObject* resolves to the same current level.
namespace debugrenderkill {

typedef void* (*GetLevelFn)(void* igsObjectThis);
typedef void (*SetPhysicsDebugRenderModeFn)(void* levelThis, unsigned short mode);
typedef void (*SetEnabledFn)(void* debugRendererThis, unsigned char enabled);

// Real DebugRenderer::SetPhysicsDebugRenderMode(0) call executed cleanly
// (2026-09-04 live test) but the ground-plane line/dot artifact was still
// visible - ruling out the physics-collision debug mode specifically, not
// the whole DebugRenderer class. Resolved DebugRenderer's own vtable via the
// same type-descriptor -> RTTICompleteObjectLocator -> vtable static chain
// InvestigatePlayerDI.java used for PlayerDI (see ResolveDebugRendererVtable
// .java / debugrenderer_vtable.txt). Vtable slot 5 (gamedll RVA 0x111a660)
// decompiles as a genuine per-frame update - real delta-time accumulation,
// then `if (*(char*)(this+9) != 0) { <calls through a global renderer
// interface pointer at fixed offsets 0x1648/0x1650 - the actual GPU submit>
// }`. Two other vtable slots (0xAA8310, called from slots 6 and 10) both
// decompile as a trivial one-byte setter for that exact same +9 field -
// almost certainly SetEnabled(bool). This is the real draw-output switch,
// independent of the physics-mode flag already ruled out.
constexpr uintptr_t kDebugRendererSetEnabledRva = 0xAA8310;

volatile LONG g_started = 0;
volatile LONG g_done = 0;
volatile LONG g_debugRendererDisabled = 0;

// 2026-09-04: a one-shot SetEnabled(false) call on both found candidate
// instances executed cleanly (no fault) but had zero visible effect on the
// ground-plane artifact - same as the earlier one-shot
// SetPhysicsDebugRenderMode(0) call. Testing whether something re-enables
// the flag every frame (which a single shot could never outrun) by
// re-applying it continuously instead: cache the resolved setter and the
// found instance addresses, then call SetEnabled(false) on each, every
// single LevelCamera() call - cheap once the scan itself is done.
constexpr int kMaxDebugRendererInstances = 16;
uintptr_t g_debugRendererInstances[kMaxDebugRendererInstances] = {};
volatile LONG g_numDebugRendererInstances = 0;
SetEnabledFn g_setEnabledFn = nullptr;

bool SafeCallGetLevel(GetLevelFn fn, void* igsObj, void*& outLevel)
{
    bool ok = false;
    __try {
        outLevel = fn(igsObj);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeCallSetPhysicsDebugRenderMode(SetPhysicsDebugRenderModeFn fn, void* level, unsigned short mode)
{
    bool ok = false;
    __try {
        fn(level, mode);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeCallSetEnabled(SetEnabledFn fn, void* debugRenderer, unsigned char enabled)
{
    bool ok = false;
    __try {
        fn(debugRenderer, enabled);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// Live scan for the DebugRenderer singleton, reusing rtticam.cpp's own
// RTTI-vtable heap scanner (already proven for cameras, PlayerFitnessModule,
// etc.) - it only filters/validates position for the camera family, so a
// non-camera class comes back with its address intact and an untouched
// identity orientation.
void TryDisableDebugRendererDirectly(HMODULE gamedll)
{
    dlvr::RttiSetClass("DebugRenderer");
    const int scanned = dlvr::RttiScanCameras();
    dlvr::RttiCamera candidates[16] = {};
    const int got = dlvr::RttiGetCameras(candidates, 16);
    LOG("debugrenderkill: DebugRenderer RTTI scan found %d, retrieved %d",
        scanned, got);
    if (got == 0) {
        return;
    }

    g_setEnabledFn = reinterpret_cast<SetEnabledFn>(
        reinterpret_cast<uintptr_t>(gamedll) + kDebugRendererSetEnabledRva);

    int stored = 0;
    for (int i = 0; i < got && stored < kMaxDebugRendererInstances; ++i) {
        void* instance = reinterpret_cast<void*>(candidates[i].object);
        if (!instance) continue;
        LOG("debugrenderkill: DebugRenderer candidate instance=%p - calling "
            "SetEnabled(false) once, then caching for continuous enforcement",
            instance);
        if (SafeCallSetEnabled(g_setEnabledFn, instance, 0)) {
            LOG("debugrenderkill: DebugRenderer::SetEnabled(false) returned "
                "cleanly on instance=%p",
                instance);
            g_debugRendererInstances[stored++] = candidates[i].object;
        } else {
            LOG("debugrenderkill: DebugRenderer::SetEnabled faulted on "
                "instance=%p",
                instance);
        }
    }
    InterlockedExchange(&g_numDebugRendererInstances, stored);
    if (stored > 0) {
        InterlockedExchange(&g_debugRendererDisabled, 1);
    }
}

DWORD WINAPI KillThread(LPVOID)
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!engine || !gamedll) {
        LOG("debugrenderkill: engine/gamedll not mapped yet");
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    auto getLevel = reinterpret_cast<GetLevelFn>(
        GetProcAddress(engine, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ"));
    auto setMode = reinterpret_cast<SetPhysicsDebugRenderModeFn>(
        GetProcAddress(engine, "?SetPhysicsDebugRenderMode@ILevel@@QEAAXG@Z"));
    if (!getLevel || !setMode) {
        LOG("debugrenderkill: could not resolve exports (getLevel=%p setMode=%p)",
            reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(setMode));
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    LOG("debugrenderkill: resolved IGSObject::GetLevel=%p, "
        "ILevel::SetPhysicsDebugRenderMode=%p",
        reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(setMode));

    const uintptr_t addr = reinterpret_cast<uintptr_t>(gamedll) + weaponanchor::kModulesLoopFnRva;
    dlvr::WatchExecSetStackFloatOffset(0);
    dlvr::WatchExecSetStackUseRsp(false);
    if (!dlvr::WatchExecStart(addr)) {
        LOG("debugrenderkill: WatchExecStart failed at gamedll+0x%zx",
            weaponanchor::kModulesLoopFnRva);
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    Sleep(2000);
    dlvr::WatchExecStop();

    dlvr::ExecHit hits[64] = {};
    const int hitCount = dlvr::WatchExecGet(hits, 64);
    LOG("debugrenderkill: collected %d live IGSObject candidate(s)", hitCount);
    if (hitCount == 0) {
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    for (int i = 0; i < hitCount; ++i) {
        void* igsObj = reinterpret_cast<void*>(hits[i].rcxValue);
        if (!igsObj) continue;
        void* level = nullptr;
        if (!SafeCallGetLevel(getLevel, igsObj, level)) {
            LOG("debugrenderkill: GetLevel faulted on igsObj=%p", igsObj);
            continue;
        }
        if (!level) {
            continue;
        }
        LOG("debugrenderkill: got live ILevel=%p via igsObj=%p - calling "
            "SetPhysicsDebugRenderMode(0)",
            level, igsObj);
        if (SafeCallSetPhysicsDebugRenderMode(setMode, level, 0)) {
            LOG("debugrenderkill: SetPhysicsDebugRenderMode(level, 0) returned "
                "cleanly - physics/navmesh debug render mode cleared");
            InterlockedExchange(&g_done, 1);
        } else {
            LOG("debugrenderkill: SetPhysicsDebugRenderMode faulted on level=%p", level);
        }
        break;
    }

    TryDisableDebugRendererDirectly(gamedll);

    InterlockedExchange(&g_started, 0);
    return 0;
}

// Cheap, called every LevelCamera() once instances are known - re-applies
// SetEnabled(false) continuously, to test whether something else re-enables
// the flag every frame (which the earlier one-shot call could never outrun).
void EnforceDebugRendererDisabled()
{
    const int n = g_numDebugRendererInstances;
    if (n == 0 || !g_setEnabledFn) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        SafeCallSetEnabled(g_setEnabledFn, reinterpret_cast<void*>(g_debugRendererInstances[i]), 0);
    }
}

}  // namespace debugrenderkill

// PHASE 1 of the VR draw-distance investigation (2026-09-05): read-only probe
// of the engine's own visibility/LOD distance values.
//
// Established by this point: four distance-driven subsystems degrade together
// in VR but not flatscreen (texture streaming, mesh LOD, entity visibility,
// AI/animation LOD). The camera hook is EXONERATED (a horizon_lock=0 run left
// draw distance unchanged, with zero camhook lines in the log proving the hook
// never installed). Narrowing the frustum 25% (fov_source=0, +-54 -> +-45.8
// deg) changed nothing visible. /notexturestreaming made the missing NPC
// render but did NOT stop it de-rendering on retreat - so texture streaming
// was one layer, and underneath it a HARD, REVERSIBLE distance threshold on
// mesh/entity visibility is still firing far too close.
//
// engine_x64_rwdi.dll exports the relevant knobs as plain non-virtual methods
// (confirmed via dumpbin - QEAA/QEBA, so directly callable with a valid this,
// exactly like IGSObject::GetLevel and ILevel::SetPhysicsDebugRenderMode
// already are elsewhere in this file):
//     ?GetObjectsVisibilityRange@ILevel@@QEBAMXZ    float get
//     ?SetObjectsVisibilityRange@ILevel@@QEAAXM@Z   void  set(float)
//     ?SetMeshCullSize@ILevel@@QEAAXM@Z             void  set(float)
//     ?SetMeshCullDistTreshold@ILevel@@QEAAXM@Z     void  set(float)  [sic]
//
// This probe only READS. Run it once in VR and once flatscreen on the same
// save and the answer falls out either way:
//   values DIFFER    -> a real VR-conditional override exists; we then know
//                       the exact value and what flatscreen's correct one is.
//   values IDENTICAL -> the settings match and the divergence is in how they
//                       are APPLIED - i.e. MeshCullSize is screen-relative and
//                       our much wider frustum shrinks every object's
//                       projected size below the cull threshold.
// Deliberately read-only so there is no chance of the crash class hit earlier:
// no hardware breakpoint stays armed, and nothing engine-side is mutated.
namespace visibilityprobe {

typedef float (*GetObjectsVisibilityRangeFn)(void* levelThis);

volatile LONG g_started = 0;
volatile LONG g_done = 0;

bool SafeCallGetFloat(GetObjectsVisibilityRangeFn fn, void* level, float& out)
{
    bool ok = false;
    __try {
        out = fn(level);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

DWORD WINAPI ProbeThread(LPVOID)
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!engine || !gamedll) {
        LOG("visibilityprobe: engine/gamedll not mapped yet");
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    auto getLevel = reinterpret_cast<debugrenderkill::GetLevelFn>(
        GetProcAddress(engine, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ"));
    auto getRange = reinterpret_cast<GetObjectsVisibilityRangeFn>(
        GetProcAddress(engine, "?GetObjectsVisibilityRange@ILevel@@QEBAMXZ"));
    if (!getLevel || !getRange) {
        LOG("visibilityprobe: could not resolve exports (getLevel=%p getRange=%p)",
            reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(getRange));
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    LOG("visibilityprobe: resolved IGSObject::GetLevel=%p "
        "ILevel::GetObjectsVisibilityRange=%p",
        reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(getRange));

    // Bounded retries: the modules loop only has live entities to catch once
    // real gameplay is running, which may be a while after this thread starts.
    // 60 attempts, not 12: a real flatscreen run spent its entire 66-second
    // budget still loading the save (engine log showed meshes loading past the
    // last attempt), so the probe gave up before gameplay entities ever
    // existed. ~5.5s per attempt, so this now covers roughly five minutes.
    for (int attempt = 1; attempt <= 60 && !g_done; ++attempt) {
        if (!DiagVisibilityProbe) {
            LOG("visibilityprobe: diagnostic disabled, stopping");
            break;
        }
        const uintptr_t addr =
            reinterpret_cast<uintptr_t>(gamedll) + weaponanchor::kModulesLoopFnRva;
        dlvr::WatchExecSetStackFloatOffset(0);
        dlvr::WatchExecSetStackUseRsp(false);
        if (!dlvr::WatchExecStart(addr)) {
            LOG("visibilityprobe: attempt %d/60 - WatchExecStart failed", attempt);
            Sleep(4000);
            continue;
        }
        Sleep(1500);
        dlvr::WatchExecStop();

        dlvr::ExecHit hits[16] = {};
        const int hitCount = dlvr::WatchExecGet(hits, 16);
        if (hitCount == 0) {
            LOG("visibilityprobe: attempt %d/60 - no live entities yet", attempt);
            Sleep(4000);
            continue;
        }

        for (int i = 0; i < hitCount; ++i) {
            void* igsObj = reinterpret_cast<void*>(hits[i].rcxValue);
            if (!igsObj) continue;
            void* level = nullptr;
            if (!debugrenderkill::SafeCallGetLevel(getLevel, igsObj, level) || !level) {
                continue;
            }
            float range = -1.0f;
            if (!SafeCallGetFloat(getRange, level, range)) {
                LOG("visibilityprobe: GetObjectsVisibilityRange faulted on level=%p", level);
                continue;
            }
            LOG("visibilityprobe: *** ILevel=%p  GetObjectsVisibilityRange() = %.3f *** "
                "(compare this number between a VR run and a flatscreen run on the same "
                "save - see this namespace's own comment for what each outcome means)",
                level, range);
            InterlockedExchange(&g_done, 1);
            break;
        }
        if (!g_done) Sleep(4000);
    }

    // Deliberately does NOT reset g_started. This is called from
    // dlvr_FilterScePad, which polls at ~250Hz - clearing the latch on exit
    // meant that the moment this thread gave up, the very next poll spawned
    // another one, and so on: a real run produced 3992 spawned threads and
    // 3992 duplicate log lines in about two minutes. One attempt per process
    // is the correct lifetime for a one-shot probe.
    return 0;
}

}  // namespace visibilityprobe

// Investigation, 2026-09-04 continued: three separate, real, correctly-
// executing DebugRenderer/physics-debug-mode kill attempts (above) had zero
// effect on the ground-plane line/dot artifact - ruling out a wrongly-
// enabled debug-render system. New evidence changed the theory: the
// artifact is DISTANCE-triggered - walking up to it makes it disappear
// entirely, resolving to normal ground geometry - the textbook signature of
// a far-LOD/streaming placeholder being replaced by real detail as the
// player approaches, not a persistent rendering bug. This also lines up
// with a separately-reported symptom (some textures staying low-res longer
// than expected) as a second symptom of the same root cause, not a
// coincidence.
//
// Lead: ILevel::SetSimulationCamera(const vec3& pos, const vec3& dir)
// (engine export, confirmed via dumpbin - see the mangled name below) is a
// distinct entry point from the render-camera hook this mod already drives.
// Many open-world engines keep a separate "simulation/streaming" reference
// camera precisely so a cutscene, replay, or (here) a VR headset's per-eye
// render cameras don't drag world-streaming/LOD decisions along with them.
// If the desktop path feeds this every frame with the player's real
// position and this mod has never called it at all, every distance-based
// streaming/LOD decision could be running off a stale or default reference
// point - explaining both symptoms with one root cause.
//
// Static tracing could not confirm this (both apparent callers found via
// getReferencesTo turned out to be thunk/vtable artifacts, not real
// gameplay call sites, so the exact desktop-fed values are unknown) - this
// is a genuine experiment, read empirically rather than guessed further in
// static analysis.
//
// First attempt fed GetHeadPose()'s raw OpenXR local/room-space position -
// real bug, caught from the log: values came back like (-0.03,-0.01,0.00),
// a room-space coordinate near the VR play-area's own origin, nowhere near
// the game's actual world-space coordinate system (which for an open-world
// level should be tens/hundreds of units). Fixed by reusing weaponanchor's
// own confirmed-live player entity and the same IControlObject::
// GetWorldPosition mechanism already proven correct earlier this
// investigation (real distances like 5.84 units, matching expectation) -
// this namespace now depends on diag_player_movemod also being enabled.
// The second vec3 parameter's exact meaning (direction vs. look-at point)
// is still unconfirmed - passing the same position for both (a degenerate
// "no preferred direction") avoids introducing a second, unvalidated
// coordinate-frame conversion (OpenXR head rotation is room-space, not
// necessarily the game's own world rotation frame - exactly the class of
// bug the aim-redirect work hit repeatedly) while still testing whether
// position alone is the dominant factor for near-field LOD/streaming.
namespace simcamerafeed {

typedef void (*SetSimulationCameraFn)(void* levelThis, const float* pos, const float* dir);
typedef void (*GetWorldPositionFn)(void* controlObjectThis, float* outVec3);

volatile LONG g_ready = 0;
uintptr_t g_level = 0;
GetWorldPositionFn g_getWorldPos = nullptr;
SetSimulationCameraFn g_setSimCam = nullptr;

bool SafeCallSetSimCamera(SetSimulationCameraFn fn, void* level, const float pos[3], const float dir[3])
{
    bool ok = false;
    __try {
        fn(level, pos, dir);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeCallGetWorldPos(GetWorldPositionFn fn, void* controlObj, float out[3])
{
    bool ok = false;
    __try {
        float result[4] = {};
        fn(controlObj, result);
        out[0] = result[0];
        out[1] = result[1];
        out[2] = result[2];
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// One-shot setup once weaponanchor has confirmed the player entity: resolve
// both engine exports and get ILevel* via the same live entity.
void TryInit()
{
    if (!weaponanchor::g_havePlayerEntity) {
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        return;
    }
    auto getLevel = reinterpret_cast<debugrenderkill::GetLevelFn>(
        GetProcAddress(engine, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ"));
    g_getWorldPos = reinterpret_cast<GetWorldPositionFn>(
        GetProcAddress(engine, "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ"));
    g_setSimCam = reinterpret_cast<SetSimulationCameraFn>(
        GetProcAddress(engine, "?SetSimulationCamera@ILevel@@UEAAXAEBVvec3@@0@Z"));
    if (!getLevel || !g_getWorldPos || !g_setSimCam) {
        LOG("simcamerafeed: could not resolve one or more exports (getLevel=%p "
            "getWorldPos=%p setSimCam=%p)",
            reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(g_getWorldPos),
            reinterpret_cast<void*>(g_setSimCam));
        return;
    }

    void* level = nullptr;
    if (!debugrenderkill::SafeCallGetLevel(
            getLevel, reinterpret_cast<void*>(weaponanchor::g_entityBase), level) ||
        !level) {
        LOG("simcamerafeed: GetLevel faulted or returned null on confirmed player entity");
        return;
    }
    g_level = reinterpret_cast<uintptr_t>(level);
    LOG("simcamerafeed: ready - ILevel=%p, will feed real world position every frame",
        level);
    InterlockedExchange(&g_ready, 1);
}

}  // namespace simcamerafeed

// Investigation, 2026-09-04 continued: after the stereo-rotation theory was
// cleanly ruled out, went back to the DebugRenderer class family found
// earlier and checked the classes never actually resolved before -
// RenderLineData/RenderPointData (individual primitive instances, sharing a
// BaseData-family per-frame Update pattern with DebugRenderer itself) and
// CLineListEx (the real container - its own vtable is much richer than the
// simple primitives'). CLineListEx's vtable slot 2 (gamedll RVA 0x13304f0)
// decompiles as a genuine "create and add a new line" factory: allocates a
// new item via one virtual call, initialises several fields to 1.0f, then
// adds it to a list via a second virtual call using data straight from its
// own 3rd argument. This is the most direct candidate yet for "who actually
// creates a debug line" - a live execution breakpoint here, capped and
// bounded the same way every other diagnostic tool in this project is, will
// show whether it fires at all while the artifact is visible and what its
// arguments look like.
namespace lineaddwatch {

constexpr uintptr_t kAddLineFnRva = 0x13304f0;
volatile LONG g_started = 0;

// A single 10s window would almost certainly fire and finish long before a
// tester could navigate from spawn to the specific spot being investigated -
// repeats indefinitely (bounded, like every other retry loop in this file)
// so there's always a live window whenever the tester actually gets there.
constexpr int kMaxWindows = 60;         // ~15 minutes of coverage
constexpr DWORD kWindowMs = 15000;

DWORD WINAPI WatchThread(LPVOID)
{
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gamedll) {
        LOG("lineaddwatch: gamedll_x64_rwdi.dll not mapped yet");
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    const uintptr_t g_gamedllBase = reinterpret_cast<uintptr_t>(gamedll);
    const uintptr_t addr = g_gamedllBase + kAddLineFnRva;
    LOG("lineaddwatch: arming at gamedll+0x%zx (%p) - repeating %d x %ds windows, "
        "look at the artifact any time",
        kAddLineFnRva, reinterpret_cast<void*>(addr), kMaxWindows, kWindowMs / 1000);

    dlvr::WatchExecSetStackFloatOffset(0);
    dlvr::WatchExecSetStackUseRsp(false);

    for (int window = 1; window <= kMaxWindows; ++window) {
        if (!DiagWatchLineAdd) {
            LOG("lineaddwatch: diagnostic disabled, stopping");
            break;
        }
        if (!dlvr::WatchExecStart(addr)) {
            LOG("lineaddwatch: window %d/%d - WatchExecStart failed", window, kMaxWindows);
            Sleep(2000);
            continue;
        }
        Sleep(kWindowMs);
        dlvr::WatchExecStop();

        dlvr::ExecHit hits[64] = {};
        const int hitCount = dlvr::WatchExecGet(hits, 64);
        if (hitCount == 0) {
            LOG("lineaddwatch: window %d/%d - 0 hits", window, kMaxWindows);
            continue;
        }
        LOG("lineaddwatch: window %d/%d - %d distinct (RCX,RDX) pair(s) hit "
            "CLineListEx's add-item function",
            window, kMaxWindows, hitCount);
        for (int i = 0; i < hitCount && i < 20; ++i) {
            char cls[128] = "?";
            dlvr::RttiClassNameOf(hits[i].rcxValue, cls, sizeof(cls));
            // returnAddr names the actual calling instruction. This site is
            // vtable-dispatched, so it has no static xref to trace back from -
            // the return address is the only way to find out what code decides
            // to draw these lines. Reported module-relative so it can be
            // looked up directly in the Ghidra project (gamedll's image base
            // is 0x180000000 there, matching this RVA).
            const uintptr_t ra = hits[i].returnAddr;
            const uintptr_t rva = (ra >= g_gamedllBase) ? (ra - g_gamedllBase) : 0;
            LOG("lineaddwatch:   hit %d: rcx(this)=%p [%s] rdx(param_2)=%p count=%lld "
                "caller=gamedll+0x%zx (raw %p)",
                i, reinterpret_cast<void*>(hits[i].rcxValue), cls,
                reinterpret_cast<void*>(hits[i].rdxValue), hits[i].count,
                static_cast<size_t>(rva), reinterpret_cast<void*>(ra));
        }
    }
    InterlockedExchange(&g_started, 0);
    return 0;
}

}  // namespace lineaddwatch

void WatchLineAddOnce()
{
    if (!DiagWatchLineAdd) {
        return;
    }
    if (InterlockedCompareExchange(&lineaddwatch::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, lineaddwatch::WatchThread, nullptr, 0, nullptr);
    }
}

void FeedSimulationCamera()
{
    if (!DiagFeedSimCamera) {
        return;
    }
    if (!simcamerafeed::g_ready) {
        simcamerafeed::TryInit();
        return;
    }

    const uintptr_t controlObjPtr = weaponanchor::g_entityBase - 0x10;
    float pos[3] = {};
    if (!simcamerafeed::SafeCallGetWorldPos(
            simcamerafeed::g_getWorldPos, reinterpret_cast<void*>(controlObjPtr), pos)) {
        return;
    }

    static LONG64 lastLogMs = 0;
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    const bool ok = simcamerafeed::SafeCallSetSimCamera(
        simcamerafeed::g_setSimCam, reinterpret_cast<void*>(simcamerafeed::g_level), pos, pos);
    if (now - lastLogMs > 3000) {
        lastLogMs = now;
        LOG("simcamerafeed: SetSimulationCamera(pos=%.2f,%.2f,%.2f) %s",
            pos[0], pos[1], pos[2], ok ? "ok" : "FAULTED");
    }
}

void TryDisablePhysicsDebugRender()
{
    if (!DiagKillPhysicsDebugRender) {
        return;
    }
    debugrenderkill::EnforceDebugRendererDisabled();
    if (debugrenderkill::g_done && debugrenderkill::g_debugRendererDisabled) {
        return;
    }
    if (InterlockedCompareExchange(&debugrenderkill::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, debugrenderkill::KillThread, nullptr, 0, nullptr);
    }
}

// >>> THE THREE SILENT RETURNS BELOW COST THE 15:47 HEADSET RUN. <<<
//
// That run logged 320 LevelCamera rejects - so this function was called
// hundreds of times - and weaponanchor emitted NOTHING, in any of the three
// logs. Every exit path inside ScanThread itself logs, so the thread either
// never started or died before its first statement, and THE LOG COULD NOT SAY
// WHICH OF THE THREE EXITS FIRED. That is RULE 0's "absence of output is not
// information" in its purest form: three unlabelled early returns on the
// critical path, and a run that cannot distinguish them.
//
// Each exit now names itself ONCE (and then every 30 s, so a state that
// persists is visibly still persisting rather than scrolling away). The
// CreateThread result is checked and reported too - it was previously
// discarded entirely, so a failed thread creation was indistinguishable from
// a thread that started and said nothing.
namespace {
// Rate-limited one-line "why did the chain stop here" reporter. Keyed by a
// caller-supplied slot so each exit has its own budget.
void ChainNote(int slot, const char* what)
{
    constexpr int kSlots = 4;
    static LONG64 lastMs[kSlots] = {};
    static bool   said[kSlots]   = {};
    if (slot < 0 || slot >= kSlots) { return; }
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    if (said[slot] && now - lastMs[slot] < 30000) { return; }
    said[slot]   = true;
    lastMs[slot] = now;
    LOG("weaponanchor: [CHAIN] STOPPED - %s", what);
}
}  // namespace

void FindPlayerMoveStyleViaWeaponAnchor()
{
    if (!DiagPlayerMovementModule) {
        ChainNote(0, "diag_player_movemod is OFF at runtime. The config: dump is "
                     "NOT the authority here - this is the variable itself, read "
                     "at the call site.");
        return;
    }

    if (!weaponanchor::g_havePlayerEntity) {
        if (InterlockedCompareExchange(&weaponanchor::g_started, 1, 0) == 0) {
            const HANDLE th = CreateThread(nullptr, 0, weaponanchor::ScanThread,
                                           nullptr, 0, nullptr);
            if (!th) {
                LOG("weaponanchor: [CHAIN] CreateThread(ScanThread) FAILED, "
                    "GetLastError=%lu. The scan will never run and nothing "
                    "downstream of it can ever publish.", GetLastError());
            } else {
                LOG("weaponanchor: [CHAIN] ScanThread created (handle %p). If no "
                    "further 'weaponanchor:' line appears, it died before its "
                    "first statement.", th);
                CloseHandle(th);
            }
        }
        ChainNote(1, "no player entity yet - waiting on ScanThread. This is the "
                     "NORMAL state for the first few seconds in a level; if it "
                     "persists, read the ScanThread line above it.");
        return;
    }

    static volatile LONG triedFitness = 0;
    if (!weaponanchor::g_haveFitnessModule &&
        InterlockedCompareExchange(&triedFitness, 1, 0) == 0) {
        weaponanchor::FindFitnessModuleOnce();
    }

    // Cheap now that the entity/module pointers and field offsets are all
    // known: two dword reads per call, and logging only on a sprint-toggle
    // edge, a real field change, or a slow heartbeat (so the log shows this
    // is actually still being read even between toggles).
    static uint32_t lastMoveStyle = 0xFFFFFFFFu;
    static uint32_t lastCurrent = 0xFFFFFFFFu;
    static bool havePrevToggle = false;
    static bool prevToggle = false;
    static LONG64 lastHeartbeatMs = 0;

    uint32_t moveStyle = 0, current = 0;
    bool ok = false;
    __try {
        moveStyle = *reinterpret_cast<volatile uint32_t*>(weaponanchor::g_movModPtr +
                                                            weaponanchor::kMoveStyleOff);
        current = *reinterpret_cast<volatile uint32_t*>(weaponanchor::g_movModPtr +
                                                          weaponanchor::kCurrentMoveStyleOff);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!ok) {
        ChainNote(2, "the movement-module read FAULTED. g_movModPtr is set but "
                     "reading m_MoveStyle/m_CurrentMoveStyle through it threw - "
                     "the pointer is stale or the offsets are wrong.");
        return;
    }

    // >>> STALENESS GATE, 2026-09-15. THE OFFSETS ARE NOT THE PROBLEM. <<<
    // sub_1802DA270 is MovementModule's registrar and it declares m_MoveStyle at
    // +0x154 and m_CurrentMoveStyle at +0x184 - exactly the constants above, so
    // they are CONFIRMED CORRECT and must not be "fixed". What the 16:48 run
    // actually showed was m_CurrentMoveStyle = 3181222240 = 0xBD9D9060, a float
    // (-0.077) sitting in an enum field. A module whose memory now holds floats
    // has been freed and recycled. Re-check the vtable every read: if it no
    // longer points at MovementModule, the pointer is dead and EVERYTHING
    // DOWNSTREAM OF g_entityBase IS ALSO SUSPECT - including the entity list the
    // hit-count poll is built from.
    {
        static volatile LONG saidStale = 0;
        uintptr_t vtNow = 0;
        // >>> RESOLVED ONCE. CALLING THIS PER FRAME COST THE 17:04 RUN. <<<
        // aimhook.cpp:334 already documents why, and I reintroduced the exact
        // bug it warns about: "RttiVtableOf() itself is not cached ... calling
        // it directly, every scan, re-walks RTTI across both
        // engine_x64_rwdi.dll and gamedll_x64_rwdi.dll from scratch every
        // time." This function runs EVERY FRAME from LevelCamera, so that was
        // a full two-DLL RTTI walk per frame - the severe frame drop Jay saw on
        // load, twice. The vtable address cannot change within a process.
        static uintptr_t s_movModVtCached = 0;
        if (!s_movModVtCached) {
            s_movModVtCached = dlvr::RttiVtableOf("MovementModule");
        }
        const uintptr_t vtWant = s_movModVtCached;
        if (vtWant && (!weaponanchor::ReadPtr(weaponanchor::g_movModPtr, vtNow) || vtNow != vtWant)) {
            if (InterlockedCompareExchange(&saidStale, 1, 0) == 0) {
                LOG("weaponanchor: *** STALE MODULE *** g_movModPtr=%p no longer "
                    "points at a MovementModule (vtable %p, expected %p). The "
                    "captured player entity has been freed or recycled, so "
                    "g_entityBase is DEAD and every entity captured alongside it "
                    "is suspect - including the hit-count poll's list. Offsets "
                    "+0x154/+0x184 are CONFIRMED CORRECT from sub_1802DA270; this "
                    "is a LIFETIME fault, not an offset fault.",
                    reinterpret_cast<void*>(weaponanchor::g_movModPtr),
                    reinterpret_cast<void*>(vtNow),
                    reinterpret_cast<void*>(vtWant));
            }
            InterlockedExchange(&weaponanchor::g_recaptureReq, 1);
            return;
        }
    }

    const bool toggleNow = IsPlayerSprintingToggle();
    const bool isEdge = havePrevToggle && toggleNow != prevToggle;
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    const bool changed = moveStyle != lastMoveStyle || current != lastCurrent;
    const bool heartbeatDue = now - lastHeartbeatMs > 3000;

    if (isEdge || changed || heartbeatDue) {
        LOG("weaponanchor: m_MoveStyle=%u m_CurrentMoveStyle=%u (sprintToggle=%d%s)",
            moveStyle, current, toggleNow ? 1 : 0, isEdge ? ", EDGE" : "");
        lastHeartbeatMs = now;
    }
    lastMoveStyle = moveStyle;
    lastCurrent = current;

    // m_MoveStyle/m_CurrentMoveStyle (the two offsets found via static
    // analysis months ago) stayed at a flat 0 through a real test session
    // that included genuine sprint-toggle edges - a real negative for
    // those two specific offsets, not a broken read (the object itself is
    // now confirmed correct via live position correlation, unlike every
    // earlier candidate this investigation tried). Since the object is
    // finally known-good, the edge-triggered full-object byte-diff
    // technique built earlier this session (then abandoned along with the
    // maybe-wrong candidate it was scanning) is worth re-applying here,
    // where it should actually work: snapshot the whole object every call
    // (one cheap guarded bulk copy, not per-byte reads) and log any byte
    // that changed only at a real sprint-toggle edge - a clean, precisely-
    // timed before/after pair with one variable moving, the same
    // methodology proven earlier, just finally pointed at the right
    // object.
    constexpr size_t kMovModSize = 0x5A8;  // confirmed sizeof(MovementModule)
    static uint8_t prevSnapshot[kMovModSize] = {};
    static bool haveSnapshot = false;
    uint8_t snapshot[kMovModSize];
    bool snapOk = false;
    __try {
        memcpy(snapshot, reinterpret_cast<const void*>(weaponanchor::g_movModPtr), kMovModSize);
        snapOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (snapOk) {
        if (isEdge && haveSnapshot) {
            LOG("weaponanchor: [byte-diff @ EDGE %d->%d] scanning confirmed MovementModule "
                "for changed bytes...",
                prevToggle ? 1 : 0, toggleNow ? 1 : 0);
            int diffCount = 0;
            for (size_t i = 0; i < kMovModSize; ++i) {
                if (snapshot[i] != prevSnapshot[i]) {
                    const bool enumLike = snapshot[i] <= 20 && prevSnapshot[i] <= 20;
                    LOG("weaponanchor:   +0x%zx changed %u -> %u%s", i, prevSnapshot[i],
                        snapshot[i], enumLike ? " (enum candidate)" : "");
                    ++diffCount;
                }
            }
            LOG("weaponanchor: [byte-diff] %d byte(s) changed at this edge", diffCount);
        }
        memcpy(prevSnapshot, snapshot, kMovModSize);
        haveSnapshot = true;
    }

    // Same edge-triggered byte-diff, applied to PlayerFitnessModule too if
    // FindFitnessModuleOnce() found it - a second, independent shot at the
    // real sprint flag, on the class this project's own earlier (pre-
    // tonight) investigation actually suspected in the first place. Small
    // object (malloc(0x28) = 40 bytes, confirmed via the same FUN_180be7000
    // decompile), so this is cheap regardless of how often it runs.
    if (weaponanchor::g_haveFitnessModule) {
        constexpr size_t kFitnessModSize = 0x28;
        static uint8_t prevFitnessSnapshot[kFitnessModSize] = {};
        static bool haveFitnessSnapshot = false;
        uint8_t fitnessSnapshot[kFitnessModSize];
        bool fitnessSnapOk = false;
        __try {
            memcpy(fitnessSnapshot,
                   reinterpret_cast<const void*>(weaponanchor::g_fitnessModPtr),
                   kFitnessModSize);
            fitnessSnapOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (fitnessSnapOk) {
            if (isEdge && haveFitnessSnapshot) {
                LOG("weaponanchor: [fitness byte-diff @ EDGE %d->%d] scanning confirmed "
                    "PlayerFitnessModule for changed bytes...",
                    prevToggle ? 1 : 0, toggleNow ? 1 : 0);
                int fitnessDiffCount = 0;
                for (size_t i = 0; i < kFitnessModSize; ++i) {
                    if (fitnessSnapshot[i] != prevFitnessSnapshot[i]) {
                        const bool enumLike = fitnessSnapshot[i] <= 20 && prevFitnessSnapshot[i] <= 20;
                        LOG("weaponanchor:   [fitness] +0x%zx changed %u -> %u%s", i,
                            prevFitnessSnapshot[i], fitnessSnapshot[i],
                            enumLike ? " (enum candidate)" : "");
                        ++fitnessDiffCount;
                    }
                }
                LOG("weaponanchor: [fitness byte-diff] %d byte(s) changed at this edge",
                    fitnessDiffCount);
            }
            memcpy(prevFitnessSnapshot, fitnessSnapshot, kFitnessModSize);
            haveFitnessSnapshot = true;
        }
    }

    prevToggle = toggleNow;
    havePrevToggle = true;
}

// The "Look back" ease timer/gate, 2026-09-03. The game's own Y-bound action
// never actually turns the camera (confirmed this session - see CLAUDE.md),
// so this is a synthetic override, gated on all three of: Y physically held,
// also sprinting (the left stick click we already inject for vr_sprint -
// free, no new lookup needed), and not in a vehicle (IsPlayerInVehicle()
// above). Returns how much of a 180-degree turn to apply right now, 0..1,
// ramped at a constant rate in real time regardless of how often this is
// called per frame (driven by wall-clock delta, not a fixed per-call step,
// so multiple tracked cameras - e.g. both eyes - calling this within the
// same frame do not make it ease faster than intended) - eases up while the
// gate is satisfied and back down the instant any part of it stops being
// true, at the same rate either way, per the user's explicit ask.
float LookBackAmount()
{
    // A real test showed two simultaneously-tracked cameras (almost
    // certainly the two stereo eyes) disagreeing on how far they had
    // actually turned - a clean 140 degrees for one, roughly 3x that for
    // the other - despite both starting from a provably identical baseline
    // yaw. That is the signature of a race, not a maths error: this whole
    // hook is known to run from more than one engine thread (this project
    // has hit that before - see g_auto's own InterlockedCompareExchange64
    // slot-claiming above), and `t`/`lastMs` here were only `volatile`,
    // which stops the compiler reordering things but does nothing to make
    // a multi-step read-modify-write atomic between threads. Two threads
    // computing their own delta against the same stale `t` and both adding
    // their own increment is exactly how one eye could end up with several
    // times the intended total rotation. Fixed with a tiny spinlock around
    // the whole read-modify-write - the critical section is a handful of
    // float operations and a couple of cheap function calls, never I/O or
    // an allocation, so any real contention resolves in a few spins.
    static volatile LONG spinlock = 0;
    while (InterlockedCompareExchange(&spinlock, 1, 0) != 0) {
        // spin - the critical section below is short enough that a proper
        // wait primitive would cost more than it saves
    }

    static float t = 0.0f;
    static LONG64 lastMs = 0;

    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    LONG64 last = lastMs;
    if (last == 0) {
        last = now;
    }
    float dtMs = static_cast<float>(now - last);
    if (dtMs < 0.0f) {
        dtMs = 0.0f;
    } else if (dtMs > 100.0f) {
        // A real gap (menu open, hitch) should not be read back as one huge
        // instantaneous step - clamp it the same way the head-turn work
        // earlier this session clamped its own dt for the same reason.
        dtMs = 100.0f;
    }
    lastMs = now;

    const bool sprinting = IsPlayerSprintingToggle();
    const bool yHeld = IsYHeld();
    const bool inVehicle = IsPlayerInVehicle();
    const bool active = yHeld && sprinting && !inVehicle;

    // Edge-triggered logging turned out to hide the real question: a real
    // test came back with zero log lines at all, which only proves the
    // combined gate never once turned true - it says nothing about which of
    // the three conditions was the reason, since a silent "always false"
    // produces exactly the same nothing an edge-only log would show for
    // "never even reached." Logging unconditionally, throttled, so a test
    // where nothing visibly happens still shows all three individual reads.
    static LONG64 lastLogMs = 0;
    if (now - lastLogMs >= 500) {
        lastLogMs = now;
        LOG_PERIODIC("camhook: [look back] yHeld=%d sprinting=%d inVehicle=%d -> active=%d "
            "t=%.2f",
            yHeld ? 1 : 0, sprinting ? 1 : 0, inVehicle ? 1 : 0, active ? 1 : 0, t);
    }

    const float easeMs = CamHookLookBackEaseMs > 1.0f ? CamHookLookBackEaseMs : 1.0f;
    const float rate = dtMs / easeMs;
    float cur = t;
    cur = active ? (cur + rate) : (cur - rate);
    cur = cur < 0.0f ? 0.0f : (cur > 1.0f ? 1.0f : cur);
    t = cur;

    InterlockedExchange(&spinlock, 0);
    return cur;
}

// Quaternion sign continuity, 2026-09-03: q and -q represent the identical
// rotation, but nothing about the maths so far guarantees which sign comes
// out frame to frame - normally invisible, because MatFromQuat is sign
// invariant (every term is a product of two components, so negating all
// four leaves the matrix unchanged) and nothing else has ever cared. The
// Look back feature made this visible for the first time: a slow (900ms)
// ease showed a real full extra spin before settling at the target angle -
// the textbook symptom of something downstream picking the "long way
// around" between two frames' quaternions because one of them silently
// flipped sign relative to the last, most plausibly the engine's own
// frame-to-frame smoothing (this project already found it re-uses a
// previous frame's head/camera data in some circumstances - see
// g_staleHead). Forcing continuity with the previous frame's own output
// (negate if the dot product goes negative) is the standard, always-safe
// fix for this class of bug - a no-op whenever there was no discontinuity,
// and only changes anything on the exact frames that would otherwise flip.
// Tracked per tracked-camera-object (keyed by address) so eyes/other
// simultaneously-tracked cameras cannot cross-contaminate each other's
// history.
void MakeQuatContinuous(uintptr_t obj, float q[4])
{
    struct Prev {
        volatile uintptr_t obj;
        float q[4];
    };
    static Prev prev[16] = {};
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (prev[i].obj == obj) {
            slot = i;
            break;
        }
        if (slot < 0 && prev[i].obj == 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;  // table full (>16 simultaneously tracked cameras) - fall
                   // back to the first slot rather than skip the fix
                   // entirely; a rare wrong-neighbour pick here is still
                   // strictly better than never checking at all.
    }
    if (prev[slot].obj == obj) {
        const float dot = q[0] * prev[slot].q[0] + q[1] * prev[slot].q[1] +
                           q[2] * prev[slot].q[2] + q[3] * prev[slot].q[3];
        if (dot < 0.0f) {
            q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3];
            if (DiagCamhookVerbose) LOG("camhook: [continuity] obj=%p flipped sign to stay near the "
                "previous frame's orientation (dot was %.3f)",
                reinterpret_cast<void*>(obj), dot);
        }
    } else {
        prev[slot].obj = obj;
    }
    prev[slot].q[0] = q[0]; prev[slot].q[1] = q[1];
    prev[slot].q[2] = q[2]; prev[slot].q[3] = q[3];
}

// Look back, 2026-09-03: found the real cause of the reported "genuine
// spin," and it is not the earlier "one tracked object is independently
// spinning" theory (that fix - reject an object whose baseline yaw moves
// too fast - did not resolve the symptom on retest). Directly compared the
// confirmed real render-eye objects (identified by hit-rate: hit roughly
// twice as often per second as every other tracked object, present since
// before the session's first frame) with the feature ON versus OFF, same
// hold, same zero stick input: OFF, their raw yaw (read fresh from the
// incoming matrix, before any of this code touches it) stayed flat to
// within under a degree for several seconds. ON, the SAME objects' raw
// yaw swept by roughly the configured look-back angle, tracking almost
// exactly alongside this feature's own ease ramp.
//
// That is the signature of this composition's own output leaking back
// into what a later call receives as "current" input for the same logical
// camera - plausibly a multi-stage render pipeline where a later
// CCamera::operator= copy's source is an earlier stage this hook already
// wrote a correction into, so a second full rotation gets stacked on top
// of the first within the same frame. Re-reading "current" yaw fresh every
// call and using it as the rotation's own base is unsafe whenever that can
// happen - there is no way to tell a genuinely fresh, uncorrected sample
// apart from an already-corrected one just by looking at it.
//
// Fix: capture the baseline yaw once, at the instant this object's gate
// first goes active (t rises from 0), and hold that fixed for the entire
// duration of the hold - composing the look-back rotation from this stored
// value instead of whatever this frame's live yaw reads as. Immune to
// feedback by construction, since after the first frame nothing here reads
// this object's own incoming yaw again until the gate fully releases.
// Trades away letting the visible "backward" direction follow the
// character if the player also turns mid-hold (never independently
// confirmed working in the first place) for eliminating the spin outright.
float LookBackCapturedBaseline(uintptr_t obj, float currentBaseYawDeg, bool gateActive)
{
    struct Cap {
        volatile uintptr_t obj;
        float yaw;
        bool captured;
    };
    static Cap cap[8] = {};
    int slot = -1;
    for (int i = 0; i < 8; ++i) {
        if (cap[i].obj == obj) {
            slot = i;
            break;
        }
        if (slot < 0 && cap[i].obj == 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;  // table full - see MakeQuatContinuous's own comment on
                   // this same fallback.
    }
    if (cap[slot].obj != obj) {
        cap[slot].obj = obj;
        cap[slot].captured = false;
    }
    if (!gateActive) {
        // Fully released - forget the capture so the next engagement
        // starts fresh from wherever the character is facing then.
        cap[slot].captured = false;
        return currentBaseYawDeg;
    }
    if (!cap[slot].captured) {
        cap[slot].yaw = currentBaseYawDeg;
        cap[slot].captured = true;
    }
    return cap[slot].yaw;
}
}  // namespace

// Phase 1 read-only probe of ILevel::GetObjectsVisibilityRange - see the
// visibilityprobe namespace comment above for the reasoning and for what each
// outcome (VR vs flatscreen values differing or matching) actually proves.
// Deliberately at dlvr scope rather than in the anonymous namespace: it is
// called from dlvr_FilterScePad in ovr2xr.cpp, because scePadReadState is
// polled in BOTH VR and flatscreen whereas the camera hook only ever installs
// in VR - and a probe whose entire purpose is comparing the two is useless if
// it can only run in one of them.
void ProbeVisibilityRangeOnce()
{
    if (!DiagVisibilityProbe) {
        return;
    }
    if (InterlockedCompareExchange(&visibilityprobe::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, visibilityprobe::ProbeThread, nullptr, 0, nullptr);
    }
}

// ---- LOD / visibility override (2026-09-05) ---------------------------------
//
// The problem, as finally characterised: distant objects sit on their lowest
// LOD mesh, small ground clutter collapses to slivers, and NPCs at range do
// not render at all until approached - all reversible with distance. The same
// behaviour is present in flatscreen, so this is NOT a VR-specific bug; it is
// the engine's ordinary 2015-era LOD tuning, which is merely tolerable when
// glanced at on a monitor and unacceptable when you are standing in it. Ruled
// out along the way: the camera hook (a horizon_lock=0 run changed nothing,
// with zero camhook lines proving it never installed), frustum width
// (fov_source=0 narrowed +-54 -> +-45.8 deg with no effect), and the texture
// streaming budget (already 2000 MB at runtime, not the 600 MB in
// r_device.scr, so never the starved value assumed).
//
// So the fix is tuning, not repair. engine_x64_rwdi.dll exports the knobs as
// plain non-virtual ILevel methods (confirmed via dumpbin), callable directly
// with a live ILevel* exactly like IGSObject::GetLevel and
// ILevel::SetPhysicsDebugRenderMode already are elsewhere in this file:
//     ?GetObjectsVisibilityRange@ILevel@@QEBAMXZ    float get   (reads 1700)
//     ?SetObjectsVisibilityRange@ILevel@@QEAAXM@Z   void  set(float)
//     ?SetMeshCullSize@ILevel@@QEAAXM@Z             void  set(float)
//     ?SetMeshCullDistTreshold@ILevel@@QEAAXM@Z     void  set(float)  [sic]
//     ?SetFXVisibilityRange@ILevel@@QEAAXM@Z        void  set(float)
//
// Applied REPEATEDLY (about once a second) rather than once, for two reasons:
// these are per-ILevel so a level load resets them, and re-applying means the
// overlay sliders take effect live - you can stand in front of the NPC that
// will not render and dial the value until it does, with no rebuild.
//
// Every value defaults to a sentinel meaning "leave alone". Only
// SetObjectsVisibilityRange has a matching getter, so it is the only one whose
// current value is known; the others are deliberately not touched until
// someone chooses a number, rather than clobbering them with a guess.
namespace lodoverride {

typedef float (*GetRangeFn)(void* levelThis);
typedef void (*SetFloatFn)(void* levelThis, float v);

volatile LONG g_started = 0;
void* g_level = nullptr;
GetRangeFn g_getObjRange = nullptr;
SetFloatFn g_setObjRange = nullptr;
SetFloatFn g_setMeshCullSize = nullptr;
SetFloatFn g_setMeshCullDist = nullptr;
SetFloatFn g_setFxRange = nullptr;

bool SafeSetFloat(SetFloatFn fn, void* level, float v)
{
    bool ok = false;
    __try {
        fn(level, v);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeGetFloat(GetRangeFn fn, void* level, float& out)
{
    bool ok = false;
    __try {
        out = fn(level);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// >>> LEVEL CHANGE, 2026-10-16. <<< g_level used to be acquired ONCE and never re-checked, and each value was sent only
// when it changed. Jay, 2026-10-15: Slums -> Old Town in one launch (game log 22:20:50 / 22:27:24), the far buildings
// vanished again and nothing brought them back - old_town.scr (DW\Data2.pak) sets f_engine_mesh_cull_size 12.0 into
// the NEW level, and the worker never wrote it again. Worse, a slider moved in Old Town wrote into the destroyed Slums
// level (a 0xC0000005 followed at the quit, 22:30:06 - 0-byte dump, not attributed).
// The game's level array (INV-528 (A), our engine e9fbfc2c): IGame* at engine
// +A3F3B8, its +8 the engine game object, levels at +224 (count u32 +232), each level's game-side ILevel at +168
// whose +8 points back. IGame::GetActiveLevel (+1648) is NOT used: read from another thread it flickers between
// LevelDI, LogicalLevel, MainMenuModuleDI and null (backups/20261011_uicam_run2: 72 transitions in 91 lines).
// That run's LevelDI game-side object 000001C2CF35B380 == the ILevel lodoverride acquired (one run).
// PLAIN READS ONLY: a level can be freed between two reads; a read fault is caught, a call into a freed object is not.
constexpr uintptr_t kGameSlot     = 0xA3F3B8;
constexpr size_t    kGameImpl     = 8;
constexpr size_t    kGameLevels   = 224;
constexpr size_t    kGameLevelCnt = 232;
constexpr size_t    kLevelILevel  = 168;

// false = the array could not be read. present = 'cur' is listed and its back pointer verifies; matches = listed
// levels whose game-side object carries vtable 'vt' (the acquired level's own class), 'match' = the last of them.
bool ScanLevels(uintptr_t eng, void* cur, uintptr_t vt, bool& present, int& matches, void*& match)
{
    present = false;
    matches = 0;
    match = nullptr;
    __try {
        const uintptr_t game = *reinterpret_cast<const uintptr_t*>(eng + EngRva(kGameSlot));
        if (!game) { return false; }
        const uintptr_t impl = *reinterpret_cast<const uintptr_t*>(game + kGameImpl);
        if (!impl) { return false; }
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(impl + kGameLevels);
        const unsigned  n   = *reinterpret_cast<const unsigned*>(impl + kGameLevelCnt);
        if (n > 256 || (n && !arr)) { return false; }
        for (unsigned i = 0; i < n; ++i) {
            const uintptr_t lvl = *reinterpret_cast<const uintptr_t*>(arr + 8ull * i);
            if (!lvl) { continue; }
            const uintptr_t il = *reinterpret_cast<const uintptr_t*>(lvl + kLevelILevel);
            if (!il || *reinterpret_cast<const uintptr_t*>(il + 8) != lvl) { continue; }
            if (reinterpret_cast<void*>(il) == cur) { present = true; }
            if (vt && *reinterpret_cast<const uintptr_t*>(il) == vt) {
                ++matches;
                match = reinterpret_cast<void*>(il);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

DWORD WINAPI Thread(LPVOID)
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!engine || !gamedll) {
        LOG("lodoverride: engine/gamedll not mapped");
        return 0;
    }
    auto getLevel = reinterpret_cast<debugrenderkill::GetLevelFn>(
        GetProcAddress(engine, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ"));
    g_getObjRange = reinterpret_cast<GetRangeFn>(
        GetProcAddress(engine, "?GetObjectsVisibilityRange@ILevel@@QEBAMXZ"));
    g_setObjRange = reinterpret_cast<SetFloatFn>(
        GetProcAddress(engine, "?SetObjectsVisibilityRange@ILevel@@QEAAXM@Z"));
    g_setMeshCullSize = reinterpret_cast<SetFloatFn>(
        GetProcAddress(engine, "?SetMeshCullSize@ILevel@@QEAAXM@Z"));
    g_setMeshCullDist = reinterpret_cast<SetFloatFn>(
        GetProcAddress(engine, "?SetMeshCullDistTreshold@ILevel@@QEAAXM@Z"));
    g_setFxRange = reinterpret_cast<SetFloatFn>(
        GetProcAddress(engine, "?SetFXVisibilityRange@ILevel@@QEAAXM@Z"));
    if (!getLevel || !g_setObjRange) {
        LOG("lodoverride: could not resolve the required exports "
            "(getLevel=%p setObjRange=%p)",
            reinterpret_cast<void*>(getLevel), reinterpret_cast<void*>(g_setObjRange));
        return 0;
    }
    LOG("lodoverride: resolved setters - objRange=%p meshCullSize=%p "
        "meshCullDist=%p fxRange=%p",
        reinterpret_cast<void*>(g_setObjRange), reinterpret_cast<void*>(g_setMeshCullSize),
        reinterpret_cast<void*>(g_setMeshCullDist), reinterpret_cast<void*>(g_setFxRange));

    float lastObj = -1.0f, lastCullSize = -1.0f, lastCullDist = -1.0f, lastFx = -1.0f;
    const uintptr_t engBase = reinterpret_cast<uintptr_t>(engine);
    uintptr_t levelVt = 0;              // the level's class (LevelDI's primary vtable)
    bool scanLogged = false, lastScanned = false, lastPresent = false;
    int lastMatches = -1;
    // B8 (audit S07 7d-1, Jay 2026-11-15 "Yes"; trace 20261115_1430_b8_lodoverride_level_list_acquire): the FIRST level comes from
    // the game's level list, by class - no exec watch. The old route armed exec client 0 at gamedll+179FF0, which boneprobe also
    // uses: each kicked the other off, and the level was only ever caught from boneprobe's CorrectItemPosition hits (RUN B5:
    // 46 s late, after a weapon draw). 465 archived acquires were all the ONE listed LevelDI (gamedll+14A8528 on Steam, OUR gamedll
    // 97046bba; another rva on Epic - so it is found by NAME). COST: one RTTI scan of both module images, once, on this thread.
    const uintptr_t levelDiVt = dlvr::RttiVtableOf("LevelDI");
    if (levelDiVt) {
        levelVt = levelDiVt;
        // [PROBE: LODOVERRIDE] gate=lod_override  site=FLAT  log-only=yes
        LOG("lodoverride: LevelDI vtable %p (gamedll+%zX) - the first level is taken from the game's level list, no exec watch",
            reinterpret_cast<void*>(levelDiVt), static_cast<size_t>(levelDiVt - reinterpret_cast<uintptr_t>(gamedll)));
    } else {
        // [PROBE: LODOVERRIDE] gate=lod_override  site=FLAT  log-only=yes
        LOG("lodoverride: *** RTTI has no LevelDI vtable (another game build?) - FALLBACK to the exec-watch acquire at "
            "gamedll+%zX (shares exec client 0 with boneprobe) ***", static_cast<size_t>(weaponanchor::kModulesLoopFnRva));
    }
    ULONGLONG listWaitSince = 0;

    for (;;) {
        if (!LodOverride) {
            Sleep(1000);
            continue;
        }
        // Acquire the first ILevel - from the level list (B8), or by WatchExec only when RTTI found no LevelDI. A later level
        // change is caught by the level-list check below, with no debug register.
        if (!g_level && levelDiVt) {
            bool present = false;
            int matches = 0;
            void* match = nullptr;
            const bool scanned = ScanLevels(engBase, nullptr, levelDiVt, present, matches, match);
            if (!scanned || matches != 1) {
                // Loading / main menu / a transition with two listed: nothing written yet. One line when the wait starts.
                if (!listWaitSince) {
                    listWaitSince = GetTickCount64();
                    // [PROBE: LODOVERRIDE] gate=lod_override  site=FLAT  log-only=yes
                    LOG("lodoverride: waiting for exactly one listed LevelDI (list %s, %d listed) - checked every 1 s",
                        scanned ? "read" : "NOT READ", matches);
                }
                Sleep(1000);
                continue;
            }
            g_level = match;
            float cur = -1.0f;
            SafeGetFloat(g_getObjRange, g_level, cur);
            // [PROBE: LODOVERRIDE] gate=lod_override  site=FLAT  log-only=yes
            LOG("lodoverride: acquired ILevel=%p from the level list (the one listed LevelDI) after %llu ms waiting, current "
                "GetObjectsVisibilityRange()=%.1f", g_level,
                listWaitSince ? static_cast<unsigned long long>(GetTickCount64() - listWaitSince) : 0ull, cur);
        }
        if (!g_level) {
            const uintptr_t addr =
                reinterpret_cast<uintptr_t>(gamedll) + weaponanchor::kModulesLoopFnRva;
            dlvr::WatchExecSetStackFloatOffset(0);
            dlvr::WatchExecSetStackUseRsp(false);
            if (dlvr::WatchExecStart(addr)) {
                Sleep(1500);
                dlvr::WatchExecStop();
                dlvr::ExecHit hits[16] = {};
                const int n = dlvr::WatchExecGet(hits, 16);
                for (int i = 0; i < n && !g_level; ++i) {
                    void* obj = reinterpret_cast<void*>(hits[i].rcxValue);
                    void* lvl = nullptr;
                    if (obj && debugrenderkill::SafeCallGetLevel(getLevel, obj, lvl) && lvl) {
                        g_level = lvl;
                        float cur = -1.0f;
                        SafeGetFloat(g_getObjRange, lvl, cur);
                        LOG("lodoverride: acquired ILevel=%p, current "
                            "GetObjectsVisibilityRange()=%.1f", lvl, cur);
                    }
                }
            }
            if (!g_level) {
                Sleep(4000);
                continue;
            }
            uintptr_t vt = 0;
            weaponanchor::ReadPtr(reinterpret_cast<uintptr_t>(g_level), vt);
            levelVt = vt;
        }

        // Is the acquired level still listed? Logged first time, then on every change of the result.
        {
            bool present = false;
            int matches = 0;
            void* match = nullptr;
            const bool scanned = ScanLevels(engBase, g_level, levelVt, present, matches, match);
            if (!scanLogged || scanned != lastScanned || present != lastPresent || matches != lastMatches) {
                if (scanned) {
                    LOG("lodoverride: level list - acquired ILevel=%p %s, %d listed level(s) of its class (vtable %p)",
                        g_level, present ? "LISTED" : "NOT LISTED", matches, reinterpret_cast<void*>(levelVt));
                } else {
                    LOG("lodoverride: level list NOT READ (game object at engine+A3F3B8 null, unreadable, or count "
                        "over 256) - level changes are NOT detected; writing to ILevel=%p as before", g_level);
                }
                scanLogged = true;
                lastScanned = scanned;
                lastPresent = present;
                lastMatches = matches;
            }
            if (scanned && !present) {
                if (matches == 1) {
                    LOG("lodoverride: LEVEL CHANGED - ILevel %p is no longer listed; adopting %p and re-applying "
                        "every value", g_level, match);
                    g_level = match;
                    lastObj = lastCullSize = lastCullDist = lastFx = -1.0f;
                } else {
                    // Nothing is written into a level that is no longer listed (loading, menu, or ambiguous).
                    Sleep(1000);
                    continue;
                }
            }
        }

        // Apply only what has actually been set, and only when it changes -
        // so the log stays readable while sliders are being dialled.
        if (LodObjectsRange > 0.0f && LodObjectsRange != lastObj) {
            if (SafeSetFloat(g_setObjRange, g_level, LodObjectsRange)) {
                float back = -1.0f;
                SafeGetFloat(g_getObjRange, g_level, back);
                LOG("lodoverride: SetObjectsVisibilityRange(%.1f) -> reads back %.1f",
                    LodObjectsRange, back);
                lastObj = LodObjectsRange;
            } else {
                LOG("lodoverride: SetObjectsVisibilityRange faulted - disabling");
                LodOverride = false;
            }
        }
        if (g_setMeshCullSize && LodMeshCullSize >= 0.0f && LodMeshCullSize != lastCullSize) {
            if (SafeSetFloat(g_setMeshCullSize, g_level, LodMeshCullSize)) {
                LOG("lodoverride: SetMeshCullSize(%.4f)", LodMeshCullSize);
                lastCullSize = LodMeshCullSize;
            }
        }
        if (g_setMeshCullDist && LodMeshCullDist >= 0.0f && LodMeshCullDist != lastCullDist) {
            if (SafeSetFloat(g_setMeshCullDist, g_level, LodMeshCullDist)) {
                LOG("lodoverride: SetMeshCullDistTreshold(%.1f)", LodMeshCullDist);
                lastCullDist = LodMeshCullDist;
            }
        }
        if (g_setFxRange && LodFxRange > 0.0f && LodFxRange != lastFx) {
            if (SafeSetFloat(g_setFxRange, g_level, LodFxRange)) {
                LOG("lodoverride: SetFXVisibilityRange(%.1f)", LodFxRange);
                lastFx = LodFxRange;
            }
        }
        Sleep(1000);
    }
}

}  // namespace lodoverride

// Starts the LOD override worker once per process. A cheap no-op while
// LodOverride is off, so it is safe to start unconditionally.
void StartLodOverrideOnce()
{
    if (InterlockedCompareExchange(&lodoverride::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, lodoverride::Thread, nullptr, 0, nullptr);
    }
}

// ---- RASTER-OCCLUSION A/B: ILevel::SetForceViewNoRastOcclusion (2026-10-03) ----
//
// QUESTION (Jay): far buildings/objects vanish at the CENTRE of the view and stay
// visible at the sides, head turn and right stick alike. Candidate, UNMEASURED:
// the engine's CPU raster-occlusion pass (INV-241 sub_1802E85F0).
//
// STATIC, OUR engine (md5 e9fbfc2c... == deployed), ghidra-project/ida/oc1_engine_out.txt:
//   ?SetForceViewNoRastOcclusion@ILevel@@ rva 29CA90 writes byte CLevelView+0x92290
//   (getter 29CAA0 reads it). sub_1802B4EF0 (the "Scene"/"FX" pass setup, INV-261)
//   passes view+0x900C0 (the raster-occlusion object) into the collection pass ONLY
//   while that byte is 0, else null (oc1 :400); sub_1805049B0 does the same (:1391).
//   The same function stores the current scene view: qword_180AADDA8 = a2 (oc1 :404).
//
// WHAT THIS DOES. A 250 ms worker - nothing on any per-frame path. While the setting
// is on, it reads the CURRENT scene view from engine+0xAADDA8 and sets its byte with
// the engine's own setter; while off, it restores the byte on any view it set, but
// ONLY when that view is the current one again. It never writes through a remembered
// pointer that is not current: a view freed by a level load would make that a write
// into freed heap, the U-49 damage class. Each target is VirtualQuery-checked first.
// The setter's RVA is checked against this build; a mismatch refuses, naming both.
namespace rastocc {

typedef void (*SetForceFn)(void* levelThis, void* view, bool v);
typedef bool (*GetForceFn)(void* levelThis, void* view);

constexpr uintptr_t kRvaSetter = 0x29CA90;
constexpr uintptr_t kRvaGetter = 0x29CAA0;
constexpr uintptr_t kRvaCurrentSceneView = 0xAADDA8;  // qword_180AADDA8
constexpr uintptr_t kViewForceNoRastOcc = 0x92290;    // byte
constexpr uintptr_t kViewRastOccObj = 0x900C0;        // qword, passed while byte == 0
constexpr int kMaxOwned = 8;
constexpr int kMaxViewLines = 20;

volatile LONG g_started = 0;

bool Writable(uintptr_t p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!p || VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    const DWORD rw = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & rw) || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return p + n <= end;
}

bool SafeReadPtr(uintptr_t addr, uintptr_t& out)
{
    bool ok = false;
    __try {
        out = *reinterpret_cast<volatile uintptr_t*>(addr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeGet(GetForceFn fn, uintptr_t view, bool& out)
{
    bool ok = false;
    __try {
        out = fn(nullptr, reinterpret_cast<void*>(view));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeSet(SetForceFn fn, uintptr_t view, bool v)
{
    bool ok = false;
    __try {
        fn(nullptr, reinterpret_cast<void*>(view), v);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

struct Owned {
    uintptr_t view;
    bool orig;
};

DWORD WINAPI Thread(LPVOID)
{
    // [PROBE: RASTOCC] gate=diag_no_rast_occlusion  site=FLAT  log-only=no
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("[RASTOCC] *** REFUSED: engine_x64_rwdi.dll not mapped - diag_no_rast_occlusion does nothing ***");
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    auto set = reinterpret_cast<SetForceFn>(GetProcAddress(
        engine, "?SetForceViewNoRastOcclusion@ILevel@@QEAAXPEAVCLevelView@@_N@Z"));
    auto get = reinterpret_cast<GetForceFn>(GetProcAddress(
        engine, "?GetForceViewNoRastOcclusion@ILevel@@QEAA_NPEAVCLevelView@@@Z"));
    const uintptr_t setRva = set ? reinterpret_cast<uintptr_t>(set) - base : 0;
    const uintptr_t getRva = get ? reinterpret_cast<uintptr_t>(get) - base : 0;
    LOG("[RASTOCC] worker up: setter=+%llX getter=+%llX (this build: +%llX / +%llX), "
        "scene view slot engine+0x%llX, diag_no_rast_occlusion=%d",
        static_cast<unsigned long long>(setRva), static_cast<unsigned long long>(getRva),
        static_cast<unsigned long long>(kRvaSetter), static_cast<unsigned long long>(kRvaGetter),
        static_cast<unsigned long long>(kRvaCurrentSceneView), DiagNoRastOcclusion ? 1 : 0);
    if (setRva != EngRva(kRvaSetter) || getRva != EngRva(kRvaGetter)) {
        LOG("[RASTOCC] *** REFUSED: export RVAs differ from engine e9fbfc2c - the view slot "
            "engine+0x%llX is not trusted on this build; diag_no_rast_occlusion does nothing ***",
            static_cast<unsigned long long>(kRvaCurrentSceneView));
        return 0;
    }

    Owned owned[kMaxOwned] = {};
    int nOwned = 0;
    uintptr_t lastSeen = 0;
    int viewLines = 0;
    unsigned viewChanges = 0;
    unsigned applies = 0;
    bool lastEnabled = false;
    bool refusedLogged = false;
    DWORD lastStatus = GetTickCount();

    for (;;) {
        Sleep(250);
        const bool en = DiagNoRastOcclusion;
        if (en != lastEnabled) {
            LOG("[RASTOCC] diag_no_rast_occlusion %d -> %d", lastEnabled ? 1 : 0, en ? 1 : 0);
            lastEnabled = en;
            refusedLogged = false;
        }

        uintptr_t cur = 0;
        if (!SafeReadPtr(base + EngRva(kRvaCurrentSceneView), cur)) {
            cur = 0;
        }
        if (cur != lastSeen) {
            ++viewChanges;
            if (viewLines < kMaxViewLines) {
                ++viewLines;
                LOG("[RASTOCC] scene view now %p (was %p), change #%u%s",
                    reinterpret_cast<void*>(cur), reinterpret_cast<void*>(lastSeen), viewChanges,
                    viewLines == kMaxViewLines ? " - further changes counted in the status line" : "");
            }
            lastSeen = cur;
        }

        const bool curOk = cur && Writable(cur + kViewForceNoRastOcc, 1)
                           && Writable(cur + kViewRastOccObj, sizeof(uintptr_t));
        int idx = -1;
        for (int i = 0; i < nOwned; ++i) {
            if (owned[i].view == cur) {
                idx = i;
            }
        }

        if (en) {
            if (!curOk) {
                if (!refusedLogged) {
                    LOG("[RASTOCC] NOT APPLIED: scene view %p at engine+0x%llX is %s",
                        reinterpret_cast<void*>(cur),
                        static_cast<unsigned long long>(kRvaCurrentSceneView),
                        cur ? "not committed read-write memory" : "null (no scene drawn yet)");
                    refusedLogged = true;
                }
            } else {
                bool now = false;
                if (SafeGet(get, cur, now) && !now) {
                    uintptr_t occ = 0;
                    SafeReadPtr(cur + kViewRastOccObj, occ);
                    bool back = false;
                    const bool setOk = SafeSet(set, cur, true) && SafeGet(get, cur, back);
                    ++applies;
                    if (idx < 0 && nOwned < kMaxOwned) {
                        owned[nOwned].view = cur;
                        owned[nOwned].orig = false;
                        ++nOwned;
                    }
                    LOG("[RASTOCC] APPLIED #%u on view %p: byte 0 -> %d%s (raster-occlusion obj "
                        "view+0x900C0 = %p, now not passed to the collection pass)",
                        applies, reinterpret_cast<void*>(cur), back ? 1 : 0,
                        setOk ? "" : " *** SET OR READ-BACK FAULTED ***", reinterpret_cast<void*>(occ));
                    refusedLogged = false;
                }
            }
        } else if (idx >= 0) {
            if (curOk && SafeSet(set, cur, owned[idx].orig)) {
                bool back = true;
                SafeGet(get, cur, back);
                LOG("[RASTOCC] RESTORED view %p: byte -> %d (orig %d)", reinterpret_cast<void*>(cur),
                    back ? 1 : 0, owned[idx].orig ? 1 : 0);
            } else {
                LOG("[RASTOCC] *** could not restore view %p (not writable or faulted) - dropped ***",
                    reinterpret_cast<void*>(cur));
            }
            owned[idx] = owned[--nOwned];
        }

        const DWORD nowMs = GetTickCount();
        if (en && nowMs - lastStatus >= 10000) {
            bool b = false;
            const bool rd = curOk && SafeGet(get, cur, b);
            LOG("[RASTOCC] status: on, view %p byte %s, applies %u, views owned %d, view changes %u",
                reinterpret_cast<void*>(cur), rd ? (b ? "1" : "0") : "UNREADABLE", applies, nOwned,
                viewChanges);
            lastStatus = nowMs;
        }
    }
}

}  // namespace rastocc

// ---- VR JITTER-EXECUTE A/B (2026-10-14, INV-561 (f)-(j)) ----
//
// CmdPPFX_SMAAScreenJitter has two entry points that disagree in VR. Its Execute (slot 1, engine+0x75DFE0) writes the
// render context's jitter flag (tail jmp to context slot 37) as enabled && !byte_180A401F9 && !COculusHelper::vt+8()
// (= helper+752, set when an Oculus session exists), while its job-snapshot slot (slot 2, +0x75E050) has no helper
// test. So in VR the worker job that EXECUTES jitter(true) draws unjittered and the jobs snapshotted after it draw
// jittered. This A/B removes the helper test from Execute only: the 3-byte `cmovnz ecx, edx` at +0x75E010 becomes a
// 3-byte NOP, ecx stays 0, and Execute writes the same value as the snapshot. Flat has helper+752 = 0 (INV-561 (j)),
// so flat behaviour is unchanged by construction.
//
// The write is ONE aligned 8-byte compare-exchange of +0x75E010..+0x75E017 (the instruction plus the next five
// bytes, unchanged), so no thread can fetch a torn instruction. The whole surrounding sequence is verified against
// engine e9fbfc2c before the first write; anything else refuses by name. Polled every 250 ms - no per-frame cost.
namespace jitvr {

constexpr uintptr_t kRvaWindow = 0x75E005;     // movzx ecx, sil ... (start of the verified window)
constexpr uintptr_t kRvaCas = 0x75E010;        // cmovnz ecx, edx - 8-byte aligned
constexpr uintptr_t kRvaTailJmp = 0x75E03D;    // jmp qword ptr [rax+128h]
constexpr uintptr_t kRvaHelperSlot = 0xA40290; // COculusHelper*
constexpr uintptr_t kHelperActive = 752;       // byte read by COculusHelper slot 1 (sub_1802E6E50)
// +75E005  40 0F B6 CE | BA 01 00 00 00 | 84 C0 | 0F 45 CA | 40 38 73 10 | 74 0D
const unsigned char kWindowOrig[] = {0x40, 0x0F, 0xB6, 0xCE, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x84, 0xC0,
                                     0x0F, 0x45, 0xCA, 0x40, 0x38, 0x73, 0x10, 0x74, 0x0D};
const unsigned char kTailJmp[] = {0x48, 0xFF, 0xA0, 0x28, 0x01, 0x00, 0x00};
const unsigned char kCasOrig[8] = {0x0F, 0x45, 0xCA, 0x40, 0x38, 0x73, 0x10, 0x74};
const unsigned char kCasPatched[8] = {0x0F, 0x1F, 0x00, 0x40, 0x38, 0x73, 0x10, 0x74};

volatile LONG g_started = 0;

bool SafeCopy(void* dst, uintptr_t src, size_t n)
{
    bool ok = false;
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// 0 = original, 1 = patched, -1 = neither / unreadable.
int CasState(uintptr_t base)
{
    unsigned char b[8] = {};
    if (!SafeCopy(b, base + EngRva(kRvaCas), 8)) { return -1; }
    if (memcmp(b, kCasOrig, 8) == 0) { return 0; }
    if (memcmp(b, kCasPatched, 8) == 0) { return 1; }
    return -1;
}

void Hex(char* out, size_t cap, uintptr_t p, size_t n)
{
    unsigned char b[32] = {};
    if (n > sizeof(b)) { n = sizeof(b); }
    if (!SafeCopy(b, p, n)) {
        snprintf(out, cap, "UNREADABLE ");
        return;
    }
    size_t k = 0;
    out[0] = 0;
    for (size_t i = 0; i < n && k + 4 < cap; ++i) {
        k += snprintf(out + k, cap - k, "%02X ", b[i]);
    }
}

// -1 = helper pointer null / unreadable, else the byte.
int HelperActive(uintptr_t base)
{
    uintptr_t helper = 0;
    if (!rastocc::SafeReadPtr(base + EngRva(kRvaHelperSlot), helper) || !helper) { return -1; }
    unsigned char v = 0;
    if (!SafeCopy(&v, helper + kHelperActive, 1)) { return -1; }
    return v;
}

// Swaps the aligned qword from one exact pattern to the other. True only when the read-back is the target.
bool Swap(uintptr_t base, const unsigned char* from, const unsigned char* to)
{
    void* p = reinterpret_cast<void*>(base + EngRva(kRvaCas));
    DWORD oldProt = 0;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &oldProt)) { return false; }
    LONG64 vFrom = 0;
    LONG64 vTo = 0;
    memcpy(&vFrom, from, 8);
    memcpy(&vTo, to, 8);
    bool ok = false;
    __try {
        ok = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), vTo, vFrom) == vFrom;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD tmp = 0;
    VirtualProtect(p, 8, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    unsigned char back[8] = {};
    return ok && SafeCopy(back, base + EngRva(kRvaCas), 8) && memcmp(back, to, 8) == 0;
}

DWORD WINAPI Thread(LPVOID)
{
    // [PROBE: JITVR] gate=diag_jitter_vr_exec  site=HEADSET  log-only=no
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("[JITVR] *** REFUSED: engine_x64_rwdi.dll not mapped - diag_jitter_vr_exec does nothing ***");
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    char win[100] = {};
    char tail[40] = {};
    Hex(win, sizeof(win), base + EngRva(kRvaWindow), sizeof(kWindowOrig));
    Hex(tail, sizeof(tail), base + EngRva(kRvaTailJmp), sizeof(kTailJmp));
    unsigned char w[sizeof(kWindowOrig)] = {};
    unsigned char t[sizeof(kTailJmp)] = {};
    const bool wRead = SafeCopy(w, base + EngRva(kRvaWindow), sizeof(w));
    const bool winOk = wRead && memcmp(w, kWindowOrig, sizeof(w)) == 0;
    const bool tailOk = SafeCopy(t, base + EngRva(kRvaTailJmp), sizeof(t)) && memcmp(t, kTailJmp, sizeof(t)) == 0;
    const int st0 = CasState(base);
    LOG("[JITVR] worker up: engine+0x%llX bytes %s(expect 40 0F B6 CE BA 01 00 00 00 84 C0 0F 45 CA 40 38 73 10 74 0D), "
        "tail engine+0x%llX %s(expect 48 FF A0 28 01 00 00), state %s, helper+752=%d, diag_jitter_vr_exec=%d",
        static_cast<unsigned long long>(kRvaWindow), win, static_cast<unsigned long long>(kRvaTailJmp), tail,
        st0 == 0 ? "ORIGINAL" : (st0 == 1 ? "ALREADY PATCHED" : "UNKNOWN"), HelperActive(base),
        DiagJitterVrExec ? 1 : 0);
    // A window already holding the NOP (left by an earlier worker in this process) is accepted when every other byte
    // matches the original.
    bool identity = tailOk && winOk;
    if (tailOk && wRead && !winOk && st0 == 1) {
        unsigned char exp[sizeof(kWindowOrig)];
        memcpy(exp, kWindowOrig, sizeof(exp));
        memcpy(exp + (kRvaCas - kRvaWindow), kCasPatched, 3);
        identity = memcmp(w, exp, sizeof(w)) == 0;
    }
    if (!identity) {
        LOG("[JITVR] *** REFUSED: the bytes at engine+0x%llX / +0x%llX are not CmdPPFX_SMAAScreenJitter::Execute of "
            "engine e9fbfc2c - diag_jitter_vr_exec does nothing on this build ***",
            static_cast<unsigned long long>(kRvaWindow), static_cast<unsigned long long>(kRvaTailJmp));
        return 0;
    }

    bool lastEnabled = false;
    bool applied = (st0 == 1);
    bool failLogged = false;
    unsigned swaps = 0;
    unsigned reverts = 0;
    bool unknownLogged = false;
    DWORD lastStatus = GetTickCount();
    for (;;) {
        Sleep(250);
        const bool en = DiagJitterVrExec;
        if (en != lastEnabled) {
            LOG("[JITVR] diag_jitter_vr_exec %d -> %d (helper+752=%d)", lastEnabled ? 1 : 0, en ? 1 : 0,
                HelperActive(base));
            lastEnabled = en;
            failLogged = false;
        }
        if (en != applied) {
            // [LOADBEARING: diag_jitter_vr_exec] without this swap VR textures / decals / shadows flash with engine jitter on  -- INV-562 (Jay 2026-10-14 A/B, log PATCHED #1..#7)
            const bool ok = en ? Swap(base, kCasOrig, kCasPatched) : Swap(base, kCasPatched, kCasOrig);
            char now[40] = {};
            Hex(now, sizeof(now), base + EngRva(kRvaCas), 8);
            if (ok) {
                applied = en;
                ++swaps;
                LOG("[JITVR] %s #%u: engine+0x%llX now %s(%s), helper+752=%d", en ? "PATCHED" : "RESTORED", swaps,
                    static_cast<unsigned long long>(kRvaCas), now,
                    en ? "Execute ignores the helper - VR writes what the snapshot writes"
                       : "original cmovnz - Execute writes 0 while helper+752 is set",
                    HelperActive(base));
            } else if (!failLogged) {
                LOG("[JITVR] *** %s FAILED at engine+0x%llX: bytes now %s- not applied ***", en ? "PATCH" : "RESTORE",
                    static_cast<unsigned long long>(kRvaCas), now);
                failLogged = true;
            }
        }
        // LOCKED fix (INV-562): if anything else puts the original bytes back, put the NOP back and say so. One 8-byte
        // read per 250 ms. Bytes that are neither pattern are NOT touched - something else owns them; that is SAID once.
        if (applied) {
            const int st = CasState(base);
            if (st == 0) {
                const bool ok = Swap(base, kCasOrig, kCasPatched);
                ++reverts;
                if (reverts <= 20 || !ok) {
                    LOG("[JITVR] *** REVERTED BY SOMETHING ELSE (#%u) - %s ***", reverts,
                        ok ? "re-applied, bytes PATCHED again" : "RE-APPLY FAILED, the fix is OFF");
                }
            } else if (st < 0 && !unknownLogged) {
                char now[40] = {};
                Hex(now, sizeof(now), base + EngRva(kRvaCas), 8);
                LOG("[JITVR] *** engine+0x%llX now holds %s- neither the original nor the fix; NOT touched, the fix is OFF ***",
                    static_cast<unsigned long long>(kRvaCas), now);
                unknownLogged = true;
            }
        }
        const DWORD nowMs = GetTickCount();
        // Every 5 min (locked on for the whole session since 2026-10-14; reverts and failures log the moment they happen).
        if (applied && nowMs - lastStatus >= 300000) {
            const int st = CasState(base);
            LOG("[JITVR] status: on, bytes %s, helper+752=%d, swaps %u, reverted by others %u",
                st == 1 ? "PATCHED" : (st == 0 ? "ORIGINAL (*** reverted by someone else ***)" : "UNKNOWN"),
                HelperActive(base), swaps, reverts);
            lastStatus = nowMs;
        }
    }
}

}  // namespace jitvr

// ---- CLIP-FAR READ + A/B: IBaseCamera::GetClipFar / SetClipFar (2026-10-03) ----
//
// QUESTION (Jay): far buildings vanish at the CENTRE of the view, kept at the sides.
// INV-368 (static, our engine e9fbfc2c): the mesh cull's 6th plane is a FLAT far plane at
// camera+356 (clip far) - a view-depth cut, so dead ahead culls first. NOT known: the live
// clip far. gamedll sets 100000 on two camera setups and 200/500 on preview-style ones
// (ghidra-project/ida/oc7_gamedll_out.txt) - which one is the scene camera is unread.
//
// HOW IT READS (oc7 + disassembly, this build): ILevel::GetViewCamera (rva 29CA10) ignores
// `this` and returns *(*(view+0x90390)+0x2B0), an IBaseCamera*; GetClipFar/GetClipNear (12F0A0
// / 12F0B0) read *(cam+8)+0x164 / +0x160; SetClipFar (12F010) writes +0x164 and rebuilds the
// projection, corners, AABB and planes (sub_1807643D0). The scene view is engine+0xAADDA8
// (oc1:404). The cull copy qword_180AAABF0 is a plain copy of the view's source camera
// (sub_18003EDE0, oc6), read here too so the log shows whether the change reached it.
//
// WHAT IT DOES. A 250 ms worker - nothing per frame. diag_clip_far logs [CLIPFAR] once a
// second. clip_far_scale != 1 sets far = (the game's own value) x scale through SetClipFar,
// re-applying if the game resets it (counted); back to 1 restores the game's value. Only the
// CURRENT camera is ever written (a remembered pointer may be freed - U-49 class). SetClipFar
// runs on this thread while the engine may read the camera: a one-frame torn plane set is
// possible, the same exposure the game's own calls have.
namespace clipfar {

typedef void* (*GetViewCamFn)(void* levelThis, void* view);
typedef float (*GetClipFn)(void* cam);
typedef void (*SetClipFn)(void* cam, float v);

constexpr uintptr_t kRvaGetViewCam = 0x29CA10;
constexpr uintptr_t kRvaGetFar = 0x12F0A0;
constexpr uintptr_t kRvaGetNear = 0x12F0B0;
constexpr uintptr_t kRvaSetFar = 0x12F010;
constexpr uintptr_t kRvaCurrentSceneView = 0xAADDA8;  // qword_180AADDA8
constexpr uintptr_t kRvaCullCamCopy = 0xAAABF0;       // qword_180AAABF0 (an object, not a pointer)
constexpr uintptr_t kViewCamHolder = 0x90390;          // GetViewCamera's first load
constexpr uintptr_t kViewSourceCam = 0x90398;          // what sub_1802B4EF0 copies into AAABF0
constexpr int kMaxApplyLines = 20;

volatile LONG g_started = 0;

bool SafeViewCam(GetViewCamFn fn, uintptr_t view, uintptr_t& out)
{
    bool ok = false;
    __try {
        out = reinterpret_cast<uintptr_t>(fn(nullptr, reinterpret_cast<void*>(view)));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeGetClip(GetClipFn fn, uintptr_t cam, float& out)
{
    bool ok = false;
    __try {
        out = fn(reinterpret_cast<void*>(cam));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeSetClip(SetClipFn fn, uintptr_t cam, float v)
{
    bool ok = false;
    __try {
        fn(reinterpret_cast<void*>(cam), v);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeReadF(uintptr_t addr, float& out)
{
    bool ok = false;
    __try {
        out = *reinterpret_cast<volatile float*>(addr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool SafeReadB(uintptr_t addr, uint8_t& out)
{
    bool ok = false;
    __try {
        out = *reinterpret_cast<volatile uint8_t*>(addr);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// Reach of a flat far plane at `farD`, along the view axis, the horizontal edge and the
// corner, from the frustum extents at the near plane (+336 l, +340 r, +344 b, +348 t, +352 near).
void Reach(uintptr_t camInner, float farD, float& centre, float& edgeH, float& corner,
           float& tanH, float& tanV)
{
    float l = 0, r = 0, b = 0, t = 0, n = 0;
    centre = farD;
    edgeH = corner = tanH = tanV = -1.0f;
    if (SafeReadF(camInner + 336, l) && SafeReadF(camInner + 340, r) && SafeReadF(camInner + 344, b)
        && SafeReadF(camInner + 348, t) && SafeReadF(camInner + 352, n) && n > 0.0f) {
        tanH = (fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r)) / n;
        tanV = (fabsf(b) > fabsf(t) ? fabsf(b) : fabsf(t)) / n;
        edgeH = farD * sqrtf(1.0f + tanH * tanH);
        corner = farD * sqrtf(1.0f + tanH * tanH + tanV * tanV);
    }
}

DWORD WINAPI Thread(LPVOID)
{
    // [PROBE: CLIPFAR] gate=diag_clip_far,clip_far_scale  site=FLAT  log-only=no
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        LOG("[CLIPFAR] *** REFUSED: engine_x64_rwdi.dll not mapped - diag_clip_far / clip_far_scale do nothing ***");
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    auto getCam = reinterpret_cast<GetViewCamFn>(GetProcAddress(
        engine, "?GetViewCamera@ILevel@@QEAAPEAVIBaseCamera@@PEAVCLevelView@@@Z"));
    auto getFar = reinterpret_cast<GetClipFn>(GetProcAddress(engine, "?GetClipFar@IBaseCamera@@QEAAMXZ"));
    auto getNear = reinterpret_cast<GetClipFn>(GetProcAddress(engine, "?GetClipNear@IBaseCamera@@QEAAMXZ"));
    auto setFar = reinterpret_cast<SetClipFn>(GetProcAddress(engine, "?SetClipFar@IBaseCamera@@QEAAXM@Z"));
    auto rva = [&](void* p) { return p ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - base) : 0ull; };
    LOG("[CLIPFAR] worker up: GetViewCamera=+%llX GetClipFar=+%llX GetClipNear=+%llX SetClipFar=+%llX "
        "(this build: +29CA10 +12F0A0 +12F0B0 +12F010), diag_clip_far=%d clip_far_scale=%.2f",
        rva(reinterpret_cast<void*>(getCam)), rva(reinterpret_cast<void*>(getFar)),
        rva(reinterpret_cast<void*>(getNear)), rva(reinterpret_cast<void*>(setFar)),
        DiagClipFar ? 1 : 0, ClipFarScale);
    if (rva(reinterpret_cast<void*>(getCam)) != EngRva(kRvaGetViewCam) || rva(reinterpret_cast<void*>(getFar)) != EngRva(kRvaGetFar)
        || rva(reinterpret_cast<void*>(getNear)) != EngRva(kRvaGetNear) || rva(reinterpret_cast<void*>(setFar)) != EngRva(kRvaSetFar)) {
        LOG("[CLIPFAR] *** REFUSED: export RVAs differ from engine e9fbfc2c - the view slot and camera layout "
            "are not trusted on this build; diag_clip_far / clip_far_scale do nothing ***");
        return 0;
    }

    uintptr_t curCam = 0;       // the camera `gameFar` belongs to
    float gameFar = -1.0f;      // the game's own clip far for curCam (never one we wrote)
    bool modified = false;      // we have written curCam's clip far
    unsigned applies = 0;
    unsigned camChanges = 0;
    bool lastDiag = false;
    float lastScale = 1.0f;
    bool refusedLogged = false;
    DWORD lastLog = 0;

    for (;;) {
        Sleep(250);
        const bool diag = DiagClipFar;
        float scale = ClipFarScale;
        if (!(scale >= 1.0f && scale <= 50.0f)) {
            scale = 1.0f;
        }
        if (scale != lastScale) {
            LOG("[CLIPFAR] clip_far_scale %.2f -> %.2f", lastScale, scale);
            lastScale = scale;
        }

        uintptr_t view = 0, cam = 0, inner = 0, srcCam = 0;
        bool ok = rastocc::SafeReadPtr(base + EngRva(kRvaCurrentSceneView), view) && view
                  && rastocc::Writable(view + kViewCamHolder, 16)
                  && SafeViewCam(getCam, view, cam) && cam && rastocc::Writable(cam, 16)
                  && rastocc::SafeReadPtr(cam + 8, inner) && inner && rastocc::Writable(inner + 336, 24)
                  && rastocc::Writable(inner + 552, 2);
        if (ok) {
            rastocc::SafeReadPtr(view + kViewSourceCam, srcCam);
        }
        if (!ok) {
            if ((diag || scale != 1.0f) && !refusedLogged) {
                LOG("[CLIPFAR] NOT READ: scene view %p / camera %p / inner %p not all committed read-write "
                    "(null view = no scene drawn yet) - no value, nothing applied",
                    reinterpret_cast<void*>(view), reinterpret_cast<void*>(cam), reinterpret_cast<void*>(inner));
                refusedLogged = true;
            }
            continue;
        }
        refusedLogged = false;

        float farNow = -1.0f, nearNow = -1.0f;
        if (!SafeGetClip(getFar, cam, farNow) || !SafeGetClip(getNear, cam, nearNow)) {
            continue;
        }
        if (cam != curCam) {
            ++camChanges;
            LOG("[CLIPFAR] scene camera now %p (inner %p, view %p, view+0x90398 %p %s), clip near %.3f far %.1f%s",
                reinterpret_cast<void*>(cam), reinterpret_cast<void*>(inner), reinterpret_cast<void*>(view),
                reinterpret_cast<void*>(srcCam), srcCam == inner ? "== inner" : "!= inner", nearNow, farNow,
                modified ? " - the previous camera's change is ABANDONED (not written again, may be freed)" : "");
            curCam = cam;
            gameFar = farNow;
            modified = false;
        }

        if (scale != 1.0f) {
            if (!modified) {
                gameFar = farNow;  // the game's own value, read before our first write
            }
            const float target = gameFar * scale;
            if (fabsf(farNow - target) > 0.5f) {
                float back = -1.0f;
                const bool setOk = SafeSetClip(setFar, cam, target) && SafeGetClip(getFar, cam, back);
                ++applies;
                if (applies <= kMaxApplyLines) {
                    LOG("[CLIPFAR] APPLIED #%u: clip far %.1f -> %.1f (game %.1f x %.2f), reads back %.1f%s%s",
                        applies, farNow, target, gameFar, scale, back, setOk ? "" : " *** SET FAULTED ***",
                        modified ? " - RE-APPLY: the game reset it" : "");
                }
                modified = true;
            }
        } else if (modified) {
            float back = -1.0f;
            const bool setOk = SafeSetClip(setFar, cam, gameFar) && SafeGetClip(getFar, cam, back);
            LOG("[CLIPFAR] RESTORED camera %p clip far -> %.1f (the game's value), reads back %.1f%s",
                reinterpret_cast<void*>(cam), gameFar, back, setOk ? "" : " *** SET FAULTED ***");
            modified = false;
        } else if (fabsf(farNow - gameFar) > 0.5f) {
            LOG("[CLIPFAR] the game changed clip far %.1f -> %.1f", gameFar, farNow);
            gameFar = farNow;
        }

        const DWORD now = GetTickCount();
        if (diag && (!lastDiag || now - lastLog >= 1000)) {
            float centre, edgeH, corner, tanH, tanV;
            Reach(inner, farNow, centre, edgeH, corner, tanH, tanV);
            float cNear = -1, cFar = -1;
            uint8_t inf = 0xFF, cInf = 0xFF;
            SafeReadB(inner + 552, inf);
            const uintptr_t cull = base + EngRva(kRvaCullCamCopy);
            SafeReadF(cull + 352, cNear);
            SafeReadF(cull + 356, cFar);
            SafeReadB(cull + 552, cInf);
            LOG("[CLIPFAR] scene cam near %.3f far %.1f inf %d | cull copy AAABF0 near %.3f far %.1f inf %d | "
                "tan H %.4f V %.4f -> flat-far reach: centre %.1f, H edge %.1f, corner %.1f m | scale %.2f "
                "applies %u cam changes %u",
                nearNow, farNow, inf, cNear, cFar, cInf, tanH, tanV, centre, edgeH, corner, scale, applies,
                camChanges);
            lastLog = now;
        }
        lastDiag = diag;
    }
}

}  // namespace clipfar

// AFW ALTERNATE-EYE A/B (2026-10-03, INV-369/INV-370, doc REF-AFW-PUREDARK-2026-10-03.md §9d). Jay: "Do it".
//
// WHAT IT TESTS. sub_1802B9680 records the "Scene" queue (layer 21) ONCE per frame and replays it per eye through
// CRenderContext slot 45 (ExecLayer, engine+0x762D50) - eye 0 then eye 1, each after sub_1802E5880(eye) set
// helper+73 and the eye shader variable. With afw_alt_eye on, frame N skips eye (N & 1)'s Scene replay, so each eye
// renders every other frame. NO WARP: the skipped eye is expected to keep its previous image (unmeasured - this run
// measures it). Shadows, recording, FX, the per-eye Sprites pass and everything else still run.
//
// THE INSTRUMENT ([AFWALT]): a frame = one eye-0 Scene replay call; QPC between consecutive ones gives the frame
// interval. Every 5 s: on/off, frames, Scene calls per eye, skips per eye, interval mean/max. Live A/B in one
// session at one refresh rate.
//
// COST. Off: nothing is installed until the box is first ticked; after that, one bool test and two plain loads per
// ExecLayer call (~25 per frame, INV-369). On: plus one QPC per frame. The patch is one vtable write, refused BY NAME
// unless slot 45 of the vtable at engine+0x8C8410 is engine+0x762D50 and slot 30 is +0x7ABB80 (e9fbfc2c, afw9) -
// so it never stacks on the bootstrap's [RENDLAYER] patch (marker removed 2026-10-03) or patches another build.
//
// KNOWN SIDE EFFECT, STATED BEFORE THE RUN: dlssflat resets both DLSS histories on any frame without exactly two
// tonemap draws (dlssflat.cpp:184-190) and takes the eye from the tonemap's index in the frame, so with one eye per
// frame DLSS runs history-less (spatial only) while the box is on. AA quality is expected to drop while on.
namespace afwalt {

typedef void* (__fastcall* ExecLayerFn)(void* ctx, int layerId, void* queue, void* params, char flags);
constexpr uintptr_t kRvaCtxVtable = 0x8C8410;
constexpr uintptr_t kRvaExecLayer = 0x762D50;
constexpr uintptr_t kRvaSetCamera = 0x7ABB80;
constexpr uintptr_t kRvaHelper = 0xA40290;
constexpr int kSceneLayer = 21;

ExecLayerFn g_orig = nullptr;
volatile LONG g_state = 0;          // 0 untried, 3 installing, 1 installed, 2 refused
// The eye THIS frame skips (-1 = none: AFW off, or no eye-0 Scene replay seen yet). Set at the eye-0 replay; read by
// CamHookRendTagRecord later in the same frame (sub_18022F3E0: views/Scene, then Sprites, then sub_1802E5A00 -> recorder)
// and carried in that frame's RENDTAG record to its submit (the submit lags the recording by 2-3 frames, INV-325).
volatile LONG g_skipEyeNow = -1;
void** g_helperGlobal = nullptr;
LONG64 g_frame = 0;                 // eye-0 Scene replays seen (render thread only)
LONGLONG g_lastQpc = 0;
LARGE_INTEGER g_qpf = {};
// U-51 VR-ONLY GATE (Jay 2026-10-07: "If flatscreen, disable; If VR, enable"). In flat the engine replays Scene for eye 0
// only (U-51: `eye1 0` in all 9 windows) and the alternation blanked the scene on every other frame - a strobe. So it acts
// only while the engine renders TWO eyes: an eye-1 Scene replay seen within the last kTwoEyeFrames frames. Eye 1's call
// still reaches this detour when it is skipped, so the test does not depend on our own skip. The setting is untouched.
// Every bootstrap AFW path keys off CamHookAfwRenderedEye (g_skipEyeNow), so held off = both-eyes path everywhere.
// COST: one compare per Scene replay (~2 per frame), one store on eye 1.
constexpr LONG64 kTwoEyeFrames = 2;
LONG64 g_lastEye1Frame = -1000;     // g_frame at the last eye-1 Scene replay (render thread only)
bool g_twoEyes = false;             // decided at each eye-0 replay
LONG g_heldOffLogs = 0;
// window counters (render thread only)
LONG g_wFrames = 0, g_wCalls[2] = {}, g_wSkips[2] = {}, g_wOtherEye = 0, g_wHeldOff = 0;
double g_wSumMs = 0.0, g_wMaxMs = 0.0;
bool g_wOn = false, g_lastOn = false, g_firstLogged = false;
ULONGLONG g_wStart = 0;

// ===== [AFWNET] THE SAFETY NET (trace docs/traces/20261121_1057_afw_tonemap_net.md; STATE 2026-11-21 (60)) =====
// A held eye is only right if the bootstrap REBUILDS it, and it cannot rebuild without the tonemap draw (the depth copies and the
// DLSS colour latch hang off it). On a tester's PC the bootstrap never saw that draw while this hook skipped every other eye render
// for the whole session (no rebuild, the image wobbling on head turns). Archive: 337 sessions that alternated all saw it no later
// than the alternation's start; the tester's never did. So: two eyes rendered + afw_enable + Data3 ours, and the bootstrap reports
// "not seen" for kNetMs continuously -> the AFW followers are held OFF (AfwAltEye / AfwRebuild = false: the AFW-off path every
// reader has; SyncFollowers keeps them off via AfwNetHeld) until the bootstrap first reports "seen", then restored. afw_enable and
// the settings file are never touched. COST: one export call per frame (two loads in the bootstrap).
constexpr ULONGLONG kNetMs = 5000;
typedef int (*PFN_TonemapSeen)();
PFN_TonemapSeen g_tsFn = nullptr;
bool      g_tsLooked = false;
ULONGLONG g_netSinceMs = 0;         // the alternation wanted with the tonemap draw not seen, since (0 = not now)
volatile LONG g_netHeld = 0;        // 1 = the followers are held off by the net
// 1 = a view-projection matrix has reached the layer this session (CamHookAfwPushVp, set BEFORE its afw_rebuild return so a hold
// still sees one arrive; never reset). Trace docs/traces/20261122_1140_afwnet_no_matrix.md: the tonemap draw seen with NO matrix =
// the rebuild refuses every frame (AfwRebuildRun code 2) - the Q3 + VDXR tester on 1.06. Archive: 646 / 646 healthy alternating
// sessions had one within ~1 s of the alternation's start.
volatile LONG g_netVpEver = 0;
LONG64    g_netHeldAt = 0;          // g_frame at the hold, for the resume line
int       g_netHolds = 0;

// OFF-CENTRE (trace docs/traces/20261124_1425_afwnet_offcentre_hold.md; RUN AF1 2026-11-24): AFW as built cannot rebuild an eye of an
// off-centre frustum - the gate's GpuCam reads the off-centre as ~13.6 deg of rotation and refuses every image (INV-612) - so with
// symmetric_fov 0 / 1 (Meta's runtime forces 0) the alternation ran with NO rebuild: half-rate eyes, the main menu text doubled on
// head movement (AF1). 1 = either eye's GAME frustum (dlvr_GameEyeTan, ungated) is off-centre, 0 = both symmetric (symmetric_fov 2:
// exactly), -1 = not known yet (no submit has described an eye). COST: two calls copying 4 floats each.
extern "C" int dlvr_GameEyeTan(int eye, float* out);
float g_offTan[4] = {};             // eye 0's tangents at the last test, for the hold line
// STAGE 1 OF AFW OFF-CENTRE (trace docs/traces/20261124_1700_afw_offcentre_stage1.md): a bootstrap that exports DlvrAfwOffCentreOk rebuilds an
// off-centre eye, so the off-centre hold below does not fire with it. An older bootstrap (no export) keeps the hold. Resolved once in NetTick.
typedef int (*PFN_OffCentreOk)();
int g_offOk = -1;                   // 1 = the export answered 1, 0 = missing / answered 0, -1 = not looked yet
int OffCentre()
{
    for (int e = 0; e < 2; ++e) {
        float t[4] = {};
        if (!dlvr_GameEyeTan(e, t)) { return -1; }
        if (e == 0) { for (int i = 0; i < 4; ++i) { g_offTan[i] = t[i]; } }
        if (fabsf(t[0] - t[1]) > 0.001f * (t[0] + t[1]) || fabsf(t[2] - t[3]) > 0.001f * (t[2] + t[3])) { return 1; }
    }
    return 0;
}

void NetTick(ULONGLONG now, bool two)
{
    if (!g_tsLooked) {
        g_tsLooked = true;
        HMODULE b = GetModuleHandleA("xinput1_3.dll");
        g_tsFn = b ? reinterpret_cast<PFN_TonemapSeen>(reinterpret_cast<void*>(GetProcAddress(b, "DlvrTonemapSeen"))) : nullptr;
        PFN_OffCentreOk ok = b ? reinterpret_cast<PFN_OffCentreOk>(reinterpret_cast<void*>(GetProcAddress(b, "DlvrAfwOffCentreOk"))) : nullptr;
        g_offOk = (ok && ok() == 1) ? 1 : 0;
        // [PROBE: AFWNET] gate=none (once per session, logged unconditionally, with the ARMED line)  site=HEADSET  log-only=no
        LOG("[AFWNET] OFF-CENTRE SUPPORT (logged unconditionally): DlvrAfwOffCentreOk %s - %s", ok ? "FOUND" : "NOT FOUND",
            g_offOk == 1 ? "the bootstrap rebuilds an off-centre eye: no off-centre hold (symmetric_fov 0 / 1, Meta's runtime)"
                         : "an older bootstrap: an off-centre frustum still holds the alternation off");
        // [PROBE: AFWNET] gate=none (once per session, logged unconditionally, at the first eye-0 Scene replay)  site=HEADSET  log-only=no
        LOG("[AFWNET] ARMED (logged unconditionally): %s | tonemap draw seen now %d (1 yes, 0 not yet, 2 the bootstrap's AFW capture is "
            "off on this AA path - NO AA / aa_native / a missing marker, -1 the bootstrap's colour hook is not installed: no verdict, "
            "the net does nothing) | the eye alternation is held off if the answer is still 0 or 2, or 1 with no view-projection "
            "matrix yet, %.1f s after it starts, and resumes the moment it is 1 with a matrix",
            g_tsFn ? "DlvrTonemapSeen resolved" : "DlvrTonemapSeen NOT resolved (an older bootstrap) - the net does nothing",
            g_tsFn ? g_tsFn() : -2, static_cast<double>(kNetMs) / 1000.0);
    }
    if (!g_tsFn) { return; }
    const int seen = g_tsFn();
    const bool vpEver = InterlockedCompareExchange(&g_netVpEver, 1, 1) == 1;
    // an off-centre frustum blocks AFW only when the bootstrap cannot rebuild one (g_offOk, trace 20261124_1700)
    const int off = g_offOk == 1 ? 0 : OffCentre();
    if (g_netHeld) {
        if ((seen == 1 && vpEver && off != 1) || !AfwEnable) {
            InterlockedExchange(&g_netHeld, 0);
            const bool back = AfwEnable && AfwData3Allowed();
            AfwAltEye = back;
            AfwRebuild = back;
            g_netSinceMs = 0;
            // [PROBE: AFWNET] gate=afw_enable (a resume after a hold; event)  site=HEADSET  log-only=no
            LOG("[AFWNET] RESUMED at frame %lld (held %lld frames): %s - afw_alt_eye / afw_rebuild back to %d", g_frame,
                g_frame - g_netHeldAt,
                seen == 1 ? "the bootstrap has seen the tonemap draw, a view-projection matrix has reached the layer and the game's "
                            "frustum is centred"
                          : "afw_enable was unticked",
                back ? 1 : 0);
        }
        return;
    }
    const bool intent = AfwEnable && two && AfwData3Allowed();
    if (intent && off == 1) {
        // OFF-CENTRE: hold at once - no wait would change it (AFW cannot rebuild an off-centre eye as built).
        InterlockedExchange(&g_netHeld, 1);
        g_netHeldAt = g_frame;
        ++g_netHolds;
        AfwAltEye = false;
        AfwRebuild = false;
        g_netSinceMs = 0;
        // [PROBE: AFWNET] gate=afw_enable + two eyes rendered (a hold; event)  site=HEADSET  log-only=no
        LOG("[AFWNET] *** HELD OFF *** at frame %lld (hold #%d): the game's frustum is OFF-CENTRE (eye 0 tangents L %.4f R %.4f U %.4f "
            "D %.4f - symmetric_fov 0 / 1, or Meta's runtime) and AFW as built cannot rebuild an off-centre eye (INV-612, RUN AF1) - "
            "both eyes are rendered every frame from now (AFW's frame-rate gain is lost, the image stays right). afw_enable stays as "
            "set (%d); resumes if the frustum becomes centred.", g_frame, g_netHolds, g_offTan[0], g_offTan[1], g_offTan[2],
            g_offTan[3], AfwEnable ? 1 : 0);
        return;
    }
    // 0 = the tonemap draw not seen yet; 2 = the bootstrap's AFW capture cannot run on this AA path; 1 with no matrix ever = the
    // rebuild refuses every frame (no camera). Any of them: no rebuild. -1 (no verdict) = the net does nothing, as before.
    const bool noRebuild = seen == 0 || seen == 2 || (seen == 1 && !vpEver);
    if (!intent || !noRebuild) { g_netSinceMs = 0; return; }
    if (g_netSinceMs == 0) { g_netSinceMs = now; return; }
    if (now - g_netSinceMs < kNetMs) { return; }
    InterlockedExchange(&g_netHeld, 1);
    g_netHeldAt = g_frame;
    ++g_netHolds;
    AfwAltEye = false;
    AfwRebuild = false;
    // [PROBE: AFWNET] gate=afw_enable + two eyes rendered (a hold; event, at most once per tonemap verdict)  site=HEADSET  log-only=no
    LOG("[AFWNET] *** HELD OFF *** at frame %lld (hold #%d): the eye alternation ran %.1f s and %s, so no AFW rebuild is possible - "
        "both eyes are rendered every frame from now (AFW's frame-rate gain is lost, the image stays right). afw_enable stays as set "
        "(%d); resumes when the bootstrap reports the tonemap draw seen with its AFW capture on.", g_frame, g_netHolds,
        static_cast<double>(now - g_netSinceMs) / 1000.0,
        seen == 2 ? "the bootstrap's AFW capture is OFF on this AA path (NO AA, aa_native.txt or a missing marker)"
        : seen == 1 ? "the tonemap draw is seen but NO view-projection matrix has reached the layer (the rebuild needs one)"
                    : "the bootstrap has NOT seen the tonemap draw",
        AfwEnable ? 1 : 0);
}

// [PROBE: AFWALT] gate=afw_alt_eye (the patch installs the first time the box is ticked; the window line then runs on or off)  site=HEADSET (the per-eye Scene replays exist only in sub_1802B9680's VR branch)  log-only=no
void Report(ULONGLONG now)
{
    const double secs = static_cast<double>(now - g_wStart) / 1000.0;
    LOG("[AFWALT] window %.1f s afw_alt_eye=%d%s: frames %ld (%.1f/s) | interval mean %.3f ms max %.3f ms | "
        "Scene calls eye0 %ld eye1 %ld | SKIPPED eye0 %ld eye1 %ld | Scene calls with eye not 0/1 %ld | "
        "HELD OFF (one eye rendered = flatscreen, U-51) %ld frames",
        secs, AfwAltEye ? 1 : 0, g_wOn != AfwAltEye ? " (toggled during this window)" : "", g_wFrames,
        secs > 0.0 ? g_wFrames / secs : 0.0, g_wFrames > 1 ? g_wSumMs / (g_wFrames - 1) : 0.0, g_wMaxMs,
        g_wCalls[0], g_wCalls[1], g_wSkips[0], g_wSkips[1], g_wOtherEye, g_wHeldOff);
    g_wFrames = 0; g_wCalls[0] = g_wCalls[1] = 0; g_wSkips[0] = g_wSkips[1] = 0; g_wOtherEye = 0; g_wHeldOff = 0;
    g_wSumMs = 0.0; g_wMaxMs = 0.0; g_wStart = now; g_wOn = AfwAltEye;
}

void* __fastcall Detour(void* ctx, int layerId, void* queue, void* params, char flags)
{
    if (layerId != kSceneLayer) {
        return g_orig(ctx, layerId, queue, params, flags);
    }
    int eye = -1;
    if (g_helperGlobal) {
        const unsigned char* helper = reinterpret_cast<const unsigned char*>(*g_helperGlobal);
        if (helper) { eye = helper[73]; }
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_firstLogged) {
        g_firstLogged = true;
        g_wStart = now;
        g_wOn = AfwAltEye;
        LOG("[AFWALT] FIRST Scene replay seen (logged unconditionally): eye %d (helper+73), tid %lu, afw_alt_eye=%d",
            eye, GetCurrentThreadId(), AfwAltEye ? 1 : 0);
    }
    if (eye == 0) {
        ++g_frame;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        if (g_lastQpc && g_qpf.QuadPart) {
            const double ms = static_cast<double>(q.QuadPart - g_lastQpc) * 1000.0 / static_cast<double>(g_qpf.QuadPart);
            g_wSumMs += ms;
            if (ms > g_wMaxMs) { g_wMaxMs = ms; }
        }
        g_lastQpc = q.QuadPart;
        ++g_wFrames;
        const bool two = (g_frame - g_lastEye1Frame) <= kTwoEyeFrames;
        if (AfwAltEye && (two != g_twoEyes || g_heldOffLogs == 0) && g_heldOffLogs < 8) {
            ++g_heldOffLogs;
            // [PROBE: AFWALT] gate=afw_alt_eye (the first decision while ticked is logged either way, then transitions, 8 at most)  site=FLAT+HEADSET  log-only=no
            LOG("[AFWALT] VR-ONLY GATE: %s at frame %lld - %s (last eye-1 Scene replay at frame %lld). afw_alt_eye=1 is "
                "left as set.", two ? "ACTIVE" : "HELD OFF", g_frame,
                two ? "the engine renders two eyes (VR), the alternation runs"
                    : "the engine renders ONE eye (flatscreen / no eye-1 replay), NO eye is skipped",
                g_lastEye1Frame < 0 ? -1LL : g_lastEye1Frame);
        }
        g_twoEyes = two;
        NetTick(now, two);   // [AFWNET]: may hold AfwAltEye off (or give it back) for THIS frame's decision below
    }
    if (eye == 1) { g_lastEye1Frame = g_frame; }
    const bool on = AfwAltEye && g_twoEyes;
    if (eye == 0) {
        if (AfwAltEye && !g_twoEyes) { ++g_wHeldOff; }
        InterlockedExchange(&g_skipEyeNow, on ? static_cast<LONG>(g_frame & 1) : -1);
    }
    if (on != g_lastOn) {
        LOG("[AFWALT] afw_alt_eye %d -> %d at frame %lld", g_lastOn ? 1 : 0, on ? 1 : 0, g_frame);
        g_lastOn = on;
    }
    void* r = nullptr;
    if (eye == 0 || eye == 1) {
        ++g_wCalls[eye];
        if (on && eye == static_cast<int>(g_frame & 1)) {
            ++g_wSkips[eye];          // skipped: the eye keeps whatever its target last held
        } else {
            r = g_orig(ctx, layerId, queue, params, flags);
        }
    } else {
        ++g_wOtherEye;
        r = g_orig(ctx, layerId, queue, params, flags);
    }
    if (now - g_wStart >= 5000) { Report(now); }
    return r;
}

void Install()
{
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        InterlockedExchange(&g_state, 2);
        LOG("[AFWALT] *** REFUSED: engine_x64_rwdi.dll not mapped - afw_alt_eye does nothing this session ***");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(engine);
    void** vt = reinterpret_cast<void**>(base + EngRva(kRvaCtxVtable));
    if (reinterpret_cast<uintptr_t>(vt[45]) != base + EngRva(kRvaExecLayer) || reinterpret_cast<uintptr_t>(vt[30]) != base + EngRva(kRvaSetCamera)) {
        InterlockedExchange(&g_state, 2);
        LOG("[AFWALT] *** REFUSED: CRenderContext vtable (engine+0x8C8410) slot 45 = %p (expected %p), slot 30 = %p "
            "(expected %p). Another build, or another patch already on slot 45 - afw_alt_eye does nothing this session ***",
            vt[45], reinterpret_cast<void*>(base + EngRva(kRvaExecLayer)), vt[30], reinterpret_cast<void*>(base + EngRva(kRvaSetCamera)));
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vt[45], sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedExchange(&g_state, 2);
        LOG("[AFWALT] *** REFUSED: VirtualProtect on CRenderContext slot 45 failed (err %lu) ***", GetLastError());
        return;
    }
    QueryPerformanceFrequency(&g_qpf);
    g_helperGlobal = reinterpret_cast<void**>(base + EngRva(kRvaHelper));
    g_orig = reinterpret_cast<ExecLayerFn>(vt[45]);
    InterlockedExchangePointer(&vt[45], reinterpret_cast<void*>(&Detour));
    VirtualProtect(&vt[45], sizeof(void*), old, &old);
    InterlockedExchange(&g_state, 1);
    LOG("[AFWALT] installed on CRenderContext slot 45 (ExecLayer, engine+0x762D50): Scene (layer 21) replays only. "
        "afw_alt_eye=%d - while on, frame N skips eye (N & 1). Stays installed; untick to restore both eyes.",
        AfwAltEye ? 1 : 0);
}

}  // namespace afwalt

// Installs the AFW alternate-eye hook the first time afw_alt_eye is ticked. Called from the scePadReadState poll:
// one bool test per poll while the box has never been on.
// For the bootstrap's DLSS upscale path (exported as dlvr_AfwRenderedEye): the eye the CURRENT frame renders while
// afw_alt_eye is active, else -1 (both eyes rendered). Read at the upscale draw, i.e. during the frame's D3D work, which
// INV-369 measured runs after that frame's layers are recorded, on the same thread - so this is that frame's stamp.
bool AfwNetHeld()
{
    return afwalt::g_netHeld != 0;
}

int CamHookAfwRenderedEye()
{
    if (!AfwAltEye || afwalt::g_state != 1) { return -1; }
    const LONG s = afwalt::g_skipEyeNow;
    return (s == 0 || s == 1) ? static_cast<int>(1 - s) : -1;
}

void StartAfwAltEyeOnce()
{
    if (!AfwAltEye || afwalt::g_state != 0) { return; }
    if (InterlockedCompareExchange(&afwalt::g_state, 3, 0) == 0) { afwalt::Install(); }
}

void StartClipFarOnce()
{
    if (InterlockedCompareExchange(&clipfar::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, clipfar::Thread, nullptr, 0, nullptr);
    }
}

// ---- vr_near_clip_cm: the VR first-person near plane (INV-549, 2026-10-12) ----
// The game's own VR near plane is the float at gamedll +1926BEC (0x3DC7E282 = 0.0976 m), loaded by exactly two instructions, both
// the first-person camera's near: sub_180C017A0 (+C017C0, PlayerFppVis camera interface slot 6) and CameraFPPDI's update fallback
// (+343295), which passes it to IBaseCamera::SetClipNear every update. A 250 ms worker writes that float when the setting changes -
// nothing per frame. Before the first write it VERIFIES both readers still decode to that address and that the value is the game's;
// any mismatch is a refusal by name, and nothing is written for the session.
namespace nearclip {

constexpr uintptr_t kRvaNear = 0x1926BEC;
constexpr uintptr_t kRvaReaders[2] = { 0xC017C0, 0x343295 };
constexpr DWORD     kGameBits = 0x3DC7E282;   // 0.0976 m
volatile LONG g_started = 0;

// movss xmm9, [rip + disp32] = F3 44 0F 10 0D disp32 (9 bytes); the target must be +1926BEC.
bool ReaderOk(uintptr_t base, uintptr_t rva)
{
    bool ok = false;
    __try {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(base + rva);
        if (p[0] == 0xF3 && p[1] == 0x44 && p[2] == 0x0F && p[3] == 0x10 && p[4] == 0x0D) {
            const int32_t disp = *reinterpret_cast<const int32_t*>(p + 5);
            ok = (base + rva + 9 + static_cast<intptr_t>(disp)) == base + ::dlvr::buildrva::Gd(kRvaNear);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

bool ReadBits(uintptr_t addr, DWORD& out)
{
    bool ok = false;
    __try { out = *reinterpret_cast<volatile DWORD*>(addr); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

bool WriteBits(uintptr_t addr, DWORD bits)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_READWRITE, &old)) { return false; }
    bool ok = false;
    __try { *reinterpret_cast<volatile DWORD*>(addr) = bits; ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &dummy);
    return ok;
}

DWORD WINAPI Thread(LPVOID)
{
    // [PROBE: NEARCLIP] gate=vr_near_clip_cm  site=FLAT  log-only=no  (the write and its read-back are flat-safe; what it SHOWS is HEADSET)
    HMODULE gd = nullptr;
    for (int i = 0; i < 240 && !gd; ++i) {   // up to 2 minutes for the game DLL
        gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!gd) { Sleep(500); }
    }
    if (!gd) {
        LOG("[NEARCLIP] *** REFUSED: gamedll_x64_rwdi.dll was not mapped within 2 minutes - vr_near_clip_cm does nothing this session ***");
        return 0;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gd);
    // The loaded build's rvas (common/buildrva.h: Steam unchanged, the EGL table on the Epic build); the checks are unchanged.
    using ::dlvr::buildrva::Gd;
    const uintptr_t addr = base + Gd(kRvaNear);
    DWORD bits = 0;
    const bool r0 = ReaderOk(base, Gd(kRvaReaders[0])), r1 = ReaderOk(base, Gd(kRvaReaders[1]));
    const bool v = ReadBits(addr, bits);
    if (!r0 || !r1 || !v || bits != kGameBits) {
        LOG("[NEARCLIP] *** REFUSED: the VR near-plane float is not where gamedll 97046bba keeps it - reader +C017C0 %s, reader "
            "+343295 %s, value at +1926BEC %s0x%08lX (want 0x%08lX = 0.0976). vr_near_clip_cm does nothing this session ***",
            r0 ? "ok" : "MISMATCH", r1 ? "ok" : "MISMATCH", v ? "" : "UNREADABLE ", static_cast<unsigned long>(bits),
            static_cast<unsigned long>(kGameBits));
        return 0;
    }
    LOG("[NEARCLIP] armed: gamedll +1926BEC = 0.0976 m (the game's VR first-person near plane; both readers verified). "
        "vr_near_clip_cm=%.2f - %s", VrNearClipCm,
        (VrNearClipCm > 9.755f && VrNearClipCm < 9.765f) ? "the game's value, nothing written" : "applying");
    DWORD written = kGameBits;
    for (;;) {
        float cm = VrNearClipCm;
        if (!(cm >= 2.0f && cm <= 12.0f)) { cm = 9.76f; }
        const DWORD want = (cm > 9.755f && cm < 9.765f) ? kGameBits : [](float m) {
            DWORD b; memcpy(&b, &m, 4); return b; }(cm * 0.01f);
        if (want != written) {
            DWORD cur = 0;
            if (!ReadBits(addr, cur) || cur != written) {
                LOG("[NEARCLIP] *** REFUSED: +1926BEC holds 0x%08lX, not the 0x%08lX last written - something else changed it; "
                    "nothing more is written this session ***", static_cast<unsigned long>(cur), static_cast<unsigned long>(written));
                return 0;
            }
            float before = 0.0f, after = 0.0f;
            memcpy(&before, &written, 4);
            const bool ok = WriteBits(addr, want);
            DWORD back = 0;
            ReadBits(addr, back);
            memcpy(&after, &back, 4);
            LOG("[NEARCLIP] near plane %.2f -> %.2f cm: %s (read back 0x%08lX = %.4f m)", before * 100.0f, cm,
                (ok && back == want) ? "WRITTEN" : "*** WRITE FAILED ***", static_cast<unsigned long>(back), after);
            if (ok && back == want) { written = want; }
        }
        Sleep(250);
    }
}

}  // namespace nearclip

void StartNearClipOnce()
{
    if (InterlockedCompareExchange(&nearclip::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, nearclip::Thread, nullptr, 0, nullptr);
    }
}

// Starts the raster-occlusion A/B worker once per process. Idle while the
// setting is off except for one pointer read every 250 ms.
void StartRastOccOnce()
{
    if (InterlockedCompareExchange(&rastocc::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, rastocc::Thread, nullptr, 0, nullptr);
    }
}

// Starts the VR jitter-execute A/B worker once per process. Verifies the engine bytes once, then idles at one bool read
// every 250 ms while the setting is off.
void StartJitVrOnce()
{
    if (InterlockedCompareExchange(&jitvr::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, jitvr::Thread, nullptr, 0, nullptr);
    }
}

// ---- DAMAGE ARGUMENT CAPTURE: HealthModule::AddDamageToBody (2026-09-10) ----
//
// WHY THIS IS LIVE AND NOT STATIC. Four IDA passes established the damage path
// (see ghidra-project\ida\DAMAGE_SYSTEM_FINDINGS_2026-09-10.md) and hit a wall
// on ONE thing: the twelve arguments of HealthModule::AddDamageToBody. Hex-Rays
// contradicts itself about them across the function's two overrides - a5 is
// `float` in one and `unsigned int` in the other - so at least one inferred
// signature is wrong and neither can be trusted. The call-site sweep that would
// have resolved it FAILED: +0x838 is vtable slot 263 of *any* class, 65 sites
// matched, and the first is a one-argument bool method on something unrelated.
//
// ENGINE_ARCHITECTURE.md already prescribes the remedy for exactly this class of
// question: "needs a watch-writes/execution-breakpoint session, the same
// technique already proven for ComputeShootDir". So: trap the function entry and
// read the arguments out of the real CONTEXT.
//
// SIX ROUTES ARE ALREADY CLOSED - read the findings file before proposing any of
// them again. In particular OnTakeDamage is a folded no-op stub (473 exports
// share one address) and the engine's collision-damage system cannot reach a
// held weapon at all.
namespace damagewatch {

// gamedll RVA, from the log strings '[HealthModule::AddDamageToBody] current
// health: ' / 'remaining HealthBody: ' - the function names itself, so this is
// identified rather than guessed.
constexpr uintptr_t kAddDamageToBodyRva = 0x2A2830;

// THE WATCH TARGET IS A SETTING, because there is more than one candidate and a
// rebuild per candidate wastes a session.
//
// MEASURED 2026-09-10, with every instrument check passing - slot verified
// exclusive (ONLY our address armed, 117 threads), and the staged gamedll's md5
// equal to the deployed one so the RVA is valid for the running build:
// **0x2A2830 recorded 0 hits while the user was landing melee hits and kills.**
// That is a GENUINE NEGATIVE, not an instrument failure: the base
// HealthModule::AddDamageToBody does not run for melee damage on zombies.
//
// LEADING HYPOTHESIS (labelled): zombies use BodyPartsHealthModule - per-limb
// health and dismemberment, which is precisely what this game does to them. Slot
// 263 was found in THREE vtables:
//     RVA 0x1447AF0 -> sub_1802A2830   (watched; never fired)
//     RVA 0x1455158 -> sub_1802A2830   (same function, second class)
//     RVA 0x14451E0 -> sub_180279AF0   (dismissed as "just a pass-through")
// **0x279AF0 is therefore the next candidate - and it was dismissed for LOOKING
// like a wrapper, not for any measured reason.**
//
// Default stays 0x2A2830 so the recorded negative stays reproducible.
// (DamageWatchRvaOverride is defined at file scope in namespace dlvr, beside
// DiagDamageWatch - it is a SETTING, so it must not live inside this
// sub-namespace or the registration in ovr2xr.cpp cannot resolve it.)

// ===== THE DATA-WATCH APPROACH: let the health value name its own writers =====
//
// WHY THIS REPLACES GUESSING ADDRESSES. Three exec-breakpoint runs on three
// reasoned candidates returned 0 hits each, with every instrument check passing
// (slot verified exclusive, correct address armed, staged gamedll md5 == the
// deployed one):
//     0x2A2830  HealthModule::AddDamageToBody   - never fired on melee
//     0x279AF0  the other slot-263 override     - never fired on melee
// The PREMISE was the weak link, and it was flagged as such the moment the
// call-site sweep failed: **slot 263 is a commonly-used index** - 65 sites, the
// first a one-argument bool method on an unrelated class - so "slot 263 is the
// damage virtual" was suggestive and never established. Two negatives make it
// wrong.
//
// So stop nominating functions and watch the DATA. That is the technique that
// actually worked here: a DR0 data watch named element 48's three writers with
// no guessing at all. Whatever writes health IS the damage pipeline, whichever
// class it turns out to belong to.
//
// THE ADDRESS IS DATA-DERIVED, NOT GUESSED. data/ai/ai_modules.def states the
// slot indices outright:
//     $HEALTH_MODULE             (i, 22)
//     $BODY_PARTS_HEALTH_MODULE  (i, 23)
//     $ARMOR_HEALTH_MODULE       (i, 31)
// modules[] lives at entity+0xC10 with 8-byte slots - the same scheme
// kMovementModuleSlot=52 already uses and which is confirmed live on EE. Inside
// the module, m_HealthBody is a CRTTIFieldUInt at +0x78, read off the field
// registrar rather than inferred.
//
// VERSION CAVEAT, AND IT IS HANDLED: ai_modules.def is 2020 base game and ours
// is EE, so the slot index is VERIFIED AT RUNTIME with RttiClassNameOf rather
// than trusted. If the class at modules[22] is not a HealthModule, this refuses
// and says so instead of watching an arbitrary pointer - which is exactly how a
// wrong index would otherwise masquerade as "no writers found".
constexpr size_t kHealthModuleSlot          = 22;
constexpr size_t kBodyPartsHealthModuleSlot = 23;
constexpr size_t kHealthBodyOffset          = 0x78;
// How long each window spends POLLING for a health value that moves before it
// gives up and re-polls. Nothing is armed during this phase, so it costs no
// debug register and has no deadline for the user to hit.
constexpr int    kDetectSeconds             = 20;

// ===== BLOCK-DIFF FIELD SCAN (2026-09-11) =====
//
// WHY THE OFFSET IS NO LONGER TRUSTED. The 11:51 run put the CONFIRMED PLAYER
// entity (00000204BA4A7DA8 - it validated against modules[] and drives the
// sprint byte-diff) and its own health module into the candidate list, polled
// all 20 candidates at 10 Hz for 3.5 minutes while the user took hits, and
// NOTHING CHANGED. Across three runs +0x78 read 111.2881 / 112.8883 / 114.1406:
// health-shaped, per-instance, and constant under damage. That is the signature
// of a CONFIGURED or MAXIMUM health, not the live value.
//
// `m_HealthBody` is what the field registrar CALLS +0x78. The registrar
// describes a declared field; it is not evidence about which field the damage
// pipeline writes. A float that merely LOOKS like health is not the live one -
// and this probe's own failure message previously said "widen the entity
// capture, NOT the offset", which was exactly backwards.
//
// So do to the OFFSET what was already done to the TARGET: stop nominating it.
// Snapshot each module's memory and DIFF it, and let the changing bytes name
// the field. Same move that cracked element 48.
//
// TWO PHASES, because raw diffing is useless on its own - timers, counters and
// cached transforms change every frame and would bury the signal:
//   A. NOISE PROFILE: poll with nothing happening and mark every offset that
//      moves on its own as VOLATILE. Those are excluded.
//   B. DETECT: poll again; report only offsets that change and were NOT marked
//      volatile - i.e. fields that started moving when combat did.
constexpr int kMaxScanTargets = 96;
constexpr int kBlockBytes     = 512;              // per module, 4-byte granular
constexpr int kBlockDwords    = kBlockBytes / 4;
constexpr int kNoiseSeconds   = 6;

struct ScanTarget {
    uintptr_t entity;
    uintptr_t base;
    unsigned  slot;
    char      cls[64];
    // The module's vptr, latched when the target is taken on. A health module
    // belongs to a body that can DIE, and its memory is then freed and reused.
    // Re-reading this before believing a decrease is what separates "this body
    // took damage" from "this block is now somebody else's" - the same test
    // weaponanchor already applies to the movement module, where it prints
    // *** STALE MODULE ***. Added 2026-09-18 after a run in which 44 of 46
    // "*** DAMAGE ***" lines were the level unloading (see below).
    uintptr_t vptr;
};
ScanTarget    g_scanTargets[kMaxScanTargets] = {};
int           g_scanTargetCount = 0;
// static, not stack: 96 * 512 = 48 KB each, and this runs on a worker thread.
unsigned char g_blockBase[kMaxScanTargets][kBlockBytes] = {};
unsigned char g_blockNow[kMaxScanTargets][kBlockBytes]  = {};
bool          g_blockOk[kMaxScanTargets]                = {};
bool          g_volatileDword[kMaxScanTargets][kBlockDwords] = {};

// One guarded bulk copy per module per tick - far cheaper than per-field SEH,
// and a module that goes away simply drops out instead of faulting the thread.
bool SnapshotBlock(uintptr_t base, unsigned char* dst)
{
    __try {
        memcpy(dst, reinterpret_cast<const void*>(base), kBlockBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Safe single-float read. SafeReadLocal lives ~2000 lines below this point and
// the damage watch needs it here; __try/__except keeps this frame in the same
// shape as SnapshotBlock above - no C++ objects to unwind (MSVC C2712).
bool ReadFieldFloat(uintptr_t addr, float& out)
{
    __try {
        out = *reinterpret_cast<const float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadFieldU32(uintptr_t addr, uint32_t& out)
{
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadFieldBytes(uintptr_t addr, void* dst, size_t n)
{
    __try {
        memcpy(dst, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ===== INV-51: `HumanAIStateVector` OWNS THE m_LastHit* BLOCK. REACHED IN TWO HOPS =====
//
// >>> THE OFFSETS WERE RIGHT ALL ALONG. THE OWNING CLASS WAS WRONG, TWICE. <<<
// INV-50 put this block on HumanAI; the correction put it on HumanLogicModule.
// BOTH ARE WRONG. The CRTTI construction site names it outright, WITH ITS SIZE:
//
//   CRTTI::CRTTI(qword_181C5C0D0, "HumanAIStateVector", 164401,
//                sub_18060E9E0, &unk_181C28120, 0, ..., 2328, 1);
//
// 2328 bytes - the only candidate large enough to hold a field at +0x6F0.
// HumanLogicModule-sized objects cannot, and that size argument would have
// caught both mistakes immediately. Compare:
//   BodyPartsHealthModule = 288 bytes (fields at 200..216 fit)
//   ArmorHealthModule     = 400 bytes (m_ArmorElementsHealth 344 + 13*4 = 396)
//
// EVERY OFFSET BELOW IS NOW CONFIRMED TWICE: once by the registrar, and again
// by independent one-line accessors that have nothing to do with it -
//   sub_1802CF580:  return *(unsigned int *)(a1 + 1664);   GetHitCount
//   sub_1802CF560:  *(_DWORD *)(a1 + 1664) = a2;           SetHitCount
//   sub_1802CF540:  a1[418..420]  = 1672/1676/1680         location vec3
//   sub_1802CF520:  a1[423..425]  = 1692/1696/1700         vector vec3
//   sub_1802CF510:  *(float *)(a1 + 1704)                  physical damage
//   sub_1802CF4E0:  *(_BYTE *)(a1 + 1687) = a2;            bone
//   sub_1802CF4D0:  *(_BYTE *)(a1 + 1686) = a2;            part
//
// THE COUNTER IS REAL AND THE INCREMENT IS VISIBLE, in sub_1802ABE70 (the hit
// registration site, called from sub_1802FCE20):
//     v185 = sub_1802CF580(v5);            // get
//     ... v185 = sub_1802CF580(v5) + 1;    // +1
//     sub_1802CF560(v5, v185);             // set
//     sub_1802CF4E0(v5, *v186);            // bone
//     sub_1802CF4D0(v5, *v187);            // part
//
// AND THE PATH TO v5 IS IN THAT SAME FUNCTION:
//     v3 = *((_QWORD *)this + 8);          // this + 0x40
//     v5 = *(_QWORD *)(v3 + 2464);         // v3 + 0x9A0
//
// >>> EVERY SENTENCE THAT USED TO FOLLOW THOSE TWO LINES WAS WRONG. CORRECTED
//     2026-09-15 FROM OUR OWN BINARY -> INV-55, INV-56. THE OLD TEXT CLAIMED
//     `this` IS THE IGSObject WEAPONANCHOR CAPTURES AND THAT +0x9A0 IS THE
//     STATE VECTOR. BOTH ARE FALSE, AND THE 20:00 RUN'S 17-of-17 REFUSAL IS
//     EXACTLY WHAT THAT COSTS. <<<
//
//   1. `this` is an AIModule, NOT an entity. sub_1802ABE70 is SLOT 247 of
//      HumanLogicModule's vtable (COL offset-in-class 0); sub_1802FCE20 is slot
//      247 of ThugLogicModule/ZombieLogicModule; sub_1802D1C60 of
//      BtzSuiciderLogicModule/InfectedLogicModule/SpitterLogicModule. IDA types
//      their `this` as IGSObject* and THAT TYPING IS A GUESS.
//   2. `this + 0x40` is AIModule::m_AI, type AI*. Read BY NAME from
//      sub_180272C30:  *((_DWORD *)v1 + 34) = 64;
//                      *(_QWORD *)v1 = &CRTTIFieldPtrNativeScript<AI>::vftable;
//                      CRTTIField::Initialize(v1, "m_AI", ...)
//   3. +0x9A0 is AI::m_AIBlend - NOT the state vector. AI::m_StateVector is at
//      +0x970. Both from AI::RegisterRTTI() = sub_1801765C0:
//          offset 2416 (0x970) SmartPtr<AIStateVector> -> "m_StateVector"
//          offset 2464 (0x9A0) SmartPtr<AIBlend>       -> "m_AIBlend"
//      THE REGISTRARS SET THE OFFSET **BEFORE** THE NAME. Reading the offset
//      that sits above a name binds it to the wrong field, and that is precisely
//      how 0x9A0 got recorded as the state vector.
//   4. The entity weaponanchor captures is the IGSObject VIEW of a HumanAI,
//      which sits at mdisp 0x28 within it, so HumanAI* == entity - 0x28, and
//      AI is a base of HumanAI at mdisp 0 so AI* == HumanAI*.
//
// WHY entity+0x40 RETURNED THE SAME VALUE ON ALL 17 ENTITIES: it is
// HumanAI+0x68, and mdisp 0x68 is the IAttractorObject base - a VTABLE POINTER.
// Predicted gamedll+0x151CF50; the run observed 0x7FFB4F92CF50 against a module
// base of 0x7FFB4E410000. Exact match. It was never a bad offset; it was the
// wrong object and the wrong field at the same time.
//
// THE CHAIN IS SELF-VERIFYING: HumanAIStateVector is RTTI-visible
// (.?AVHumanAIStateVector@@), so after the hops we CONFIRM THE CLASS NAME
// before reading a single field. Three runs were spent reading confident
// garbage off unverified bases; this one cannot.
namespace humanai {
// ---- THE CHAIN. Signed: the first hop goes BACKWARDS. ----
//
// >>> CORRECTED AGAIN 2026-09-15 AFTER THE 20:34 RUN, AND THE PREVIOUS
//     "CORRECTION" IN THIS FILE WAS ITSELF WRONG. READ THIS BEFORE TOUCHING
//     ANY OFFSET TAKEN FROM A CRTTI REGISTRAR. <<<
//
// CRTTI REGISTRAR OFFSETS ARE MEASURED FROM THE CRTTIObject SUBOBJECT, NOT
// FROM THE COMPLETE OBJECT. Read from the base-class tables in our own binary:
//     HumanAI            CRTTIObject at mdisp 0x28
//     AI                 CRTTIObject at mdisp 0x28
//     AIModule           CRTTIObject at mdisp 0x00
//     LogicModule        CRTTIObject at mdisp 0x00
//     HumanAIStateVector CRTTIObject at mdisp 0x00
// That one rule explains all three outcomes of the 20:34 run independently:
//   - AIModule::m_AI @ 64 needed no correction  -> C4 PASSED 18/18
//   - HumanAIStateVector's fields need none     -> the 14 BONE HITs worked
//   - AI's fields DO need +0x28                 -> C3 FAILED 0/18
// It has been invisible until now only because every class this project had
// touched has CRTTIObject at 0, so the correction was always zero.
//
// AI::RegisterRTTI (sub_1801765C0), CRTTI-relative -> complete:
//     +0x970 (2416) -> +0x998   m_StateVector    SmartPtr<AIStateVector>
//     +0x9A0 (2464) -> +0x9C8   m_AIBlend        SmartPtr<AIBlend>
//     +0x9F8 (2552) -> +0xA20   m_PlanState
//     +0xA30        -> +0xA58   m_SpawnerCreator
//     +0xA40        -> +0xA68   m_SMCycle
//     +0xE98        -> +0xEC0   m_ObjectBindedTo
//
// AND SmartPtr<T> HOLDS ITS RAW T* AT +8. The engine's own read, in
// sub_1802FCE20, is the authority and it is unambiguous:
//     v26 = *((_QWORD *)this + 8);          // AIModule::m_AI -> complete AI
//     if ( !*(_QWORD *)(v26 + 2456)         // +0x998 m_StateVector, null-guard
//        || (v27 = *(_QWORD *)(v26 + 2464)) == 0     // +0x9A0 the AIStateVector*
//        || !*(_BYTE *)(v27 + 2191) ...     // +0x88F, inside its 2328 bytes
//
// >>> SO INV-52's ORIGINAL "+0x9A0" WAS CORRECT AND THE 2026-09-15 EDIT THAT
//     RELABELLED IT m_AIBlend WAS WRONG. m_AIBlend is at complete +0x9C8. The
//     ONLY thing ever wrong with the original chain was its BASE: it applied
//     +0x9A0 to the ENTITY, where the engine applies it to the AI. <<<
constexpr ptrdiff_t kEntityToHumanAI = -0x28;  // IGSObject/CRTTIObject view -> complete AI
constexpr size_t    kAiToStateVecPtr = 0x9A0;  // complete AI -> the AIStateVector* itself
constexpr size_t    kAiToStateVecFld = 0x998;  // complete AI -> m_StateVector field start
constexpr size_t    kAiModuleToAi    = 0x40;   // AIModule::m_AI - CONFIRMED LIVE by C4 18/18
// Composed from the entity: -0x28 + 0x9A0 = +0x978.
constexpr ptrdiff_t kEntityToStateVec =
    kEntityToHumanAI + static_cast<ptrdiff_t>(kAiToStateVecPtr);
// Predicted vptr of a captured entity: the COL at HumanAI offset-in-class 0x28.
// CONFIRMED LIVE 20:34, 18/18, against gamedll base 0x7FFB4A710000.
constexpr uintptr_t kHumanAiIgsVtableRva = 0x151E600;
// data/ai/ai_modules.def, read out of the SHIPPED EE Data0.pak (not the 2020
// dump): the logic module varies by NPC type, so all of them are tried.
constexpr unsigned kLogicModuleSlots[] = { 35, 36, 37, 38, 44, 47, 50, 59, 61, 63 };
constexpr int kLogicModuleSlotCount =
    static_cast<int>(sizeof(kLogicModuleSlots) / sizeof(kLogicModuleSlots[0]));
constexpr size_t kStateVectorSize = 2328;    // from the CRTTI ctor

// >>> THE SETTER WE WATCH. IT HANDS US THE POINTER WE SPENT FOUR RUNS DERIVING. <<<
//   sub_1802CF4E0(__int64 a1, char a2):  *(_BYTE *)(a1 + 1687) = a2;
// a1 IS the HumanAIStateVector and a2 IS the bone index, at the exact instant a
// hit is registered. WatchExec traps BEFORE the instruction runs and captures
// RCX/RDX, so no patching, no prologue decoding, and no pointer chain at all.
constexpr uintptr_t kSetBoneRva = 0x2CF4E0;
constexpr size_t kHitCount            = 0x680;   // uint32, monotonic
constexpr size_t kLastDamageSum       = 0x684;   // float
constexpr size_t kLastHitLocation     = 0x688;   // vec3  <== R5's "AT X LOCATION"
constexpr size_t kLastHitDir          = 0x694;   // byte
constexpr size_t kLastHitPart         = 0x696;   // byte  <== body part
constexpr size_t kLastHitBone         = 0x697;   // byte  <== BONE
constexpr size_t kDeathType           = 0x698;   // byte
constexpr size_t kLastHitVector       = 0x69C;   // vec3
constexpr size_t kLastHitPhysicDamage = 0x6A8;   // float
constexpr size_t kLastHitForcedCutTypes = 0x6C8; // NOT the gore record - see below

// ===================== R5's GORE HALF, AND IT IS NOT +0x6C8 =====================
//
// >>> m_LastHitForcedCutTypes IS A DEAD END, ESTABLISHED 2026-09-15 21:35 AND IT
//     TOOK A SYNCHRONOUS CAPTURE TO PROVE IT. All 70 hits that run read
//     CUTS=0xFFFFFFFF *at the trapped instruction*, and SEVERAL OF THEM SEVERED
//     LIMBS. The decompile rules out the two innocent explanations: in
//     sub_1802ABE70 the writes `*(_BYTE *)(v5 + 1740)` and
//     `*(_DWORD *)(v5 + 1736)` (lines 829/831) land on the SAME v5 that is then
//     handed to SetHitCount (1178) and SetLastHitBone (1186, our trap) - so the
//     object IS the state vector and the write IS before the trap in pseudocode
//     order, not merely in address order. We would have seen a value. There is
//     none. The field's NAME promised something it does not carry. <<<
//
// THE REAL DISMEMBERMENT STATE IS ON BodyPartsHealthModule, from its own
// registrar (BodyPartsHealthModule::RegisterRTTI, gamedll+0x2848F0):
//
//   +0xDC m_BodyCutType   +0xE0 m_CutOffBones    +0xE4 m_BodyHoles
//   +0xE8 m_DestroyedBones +0xEC m_HeadSmashed   +0xED m_HeadSmashedInHalf
//   +0xF0 m_CutOffTypes
//
// WHY THESE OFFSETS APPLY TO WHAT WE ACTUALLY FIND, which was the real risk:
// every run has RTTI'd these modules as ArmorHealthModule, not
// BodyPartsHealthModule - and ArmorHealthModule DERIVES FROM
// BodyPartsHealthModule AT mdisp 0x0, so the offsets are unadjusted. All three
// of HealthModule / BodyPartsHealthModule / ArmorHealthModule also carry
// CRTTIObject at mdisp 0x0, so INV-57's correction is zero here.
//
// AND THE SIZE CHECK PASSES (INV-51's lesson - a field at +0xF0 cannot live in
// a 200-byte class): HealthModule is 200 bytes per its CRTTI ctor, which is
// exactly why these are NOT on it; BodyPartsHealthModule is 288 and
// ArmorHealthModule 400, and 0xF0 = 240 fits both.
//
// NOT ESTABLISHED, deliberately: whether these fields MOVE on a dismemberment.
// The .text write counts for these displacements are class-agnostic - 0xE0 and
// 0xE8 are ordinary offsets used by many classes - and a displacement does not
// identify a class. That is precisely the mistake that made +0x6C8 look alive.
// It is a RUNTIME question and the delta reporting below is how it gets asked.
// ===== THE MELEE ELEMENT OVERRIDE POINT (INV-65, U-24) =====
//
// sub_180C4D920 is the SLastHit builder. Its THIRD parameter (a3, in R8) is the
// contact-shaped record, and the element index lives at a3+0x8.
//
// CONFIRMED AT INSTRUCTION LEVEL, not from pseudocode:
//     0x180C4DE0C   mov  rsi, [rbp+120h+arg_10]    ; rsi = a3 (3rd parameter)
//     0x180C4E611   mov  r12d, [rsi+8]             ; r12d = [a3+8]  <- the element
//     0x180C4E990   mov  [r14+20h], r12d           ; -> SLastHit+0x20
// and the store two instructions earlier, `mov dword ptr [r14+14h], 0BF800000h`
// (-1.0f), pins that block to the decompile's line 709/710.
//
// Five further reads of [rsi+8] feed EDX, consistent with the SDamageInfoDi half
// (its element is at +0x38, which is where the AI side reads at a2+56). That
// half is pseudocode-level, NOT disassembly-confirmed - but it does not need to
// be for the override to be sound: rewriting [a3+8] ON ENTRY means EVERY
// consumer inside the function reads our value, whoever they turn out to be.
//
// WHY THIS POINT AND NOT ANOTHER: all NINE melee attack paths funnel through
// this one function (INV-65), including the two that hardcode "spine3". There
// is no upstream chooser to hook - the bone is a compile-time constant per path.
constexpr uintptr_t kSlastHitBuilderRva = 0xC4D920;
constexpr int32_t   kRecordElemOffset   = 0x8;
constexpr int       kPokeRegR8          = 2;      // a3 is the 3rd x64 argument

constexpr size_t kBpBodyCutType     = 0xDC;
constexpr size_t kBpCutOffBones     = 0xE0;
constexpr size_t kBpBodyHoles       = 0xE4;
constexpr size_t kBpDestroyedBones  = 0xE8;
constexpr size_t kBpHeadSmashed     = 0xEC;   // byte
constexpr size_t kBpHeadSmashedHalf = 0xED;   // byte
constexpr size_t kBpCutOffTypes     = 0xF0;
// $BODY_PARTS_HEALTH_MODULE (i, 23) and $HEALTH_MODULE (i, 22), read from the
// SHIPPED EE Data0.pak's data/ai/ai_modules.def.
constexpr unsigned kBodyPartsHealthSlot = 23;
constexpr unsigned kHealthSlot          = 22;
}  // namespace humanai

// ===== [ELEM] SELF-REPORTING, ON ITS OWN CLOCK =====
//
// >>> THIS EXISTS BECAUSE THE 2026-09-16 11:43 RUN WAS VOID AND THE DEFECT WAS
// THE REPORTING, NOT THE PROBE. <<< The stats were emitted from inside
// damagewatch's health-DETECT tick loop. At 11:44:24 damagewatch latched a health
// module and moved into its write-watch window; the detect loop stopped ticking,
// and the element override - still armed, cap lifted, expired=no - went completely
// unreported for the remaining 3m 45s. Three overrides were ever recorded, and the
// run could not answer the question it was run for.
//
// THE RULE THIS ENCODES: **a probe's own health must not be reported from a loop
// it does not own.** Two runs in a row were lost to that shape - first a shared
// 400-hit cap (INV-69), then a foreign loop's phase.
//
// ONE thread, started exactly once at arming, running for the process lifetime.
// Deliberately never stopped and never restarted: camhook.cpp records that
// clearing a start latch on exit spawned 3,992 threads in two minutes.
//
// It does NOT call WatchExecActive(). That function STOPS the watch when it finds
// the expired flag, so "reading" it would disarm the very thing being reported.
// WatchExecStats() is side-effect free, which is why it exists.
DWORD WINAPI ElemStatsThread(LPVOID)
{
    bool saidExpired = false;
    // The capture dedup lives here, with the poll it belongs to. INV-59: the table
    // COPIES and AGGREGATES, so a real increment is the only thing worth a line -
    // never one line per returned entry per poll, which once produced 25,031 lines
    // from 13 real bodies.
    static uintptr_t s_eSv[64] = {};
    static uint32_t  s_eCount[64] = {};
    int ticks = 0;
    for (;;) {
        // 250 ms for captures, 5 s for stats. The capture poll has to stay fast
        // enough that a burst of distinct call sites is seen while the table still
        // holds them; the stats line only needs a time series.
        Sleep(250);
        ++ticks;

        // >>> INLINE MODE'S MODEL DISCOVERY. RUN 6 HAD NO EQUIVALENT AND WROTE
        // NOTHING FOR 11 MINUTES - `PART MAP hits=0 … 0 model(s) mapped`. <<<
        // The DR path fills the part map from the WatchExec hits drained below;
        // in inline mode there are none, so the hook parks its records and this
        // hands them to the SAME discovery entry point, on the SAME 250 ms
        // cadence, on this worker rather than the game's melee thread.
        if (MeleeHookInline != 0) {
            const int drained = MeleeElemDrainSeen();
            static int s_totalDrained = 0;
            static bool s_saidFirst = false;
            s_totalDrained += drained;
            if (drained > 0 && !s_saidFirst) {
                s_saidFirst = true;
                LOG("damagewatch: [MELEEHOOK] discovery is running - drained %d "
                    "record(s) to the element dump on the first poll that had "
                    "any. THIS IS THE HALF RUN 6 WAS MISSING: without it the "
                    "part map never fills and every poke writes nothing.",
                    drained);
            }
        }

        dlvr::ExecHit eh[64] = {};
        const int en = dlvr::WatchExecGet(eh, 64);
        for (int h = 0; h < en && h < 64; ++h) {
            const uintptr_t rec = eh[h].r8Value;   // a3, the contact record
            if (!rec) { continue; }
            if (s_eSv[h] != rec) { s_eSv[h] = rec; s_eCount[h] = 0; }
            if (eh[h].count <= s_eCount[h]) { continue; }
            s_eCount[h] = static_cast<uint32_t>(eh[h].count);

            uintptr_t model = 0;
            weaponanchor::ReadPtr(rec, model);
            char mcls[96];
            if (!model || !RttiClassNameOf(model, mcls, sizeof(mcls))) {
                strncpy_s(mcls, sizeof(mcls), "<no rtti>", _TRUNCATE);
            }
            const bool forced = (MeleeForceElem >= 0.0f);
            LOG("damagewatch: *** MELEE ELEM *** record=%p  model=%p '%s'  "
                "element=%u%s  (watch count=%llu)  <== [R8+0x%X] captured at "
                "the SLastHit builder, upstream of all nine attack paths",
                reinterpret_cast<void*>(rec), reinterpret_cast<void*>(model),
                mcls,
                eh[h].pokeOrig,          // the ORIGINAL, read either way
                forced
                    ? (eh[h].pokeValid
                           ? " >>> OVERRIDDEN, see the value above as the "
                             "ORIGINAL and the armed value as the new one"
                           : " >>> OVERRIDE ATTEMPTED BUT THE WRITE FAILED - "
                             "the read-back or the store faulted, so this hit "
                             "was NOT redirected")
                    : " (observe only, nothing written)",
                static_cast<unsigned long long>(eh[h].count),
                humanai::kRecordElemOffset);

            // >>> AND THE ELEMENT TABLE FOR WHATEVER WE JUST HIT. <<<
            // This is what turns "melee_force_elem=N did something" into "N is
            // <name> and converts to bone <b> ON THIS MODEL". It rides the hit
            // we already have - no extra run, no extra slot - and dumps once per
            // distinct model (capped at 8).
            //
            // The flat validation on 2026-09-16 is what licenses trusting it:
            // the same walk reproduced INV-34's l_handholder=35, r_handholder=71,
            // propsholder1=84, propsholder2=85 AND its undecided 92-vs-93 count,
            // on the player model. It also showed only 23 of 92 elements map to a
            // real bone - the other 69 return the 255 no-bone sentinel - which is
            // the leading candidate explanation for why forcing a rare element
            // makes hits ineffective. This is the line that tests that.
            dlvr::ElementTableDumpFromHitRecord(rec, "melee target");
        }

        if (ticks % 20 != 0) { continue; }   // stats every 20 x 250 ms = 5 s

        unsigned long fires = 0, refused = 0;
        bool expired = false;
        int armedT = 0, liveT = 0;
        dlvr::WatchExecStats(&fires, &refused, &expired, &armedT, &liveT);
        unsigned long mapHits = 0, mapMiss = 0, mapRej = 0;
        int mapEntries = 0;
        dlvr::WatchExecPokeMapStats(&mapHits, &mapMiss, &mapEntries, &mapRej);
        if (MeleeForcePart > 0 && mapRej > 0) {
            // >>> THE ANSWER TO THE TWO CRASHES, IF IT IS NON-ZERO. <<< A
            // rejection is a record that matched a live model pointer at the key
            // offset but whose original value at the write offset was NOT a
            // plausible element - i.e. a differently-shaped record (INV-67) that
            // we would previously have written an element index into.
            LOG("damagewatch: [ELEM] *** SHAPE CHECK REJECTED %lu WRITE(S) *** - "
                "records matched a mapped model but their element field was out "
                "of range for it, so NOTHING was written. These are exactly the "
                "writes suspected of causing the 2026-09-16 crashes. A non-zero "
                "count here CONFIRMS wrong-shaped records were being written "
                "into; zero means that hypothesis is dead.", mapRej);
        }
        if (MeleeForcePart > 0) {
            // >>> THE MISS COUNT IS THE GATE ON THIS RUN. <<< A miss is a hit on
            // a model with no mapping: nothing was written, so that blow was the
            // UNMODIFIED game. High misses with a low hit count means the
            // behaviour observed is mostly not ours.
            unsigned long tHits = 0, tMiss = 0;
            int tEntries = 0, tPoisoned = 0;
            dlvr::WatchExecPokeTypeStats(&tHits, &tMiss, &tEntries, &tPoisoned);
            // >>> typeHits IS REPORTED SEPARATELY ON PURPOSE. <<< It is the only
            // way to tell whether D-44's fix is doing anything: an instance hit
            // would have landed anyway, a TYPE hit is a blow that would have
            // MISSED before this existed. Folding them into one number would
            // make the fix unmeasurable.
            LOG("damagewatch: [ELEM] PART MAP hits=%lu typeHits=%lu misses=%lu "
                "rejected=%lu, %d model(s) mapped, %d type key(s)%s.%s",
                mapHits, tHits, mapMiss, mapRej, mapEntries, tEntries,
                (tPoisoned > 0) ? " *** SOME POISONED ***" : "",
                (mapMiss > (mapHits + tHits))
                    ? "  <== MORE MISSES THAN HITS: most blows wrote NOTHING and "
                      "were the unmodified game. Do not read a behavioural result "
                      "from this run without accounting for that."
                    : "");
            if (tPoisoned > 0) {
                LOG("damagewatch: [ELEM] *** %d TYPE KEY(S) POISONED of %d *** - "
                    "two skeletons share an element count but not a layout, so "
                    "hits on those types write NOTHING deliberately rather than "
                    "guess a wrong-but-valid index (D-42).", tPoisoned, tEntries);
            }
        }
        // >>> WHICH DENOMINATOR, AND SAY WHICH. <<< Run 6 printed `fires=407` in
        // inline mode and that number was NOT the melee site - WatchExecStats is
        // global, so it was reporting diag_player_movemod's transient watch. A
        // denominator that belongs to another client is worse than none: it
        // reads as activity. In inline mode the hook's own call count is the
        // only honest one, and the label says so.
        const bool inlineNow = (MeleeHookInline != 0);
        const unsigned long denom = inlineNow ? MeleeElemInlineHookCalls() : fires;
        const unsigned long dropped = inlineNow ? MeleeElemSeenDropped() : 0ul;
        if (inlineNow && dropped > 0) {
            LOG("damagewatch: [MELEEHOOK] *** %lu RECORD(S) DROPPED *** - the hook "
                "filled its ring before the worker drained it, so those models "
                "were never handed to discovery. Counted, not overwritten: a "
                "missing model must not look like one that never appeared.",
                dropped);
        }
        LOG("damagewatch: [ELEM] STATS %s=%lu (every one of these REPLACED the "
            "element) | table %d/64, refused %lu new key(s) | expired=%s | "
            "threads armed %d, live %d%s",
            inlineNow ? "hookCalls" : "fires", denom,
            dlvr::WatchExecCount(), refused,
            inlineNow ? "n/a (no WatchExec)" : (expired ? "YES" : "no"),
            armedT, liveT,
            (liveT > armedT)
                ? "  <== threads exist that were created AFTER arming and do NOT "
                  "carry the breakpoint; work on them is invisible to this watch"
                : "");
        if (expired && !saidExpired) {
            saidExpired = true;
            LOG("damagewatch: [ELEM] *** THE WATCH EXPIRED AND DISARMED ITSELF "
                "after %lu hits. THE OVERRIDE IS NO LONGER RUNNING. *** Everything "
                "observed from here on is the UNMODIFIED game. If you see this, the "
                "hit cap was not lifted - see INV-69.", fires);
        }
    }
}

// ===================== THE MELEE ATTACK TRIGGER =====================
//
// Jay asked whether the watch correlates against an attack trigger. It does
// now, and the trigger was found by READING the binary, not by guessing.
//
// The melee input handler is gamedll RVA 0xDB3720. DO NOT ATTACH A SLOT INDEX
// TO THAT SENTENCE: melee_vtable_full.txt holds FIVE melee vtables and the
// function appears at a DIFFERENT index in each ([35] in the block at
// 0x1733A58, [22] in the block at 0x1733AC0, which is the one RttiVtableOf
// actually returns). Decompiled (E:\IDA, staged gamedll_x64_rwdi.dll.i64):
//
//     void __fastcall sub_180DB3720(__int64 a1, int a2, float a3)
//
// It is an INPUT-EVENT handler, not a swing-physics routine: a2 is an action
// code and a3 == 1.0 means "pressed". The switch handles codes
// 35, 139, 159, 160, 212, 272, 276 - and the earlier finding stands, there is
// no direction or velocity anywhere in it. That is fine. We are not asking it
// where the weapon went; we are asking it WHEN THE PLAYER SWUNG, so a health
// drop can be attributed to our attack instead of to fire, a fall, or another
// zombie.
//
// WE DO NOT GUESS WHICH CODE IS ATTACK. Every call is timestamped and the
// first 40 are logged with their code and value, so the run itself tells us
// which code the swing button produces. Guessing it here is exactly the class
// of mistake that cost three runs.
//
// WHY A VTABLE SWAP AND NOT InstallDetour: the detour machinery needs the
// target's prologue bytes and a hand-decoded stolen-instruction length, which
// cannot be produced for an address only known at runtime. A vtable slot needs
// neither. And because the signature is now KNOWN EXACTLY, the thunk is a
// plain typed C++ function - the compiler spills and reloads the float in xmm2
// around our logging call by construction, so forwarding is exact. An untyped
// thunk would have destroyed that float argument.
using MeleeInputFn = void(__fastcall*)(void*, int, float);

MeleeInputFn   g_origMeleeInput = nullptr;
volatile LONG  g_swingSeq       = 0;
LONGLONG       g_lastSwingQpc   = 0;
// Presses only - the correlation clock. Kept separate from the all-events
// counters above so the axis flood stays measurable without polluting this.
LONGLONG       g_lastPressQpc   = 0;
// >>> [EVTRING] - WE DO NOT GUESS WHICH CODE IS "ATTACK". <<<
// Jay's attack is LEFT MOUSE, and inputenums.def puts EMouse__BUTTON_1 at
// 0xF000 - nowhere near the codes this handler receives (3, 6, 40, 282...), so
// these are action IDs and the device binding tells us nothing. Filtering on
// "value == 1.0" was no better: it caught every digital key-down and reported
// 672 "presses" in three minutes. So instead of narrowing a filter on a guess,
// keep the last N events and DUMP THEM NEXT TO EACH HIT. The run then reports
// which code precedes a hit, the same way it reported the vtable slot.
struct InputEvt { LONGLONG qpc; int code; float val; };
constexpr int  kEvtRing = 24;
InputEvt       g_evtRing[kEvtRing] = {};
volatile LONG  g_evtSeq = 0;
int            g_lastPressCode  = -1;
volatile LONG  g_pressSeq       = 0;
int            g_lastSwingCode  = -1;
float          g_lastSwingVal   = 0.0f;
LONG           g_swingLogged    = 0;

// >>> R5 STEP 3: THE HIT-RECORD LATCH. THE INSTANCE ARRIVES HERE FOR FREE. <<<
//
// `self` IS the WeaponMeleeController*. INV-124/127 put the melee hit record at
// this+0x6F0 (left/single) and this+0x728 (right), 0x38 bytes each, with the
// phase latch at this+0x760 - so holding this pointer is the whole of the reach
// problem (U-31), and the thunk that solves it was already installed for the
// attack-trigger correlation.
//
// NOTHING IS WRITTEN THROUGH IT. recordprobe below only reads.
void* volatile g_meleeSelf   = nullptr;
volatile LONG  g_meleeSelfLogged = 0;

void __fastcall MeleeInputThunk(void* self, int code, float val)
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    // The latch is unconditional and costs one store. It is NOT behind the probe
    // setting on purpose: a setting read here would make "the probe was off"
    // and "no swing ever arrived" produce the same silence later.
    if (self) {
        InterlockedExchangePointer(&g_meleeSelf, self);
        if (InterlockedExchange(&g_meleeSelfLogged, 1) == 0) {
            LOG("recordprobe: [LATCH] WeaponMeleeController* = %p, captured from "
                "the melee input thunk (gamedll+0xDB3720, vtable RVA 0x1733AC0 "
                "slot 22). Hit records are +0x6F0 / +0x728, phase latch +0x760 "
                "(INV-127).", self);
        }
    }
    g_lastSwingQpc  = now.QuadPart;
    g_lastSwingCode = code;
    g_lastSwingVal  = val;
    // >>> AXIS EVENTS ARE NOT ATTACKS. THE 16:09 RUN TAGGED EVERY SINGLE
    //     FALSE POSITIVE "CORRELATED" BECAUSE OF THIS. <<<
    // That run logged codes 0/1/2/3 carrying equal-and-opposite fractional
    // values (+0.08/-0.08, +1.92/-1.92, +3.33/-3.33) - mouse look, split into
    // one action code per axis direction. They fire continuously: 829 events in
    // a single 20 s window. With "most recent input event of ANY kind" as the
    // reference, SOMETHING is always within 400 ms, so the correlation column
    // said CORRELATED for everything and therefore meant nothing.
    //
    // A press is value == 1.0 exactly (the handler's own switch tests a3 == 1.0
    // on its arms - see INV-49). Anything fractional is an axis. Only presses
    // move the correlation clock; every call still counts, so the flood stays
    // visible in the totals.
    {
        const LONG slot = InterlockedIncrement(&g_evtSeq) - 1;
        InputEvt& e = g_evtRing[slot % kEvtRing];
        e.qpc  = now.QuadPart;
        e.code = code;
        e.val  = val;
    }
    const bool isPress = (val == 1.0f);
    if (isPress) {
        InterlockedExchange(&weaponanchor::g_meleeUsed, 1);
        g_lastPressQpc  = now.QuadPart;
        g_lastPressCode = code;
        InterlockedIncrement(&g_pressSeq);
    }
    const LONG seq = InterlockedIncrement(&g_swingSeq);
    // Log the first 40 only. The 15:40 run produced 2,658 "events" in one 20 s
    // window at a steady ~12 ms spacing - that cadence WAS the evidence it was
    // a per-frame predicate and not a keypress, so the sequence number is
    // printed to keep that tell visible.
    // Log PRESSES only, and budget 60 of those. The 16:09 run burned its whole
    // 40-event budget on mouse-look in about five seconds and the light-vs-heavy
    // code mapping was lost with it. Axis events are still counted in seq, they
    // are just not worth a line each.
    if (isPress && InterlockedIncrement(&g_swingLogged) <= 60) {
        LOG("damagewatch: [TRIGGER] ATTACK PRESS #%ld code=%d  (event #%ld "
            "overall, the rest are axis/look)  (gamedll+0xDB3720)",
            g_pressSeq, code, seq);
    }
    if (g_origMeleeInput) { g_origMeleeInput(self, code, val); }
}

// Returns true only if a slot now holds our thunk. Says WHY on every failure
// path - a trigger that silently did not install would make an empty
// correlation column look like "the player never swung".
//
// >>> THE 2026-09-15 15:40 RUN IS WHY THIS NO LONGER TAKES A SLOT INDEX. <<<
// v1 hardcoded slot 35, read out of melee_vtable_full.txt. That file contains
// FIVE melee vtables, and RttiVtableOf("WeaponMeleeController") returns
// RVA 0x1733AC0 - which is 0x1733A58 + 0x68, i.e. THIRTEEN SLOTS into the
// block I had read. So the index shifts by 13 and the input handler sits at
// [22] on the vtable we actually get, while [35] there is sub_180DBD390,
// a two-arg predicate returning bool. We hooked the predicate with a
// void-returning three-arg thunk: its "codes" were a pointer's low dword, it
// fired twice a frame instead of once a press, and the game spent the whole
// run reading a clobbered EAX as that predicate's answer.
//
// The fix is to stop naming a slot at all. We know the FUNCTION - gamedll +
// 0xDB3720, whose signature we decompiled - so scan the vtable for that
// pointer and hook wherever it actually lives. An index cannot be stale if it
// is never written down. And if the function is not in this vtable, REFUSE:
// v1's "installing regardless" printed the evidence that it had the wrong
// target and then installed anyway, which is the actual defect.
constexpr uintptr_t kMeleeInputRva = 0xDB3720;
constexpr int       kVtableScanSlots = 64;

bool InstallMeleeTrigger()
{
    if (g_origMeleeInput) { return true; }
    const uintptr_t vt = dlvr::RttiVtableOf("WeaponMeleeController");
    if (!vt) {
        LOG("damagewatch: [TRIGGER] NOT INSTALLED - "
            "RttiVtableOf(\"WeaponMeleeController\") returned 0. No melee "
            "controller has been constructed yet; retrying next window.");
        return false;
    }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd) {
        LOG("damagewatch: [TRIGGER] NOT INSTALLED - gamedll_x64_rwdi.dll not "
            "mapped.");
        return false;
    }
    const uintptr_t want = reinterpret_cast<uintptr_t>(gd) + kMeleeInputRva;

    int found = -1;
    int readable = 0;
    for (int i = 0; i < kVtableScanSlots; ++i) {
        uintptr_t v = 0;
        __try {
            v = reinterpret_cast<uintptr_t*>(vt)[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        ++readable;
        if (v == want) { found = i; break; }
    }
    if (found < 0) {
        LOG("damagewatch: [TRIGGER] NOT INSTALLED - gamedll+0x%zX (the melee "
            "input handler) is NOT in the first %d readable slot(s) of vtable "
            "%p. REFUSING rather than hooking whatever sits at a guessed index "
            "- that is exactly what broke the 15:40 run. Nothing was written.",
            kMeleeInputRva, readable, reinterpret_cast<void*>(vt));
        return false;
    }

    void** slot = reinterpret_cast<void**>(vt) + found;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG("damagewatch: [TRIGGER] NOT INSTALLED - VirtualProtect failed on "
            "vtable %p slot %d, GetLastError=%lu.",
            reinterpret_cast<void*>(vt), found, GetLastError());
        return false;
    }
    g_origMeleeInput = reinterpret_cast<MeleeInputFn>(want);
    *slot = reinterpret_cast<void*>(&MeleeInputThunk);
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LOG("damagewatch: [TRIGGER] INSTALLED - found gamedll+0x%zX at vtable %p "
        "SLOT %d (not 35; v1's hardcoded index was 13 slots off). Signature is "
        "void(this,int code,float val), decompiled, so the float in xmm2 "
        "survives the thunk. Every call is timestamped.",
        kMeleeInputRva, reinterpret_cast<void*>(vt), found);
    return true;
}

// ================= R5 STEP 3: THE HIT-RECORD PROBE =================
//
// READ-ONLY. It takes no exec-watch slot, no debug register, no detour and no
// per-frame path - it samples from its own thread. The only thing it needs is
// g_meleeSelf, which the attack-trigger thunk already latches.
//
// WHAT IT IS FOR. Everything in INV-124..127 is static. This measures five
// things a flatscreen run can settle, so no headset run is spent on them:
//   1. does the phase latch really go 1 -> 2 -> 3, and how many frames does the
//      damage window last;
//   2. do the two records fill, and does frame n's start equal frame n-1's end
//      (the sliding sweep INV-127 predicts);
//   3. >>> ARE +0x30/+0x34 TIMES OR LENGTHS? <<< sub_180DC2000 uses them as
//      scalars in a length term, sub_180DB1D10 compares them against elapsed
//      time. One of those readings is wrong. ~0.1-0.4 says seconds, ~1-2 says
//      metres. This is the open contradiction in INV-127;
//   4. is unk_181CFB510 - the vec3 sub_180DB0650 fills the record with - really
//      (0,0,0)? Static could NOT answer it: the bytes are not in the file and a
//      read of the database returns IDA's FF filler, which is not a value. If it
//      is not zero then sub_180DC2000's |x| <= 1e-10 test can never be true and
//      INV-127's "the hand-holder branch is the ordinary path" is WRONG;
//   5. which hand's record a given swing uses, and whether +0x34 == -1.0f marks
//      the unused one as the reset implies.
//
// INV-11 ALLOWS ALL FIVE IN FLATSCREEN: they are structural, temporal and
// constant reads. The one thing that needs the headset is where the sweep sits
// relative to the controller, and that is deliberately NOT asked here.
constexpr uintptr_t kRecAOff  = 0x6F0;      // INV-127, left / single-hand
constexpr uintptr_t kRecBOff  = 0x728;      // INV-127, right
constexpr uintptr_t kPhaseOff = 0x760;      // INV-127, 1 = window open, 2, 3
constexpr uintptr_t kZeroConstRva = 0x1CFB510;   // what sub_180DB0650 writes

struct HitRec {
    float a[3];        // +0x00
    float b[3];        // +0x0C
    float start[3];    // +0x18
    float end[3];      // +0x24
    float winOpen;     // +0x30
    float winClose;    // +0x34
};
static_assert(sizeof(HitRec) == 0x38, "HitRec must match INV-127's 0x38 bytes");

// No C++ objects in scope: MSVC will not allow __try in a function that needs
// unwinding, and a probe that crashes the game is worse than no probe.
bool ProbeRead(const void* src, void* dst, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

volatile LONG g_recordProbeStarted = 0;

DWORD WINAPI RecordProbeThread(LPVOID)
{
    // ---- the constant, read ONCE and printed whatever it turns out to be ----
    {
        const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!gd) {
            LOG("recordprobe: [ZEROCONST] NO VERDICT - gamedll_x64_rwdi.dll is "
                "not mapped, so RVA 0x%zX could not be read. This is not 'the "
                "constant is non-zero'.", kZeroConstRva);
        } else {
            const void* at = reinterpret_cast<const char*>(gd) + kZeroConstRva;
            float v[3] = {0};
            unsigned char raw[12] = {0};
            if (!ProbeRead(at, v, sizeof(v)) || !ProbeRead(at, raw, sizeof(raw))) {
                LOG("recordprobe: [ZEROCONST] NO VERDICT - %p is mapped but "
                    "unreadable. Not a value.", at);
            } else {
                const bool isZero = (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
                LOG("recordprobe: [ZEROCONST] gamedll+0x%zX = %.9g %.9g %.9g   "
                    "bytes %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X   "
                    ">>> exactly (0,0,0)? %s <<<  If NO, sub_180DC2000's "
                    "|x| <= 1e-10 test can never fire and INV-127's hand-holder "
                    "reading is wrong.",
                    kZeroConstRva, v[0], v[1], v[2],
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                    raw[6], raw[7], raw[8], raw[9], raw[10], raw[11],
                    isZero ? "YES" : "NO");
            }
        }
    }

    // ---- the sampler ----
    // 2 ms. A damage window spans several frames at any frame rate this game
    // reaches, so this catches a handful of samples per swing without being a
    // per-frame cost of any kind - it is not on the frame path at all.
    LONG  phasePrev = -1;
    unsigned long posePrev = 0;
    LONG  swingsLogged = 0;
    LONG  samplesThisSwing = 0;
    DWORD lastRefusal = 0;
    HitRec prevA = {};
    bool   havePrevA = false;
    constexpr LONG kMaxSwings = 12;
    constexpr LONG kMaxSamplesPerSwing = 24;

    for (;;) {
        void* self = InterlockedCompareExchangePointer(&g_meleeSelf, nullptr, nullptr);
        if (!self) {
            // NAME WHAT IS MISSING AND HOW TO GET IT. A silent probe and a probe
            // with nothing to look at must not read the same in the log.
            const DWORD now = GetTickCount();
            if (lastRefusal == 0 || now - lastRefusal > 10000) {
                lastRefusal = now;
                LOG("recordprobe: NO INSTANCE YET - g_meleeSelf is null, so the "
                    "melee input thunk has not fired once. That thunk is "
                    "installed by InstallMeleeTrigger(), which needs "
                    "RttiVtableOf(\"WeaponMeleeController\") to resolve - it "
                    "returns 0 until a melee controller has been constructed, "
                    "i.e. until you are in a level holding a melee weapon. "
                    ">>> THIS IS NOT 'the record is empty'. NOTHING HAS BEEN "
                    "SAMPLED. <<<  Swing once and this line stops.");
            }
            Sleep(250);
            continue;
        }

        const char* base = reinterpret_cast<const char*>(self);
        LONG phase = 0;
        if (!ProbeRead(base + kPhaseOff, &phase, sizeof(phase))) {
            Sleep(100);
            continue;
        }
        if (phase != phasePrev) {
            // >>> THE POSE CALL COUNT RIDES THE PHASE LINE ON PURPOSE. <<<
            // [POSE] itself is budgeted to 40 lines, but the COUNT is not, so
            // every window's sub-step count is on record even after the detail
            // stops - the same split that made the [SAMPLE]/[PHASE] pair work
            // on 2026-09-18 (INV-128). A count that stays flat across a whole
            // window is the signal that the hook is not live, and the line
            // says which of those it is rather than leaving a silence.
            const unsigned long poseNow = dlvr::MeleePoseHookCalls();
            // writes and failures beside the calls: an offset that is armed but
            // never lands, and one that is not armed at all, must not read the
            // same on this line.
            // driven/refused ride here for the same reason: step 5b's detail is
            // budgeted and its stats line is 1 Hz, but the question "did the
            // controller steer THIS window" has to be answerable per window.
            // A window with calls climbing and driven flat is the rig being
            // dead, not the hook being dead, and those are different fixes.
            LOG("recordprobe: [PHASE] %ld -> %ld   (this+0x%zX)   pose calls "
                "total=%lu delta=%lu  writes=%lu failed=%lu  "
                "driven=%lu refused=%lu  hook=%s",
                phasePrev, phase, kPhaseOff, poseNow, poseNow - posePrev,
                dlvr::MeleePoseHookWrites(), dlvr::MeleePoseHookWriteFails(),
                dlvr::MeleePoseHookDriven(), dlvr::MeleePoseHookRefused(),
                dlvr::MeleePoseHookLive() ? "live"
                    : (DiagMeleePoseProbe ? "NOT INSTALLED - see poseprobe: lines"
                                          : "off (diag_melee_pose_probe=0)"));
            posePrev = poseNow;
            if (phase == 1) {
                samplesThisSwing = 0;
                havePrevA = false;
                ++swingsLogged;
            }
            phasePrev = phase;
        }
        if (phase == 1 && swingsLogged <= kMaxSwings &&
            samplesThisSwing < kMaxSamplesPerSwing) {
            HitRec ra = {};
            HitRec rb = {};
            const bool okA = ProbeRead(base + kRecAOff, &ra, sizeof(ra));
            const bool okB = ProbeRead(base + kRecBOff, &rb, sizeof(rb));
            if (!okA && !okB) {
                LOG("recordprobe: [SAMPLE] BOTH RECORDS UNREADABLE at %p "
                    "+0x%zX/+0x%zX - no verdict, not an empty swing.",
                    self, kRecAOff, kRecBOff);
                Sleep(2);
                continue;
            }
            // Log the first sample of every swing unconditionally, then only
            // when the sweep END actually moves - a static record between frames
            // is itself the answer to "does it slide", so silence there would
            // destroy the measurement.
            const bool moved =
                !havePrevA || ra.end[0] != prevA.end[0] ||
                ra.end[1] != prevA.end[1] || ra.end[2] != prevA.end[2];
            if (moved) {
                ++samplesThisSwing;
                LOG("recordprobe: [SAMPLE] swing#%ld n=%ld  A start(%.4f %.4f %.4f) "
                    "end(%.4f %.4f %.4f) win(%.5f %.5f)  |  B start(%.4f %.4f %.4f) "
                    "end(%.4f %.4f %.4f) win(%.5f %.5f)  readable A=%d B=%d",
                    swingsLogged, samplesThisSwing,
                    ra.start[0], ra.start[1], ra.start[2],
                    ra.end[0], ra.end[1], ra.end[2], ra.winOpen, ra.winClose,
                    rb.start[0], rb.start[1], rb.start[2],
                    rb.end[0], rb.end[1], rb.end[2], rb.winOpen, rb.winClose,
                    okA ? 1 : 0, okB ? 1 : 0);
                // >>> EVERY SAMPLE, NOT JUST THE FIRST. <<< The 19:22 run logged
                // these on frame 1 only, where they equalled that frame's own
                // start/end exactly - which is equally consistent with "the
                // PREVIOUS frame's segment, seeded on frame 1" and with "a copy
                // of the current one", and the two have opposite consequences:
                // if it is the previous segment then the record is a real
                // four-point HitDamageQuad and anything writing +0x18..0x2C must
                // write these too or the quad is malformed. Frames 2..n decide
                // it, so frames 2..n are printed. PREV is the previous ROW's
                // start/end, carried here so the comparison needs no post-pass.
                LOG("recordprobe: [QUAD] swing#%ld n=%ld  +0x00(%.4f %.4f %.4f) "
                    "+0x0C(%.4f %.4f %.4f)  vs THIS start(%.4f %.4f %.4f) "
                    "end(%.4f %.4f %.4f)  vs PREV start(%.4f %.4f %.4f) "
                    "end(%.4f %.4f %.4f)",
                    swingsLogged, samplesThisSwing,
                    ra.a[0], ra.a[1], ra.a[2], ra.b[0], ra.b[1], ra.b[2],
                    ra.start[0], ra.start[1], ra.start[2],
                    ra.end[0], ra.end[1], ra.end[2],
                    havePrevA ? prevA.start[0] : 0.0f,
                    havePrevA ? prevA.start[1] : 0.0f,
                    havePrevA ? prevA.start[2] : 0.0f,
                    havePrevA ? prevA.end[0] : 0.0f,
                    havePrevA ? prevA.end[1] : 0.0f,
                    havePrevA ? prevA.end[2] : 0.0f);
                prevA = ra;
                havePrevA = true;
            }
        }
        Sleep(2);
    }
}

volatile LONG g_started = 0;
LONG          g_reported = 0;

// Resolve the player's health module and confirm its class. Returns 0 on any
// failure, having logged why - never a bare "not found".
uintptr_t ResolveHealthModule(size_t slot, const char* expectSubstr, char* clsOut,
                              size_t clsLen)
{
    const uintptr_t ent = weaponanchor::g_entityBase;
    if (!ent) {
        // NAME THE SETTING, do not just report the symptom. The first version of
        // this message said only "weaponanchor has not resolved one", which told
        // the user what was missing and not how to get it - and cost a whole run,
        // because the resolver is gated on a setting that had been deliberately
        // switched OFF as an exec-watch precaution.
        LOG("damagewatch: no confirmed player entity - cannot locate the health "
            "module, REFUSING to guess. The resolver lives behind "
            "**diag_player_movemod=1** (weaponanchor::FindPlayerMoveStyleViaWeaponAnchor "
            "is gated on DiagPlayerMovementModule); it position-correlates captured "
            "entities against the render camera and needs a few seconds in a level. "
            "SET diag_player_movemod=1 AND KEEP diag_bone_dump/diag_bone_blend_probe "
            "AT 0. That is safe: the resolver uses the exec watch (DR1) while this "
            "probe uses the DATA watch (DR0), and the two coexist by design - it is "
            "the bone probe, another DR1 client, that must stay off.");
        return 0;
    }
    uintptr_t mod = 0;
    if (!weaponanchor::ReadPtr(
            ent + weaponanchor::kModulesArrayOffset + slot * sizeof(uintptr_t), mod) ||
        !mod) {
        LOG("damagewatch: modules[%zu] on entity %p is null or unreadable",
            slot, reinterpret_cast<void*>(ent));
        return 0;
    }
    // VERIFY, DO NOT TRUST THE INDEX. ai_modules.def is 2020 data; this is EE.
    if (!RttiClassNameOf(mod, clsOut, clsLen)) {
        LOG("damagewatch: modules[%zu] = %p but its RTTI class could not be read - "
            "REFUSING to watch an unidentified pointer",
            slot, reinterpret_cast<void*>(mod));
        return 0;
    }
    if (!strstr(clsOut, expectSubstr)) {
        LOG("damagewatch: modules[%zu] = %p is class '%s', NOT '%s' - the 2020 "
            "ai_modules.def slot index does not hold on EE. REFUSING; find the "
            "right slot before watching anything.",
            slot, reinterpret_cast<void*>(mod), clsOut, expectSubstr);
        return 0;
    }
    return mod;
}

// ===================================================================
// R5 STEP 2 - THE RETURN-ADDRESS CENSUS. IT WRITES NOTHING.
// ===================================================================
//
// THE TWO QUESTIONS, and nothing else:
//   1. WHAT DOES TRAPPING THIS SITE COST? `IModelObject::GetMeshElemFromBoneID`
//      is an ENGINE export reached from 372 call sites in gamedll alone
//      (INV-64/INV-104) and its per-call rate is [UNKNOWN]. If it is a per-frame
//      path, row 3 of DESIGN_REGWRITE.md's site table dies here, cheaply.
//   2. WHICH RETURN ADDRESSES ARE THE MELEE ONES? The write's call-site gate has
//      to be built from measurement. INV-65 names the six caller FUNCTIONS; the
//      difference between a function and the return address inside it is a
//      `call` instruction nobody has located.
//
// >>> IT TAKES WatchExec CLIENT 0, SO IT REPLACES [BONEWATCH] - by design, and
// that is option (a) of DESIGN_REGWRITE.md section 7. There will be no
// *** BONE HIT *** lines while this is on. <<<

// Names the module an address belongs to, for the report. A return address that
// resolves to no module at all is a finding in itself, so it is printed as such
// rather than silently blanked.
void ModuleOf(uintptr_t addr, char* out, size_t outLen, uintptr_t* rvaOut)
{
    out[0] = '\0';
    if (rvaOut) { *rvaOut = 0; }
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) || !m) {
        strncpy_s(out, outLen, "NO MODULE", _TRUNCATE);
        return;
    }
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(m, path, MAX_PATH) == 0) {
        strncpy_s(out, outLen, "UNNAMED MODULE", _TRUNCATE);
        return;
    }
    const char* slash = strrchr(path, '\\');
    strncpy_s(out, outLen, slash ? slash + 1 : path, _TRUNCATE);
    if (rvaOut) { *rvaOut = addr - reinterpret_cast<uintptr_t>(m); }
}

void censusArm(bool* armed, DWORD* start, DWORD* lastTick, bool* done)
{
    // ---- refuse, BY NAME, on the one configuration that would silently lose ----
    if (DiagMeleeElemProbe) {
        LOG("damagewatch: [BONECENSUS] REFUSING TO ARM - diag_melee_elem_probe=1 "
            "holds the same WatchExec client 0 slot. Both cannot run. Set "
            "diag_melee_elem_probe=0 and relaunch. NOTHING is being watched.");
        *done = true;   // do not retry every second and flood the log
        return;
    }
    const HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!eng) {
        LOG("damagewatch: [BONECENSUS] engine_x64_rwdi.dll not mapped yet - "
            "retrying. NOTHING is armed.");
        return;
    }
    // Resolved BY EXPORT NAME, not by the RVA. INV-104's 0x143DC0 is the
    // cross-check, not the claim - camhook.cpp already resolves this exact
    // mangled name for the bone dump.
    const auto fn = reinterpret_cast<uintptr_t>(GetProcAddress(
        eng, "?GetMeshElemFromBoneID@IModelObject@@QEBAHE@Z"));
    if (!fn) {
        LOG("damagewatch: [BONECENSUS] GetProcAddress FAILED for "
            "?GetMeshElemFromBoneID@IModelObject@@QEBAHE@Z on engine - the "
            "export is not there under that name. NOTHING is armed, and a "
            "silent run below means THIS.");
        *done = true;
        return;
    }
    const uintptr_t rva = fn - reinterpret_cast<uintptr_t>(eng);
    LOG("damagewatch: [BONECENSUS] target %p = engine+0x%zX  (INV-104 read "
        "0x143DC0 out of the shipped DLL: %s)",
        reinterpret_cast<void*>(fn), rva,
        rva == 0x143DC0 ? "AGREES"
                        : ">>> DISAGREES - the binary is not the one INV-104 was "
                          "read from. The census still runs, on the LIVE address, "
                          "but update INV-104 <<<");

    dlvr::WatchExecSetStackFloatOffset(0);
    dlvr::WatchExecSetStackUseRsp(false);
    dlvr::WatchExecSetFieldCapture(0, 0);
    // 0 = UNLIMITED, explicitly. A hit cap would disarm silently mid-window and
    // everything measured after it would be the pacing of an unarmed game, which
    // reads as "this site costs nothing". The WINDOW is the bound.
    dlvr::WatchExecSetMaxHits(0);
    dlvr::WatchExecReturnCensusReset();
    dlvr::WatchExecReturnCensusEnable(true);
    if (!dlvr::WatchExecStart(fn)) {
        LOG("damagewatch: [BONECENSUS] WatchExecStart FAILED at engine+0x%zX - "
            "the slot is taken or the address is bad. NOTHING is being watched; "
            "a silent run means THIS, not an absence of calls. Status: %s",
            rva, dlvr::WatchExecStatus());
        dlvr::WatchExecReturnCensusEnable(false);
        *done = true;
        return;
    }
    *armed = true;
    *start = GetTickCount();
    *lastTick = *start;
    const long windowMs = static_cast<long>(MeleeCensusMs);
    LOG("damagewatch: [BONECENSUS] ARMED on engine+0x%zX, mode: %s. "
        ">>> THIS REPLACES [BONEWATCH] - there will be NO *** BONE HIT *** "
        "lines while it runs, by design. IT WRITES NOTHING. <<< "
        ">>> IF THE FRAME RATE COLLAPSES FROM HERE, THAT IS THE ANSWER TO "
        "QUESTION 1 - say so rather than quitting silently. <<<%s",
        rva,
        windowMs > 0 ? "TIMED WINDOW" : "SESSION (stays armed, full table every 30 s)",
        windowMs > 0
            ? "  *** WARNING: A TIMED WINDOW OPENS NOW, WHICH IS DURING STARTUP - "
              "THIS PROBE ARMS AS SOON AS THE ENGINE MODULE IS MAPPED, NOT WHEN "
              "COMBAT STARTS. It will close and latch long before the first "
              "zombie and CANNOT answer question 2. Set melee_census_ms=0. ***"
            : "");
}

void censusReport(const char* why, DWORD elapsedMs)
{
    unsigned long total = 0, distinct = 0, refusedNew = 0;
    dlvr::WatchExecReturnCensusStats(&total, &distinct, &refusedNew);
    unsigned long fires = 0;
    bool expired = false;
    int armedThreads = 0, liveThreads = 0;
    dlvr::WatchExecStats(&fires, nullptr, &expired, &armedThreads, &liveThreads);

    // >>> SAY THAT NOTHING WAS WRITTEN, AS A MEASUREMENT RATHER THAN AS AN
    // ARGUMENT FROM ABSENCE. <<< Step 3 is not built and nothing calls
    // WatchExecSetRegWrite, so these are three zeroes by construction - but
    // "we did not arm it" and "it did not fire" are different claims, and only
    // the second one is checkable. Printing them also keeps
    // WatchExecRegWriteStats referenced: the four arming functions are
    // link-stripped out of this build precisely because nothing calls them,
    // which is the strongest available guarantee that this binary cannot write
    // a register (verified by grep -a on the deployed DLL: every `regwrite ...`
    // arming string is ABSENT while both census strings are PRESENT).
    unsigned long rwApplied = 0, rwGated = 0, rwRefused = 0;
    dlvr::WatchExecRegWriteStats(&rwApplied, &rwGated, &rwRefused, nullptr);
    LOG("damagewatch: [BONECENSUS] register write-back: applied=%lu gatedOut=%lu "
        "refused=%lu  (step 3 is NOT BUILT - anything other than 0/0/0 here "
        "means something armed a write that nothing should be able to arm)",
        rwApplied, rwGated, rwRefused);

    const double secs = elapsedMs > 0 ? (elapsedMs / 1000.0) : 0.0;
    LOG("damagewatch: [BONECENSUS] %s after %lu ms: total=%lu  rate=%.1f/s  "
        "distinct=%lu  table-refused=%lu  | fires=%lu expired=%s "
        "armedThreads=%d liveThreads=%d",
        why, static_cast<unsigned long>(elapsedMs), total,
        secs > 0.0 ? (total / secs) : 0.0, distinct, refusedNew, fires,
        expired ? "YES" : "no", armedThreads, liveThreads);

    if (expired) {
        LOG("damagewatch: [BONECENSUS] *** THE WATCH EXPIRED. The hit cap was "
            "supposed to be 0/unlimited - the numbers above cover only the part "
            "of the window before it disarmed, and any frame-pacing reading "
            "after that point is of an UNARMED game.");
    }
    if (liveThreads > armedThreads) {
        LOG("damagewatch: [BONECENSUS] %d thread(s) exist that the breakpoint "
            "was never armed on (armed %d, live %d). Calls they make are "
            "INVISIBLE to this census and the totals are a lower bound.",
            liveThreads - armedThreads, armedThreads, liveThreads);
    }
    if (refusedNew > 0) {
        LOG("damagewatch: [BONECENSUS] *** THE TABLE FILLED. %lu distinct return "
            "address(es) had no room. THIS CENSUS IS INCOMPLETE and a gate list "
            "built from it would be too.", refusedNew);
    }
    if (total == 0) {
        LOG("damagewatch: [BONECENSUS] ZERO HITS. That is not 'the site is "
            "cheap' - check the ARMED line above exists and that armedThreads "
            "is non-zero first. A watch that never armed and a site that never "
            "runs produce the same 0 here, and only those two numbers "
            "distinguish them.");
        return;
    }

    dlvr::ReturnHit hits[dlvr::kReturnCensusMax];
    const int n = dlvr::WatchExecReturnCensusGet(hits, dlvr::kReturnCensusMax);
    LOG("damagewatch: [BONECENSUS] --- %d call site(s), as captured (return "
        "address = the instruction AFTER the call, which is what the write's "
        "gate takes) ---", n);
    for (int i = 0; i < n; ++i) {
        char mod[64] = {};
        uintptr_t rva = 0;
        ModuleOf(hits[i].ret, mod, sizeof(mod), &rva);
        LOG("damagewatch: [BONECENSUS]   ret=%p  %s+0x%zX  count=%llu  "
            "lastModel=%p  lastBone=%llu (0x%llX)",
            reinterpret_cast<void*>(hits[i].ret), mod, rva,
            static_cast<unsigned long long>(hits[i].count),
            reinterpret_cast<void*>(hits[i].lastRcx),
            static_cast<unsigned long long>(hits[i].lastRdx & 0xFFull),
            static_cast<unsigned long long>(hits[i].lastRdx));
    }
    LOG("damagewatch: [BONECENSUS] --- end. The melee sites are the ones whose "
        "counts move WHEN A BLOW LANDS; INV-65's six callers are "
        "sub_180D023E0, sub_180D28410, sub_180DB4410, sub_180DC7190, "
        "sub_180DC7630 and sub_180D00040, so a gamedll RVA inside one of those "
        "is the candidate. Do NOT take a bare address from this list as a melee "
        "site without that correlation. ---");
}

// How often the SESSION mode (melee_census_ms=0) reprints the full per-site
// table. Frequent enough that a blow landed between two reports is visible as a
// count that moved, rare enough not to bury the log.
const DWORD kCensusSessionReportMs = 30000;

void censusPoll(bool* armed, DWORD* start, DWORD* lastTick, bool* done)
{
    const DWORD now = GetTickCount();
    const DWORD elapsed = now - *start;
    const long windowMs = static_cast<long>(MeleeCensusMs);

    // A progress line every 2 s, so a window that ends up unusable is
    // attributable to the moment it became so rather than to the whole run.
    if (now - *lastTick >= 2000) {
        *lastTick = now;
        unsigned long total = 0, distinct = 0, refusedNew = 0;
        dlvr::WatchExecReturnCensusStats(&total, &distinct, &refusedNew);
        LOG("damagewatch: [BONECENSUS] +%lu ms  total=%lu  distinct=%lu  "
            "refused=%lu", static_cast<unsigned long>(elapsed), total, distinct,
            refusedNew);
    }

    if (windowMs <= 0) {
        // ===== SESSION MODE, AND IT EXISTS BECAUSE A TIMED WINDOW CANNOT
        // ANSWER QUESTION 2. =====
        //
        // This probe arms as soon as engine_x64_rwdi.dll is mapped, which is
        // early in startup - so a 15 s window opens on the LOADING SCREEN,
        // closes before the player reaches anything to hit, and latches. That is
        // not a hypothetical: the earlier damagewatch probe did exactly this and
        // the comment at the top of Thread() records it - "THE WINDOW CLOSED
        // BEFORE THE FIRST ZOMBIE", a one-shot probe whose window is chosen by
        // module-discovery timing rather than by when the event happens.
        //
        // So in session mode the census stays armed and REPRINTS THE WHOLE
        // TABLE every 30 s. The counts that move between two reports while the
        // player is swinging are the melee call sites - which is the entire
        // point, and a totals-only progress line cannot show it.
        static DWORD s_lastFull = 0;
        if (s_lastFull == 0) { s_lastFull = *start; }
        if (now - s_lastFull >= kCensusSessionReportMs) {
            s_lastFull = now;
            censusReport("SESSION REPORT (still armed)", elapsed);
        }
        return;
    }
    if (elapsed < static_cast<DWORD>(windowMs)) {
        return;
    }
    censusReport("WINDOW CLOSED", elapsed);
    dlvr::WatchExecStop();
    dlvr::WatchExecReturnCensusEnable(false);
    *armed = false;
    *done = true;
    LOG("damagewatch: [BONECENSUS] DISARMED. WatchExec client 0 is free again - "
        "but [BONEWATCH] is NOT re-armed in this session, because its own "
        "one-shot guard has not been reset. Relaunch with "
        "diag_melee_bone_census=0 to get the bone trap back.");
}

// ===================================================================
// [LOCREAD] - WHO READS m_LastHitLocation? IT WRITES NOTHING.
// ===================================================================
//
// THE QUESTION: INV-106 found no static consumer of +0x688 in gamedll - setter
// and no getter, while siblings have both - but could not rule out a function
// that reads ONLY that offset (74 displacement-matched candidates it cannot tell
// from ItemDescFirearm readers) or an engine-side accessor. If nothing reads it,
// INV-105's mismatch is bookkeeping and R5's write half is the element poke that
// already works (INV-68).
//
// HOW: the melee trap hands us a real HumanAIStateVector in RCX on every hit
// (INV-103). Take the first one, and arm a DR0 data breakpoint in READ-OR-WRITE
// mode at stateVec + 0x688.
//
// >>> THE POSITIVE CONTROL IS BUILT IN, AND IT IS THE WHOLE REASON THIS CAN
// PRODUCE A CLEAN NEGATIVE AT ALL. <<< We already know ONE writer:
// SetLastHitLocation, gamedll+0x2CF540 (INV-105/INV-106). If the watch is
// working it MUST appear. If it does not, the run is VOID and says so - "no
// readers" and "the instrument saw nothing" are otherwise the same empty list,
// which is the trap this project keeps paying for (INV-6).
//
// WHAT IT COSTS: WatchWritesStart arms DR0 across every thread ONCE (an arming
// cost, not a per-frame one), and the field is written once per melee hit. If
// something reads it per frame the frame rate will say so - and that would BE
// the answer. The data watch self-disarms at 400 hits and reports it.
constexpr size_t kLastHitLocation = 0x688;
// SetLastHitLocation's extent, for the positive control. A data breakpoint is
// TRAP-class - it fires AFTER the store - so the reported RIP is an instruction
// inside this function, not its entry.
constexpr uintptr_t kSetLocLo = 0x2CF540;
constexpr uintptr_t kSetLocHi = 0x2CF560;

void locreadReport(uintptr_t watched, uintptr_t stateVec, DWORD elapsedMs,
                   const dlvr::ExecHit* hits, int n, const char* why)
{
    dlvr::WriteHit w[64] = {};
    const int wn = dlvr::WatchWritesGet(w, 64);
    const bool active = dlvr::WatchWritesActive();

    // Is the body we are watching still being hit? The exec table aggregates, so
    // this is a running total for that state vector, and it is the denominator
    // for "nothing read it": no further hits means no further WRITES either, and
    // then an empty reader list says nothing at all.
    unsigned long long hitsOnWatched = 0;
    for (int i = 0; i < n; ++i) {
        if (hits[i].rcxValue == stateVec) { hitsOnWatched += hits[i].count; }
    }

    LOG("damagewatch: [LOCREAD] %s after %lu ms: watching %p "
        "(stateVec %p + 0x%zX) | distinct touchers=%d | watch %s | melee hits "
        "recorded on THIS body=%llu | status: %s",
        why, static_cast<unsigned long>(elapsedMs),
        reinterpret_cast<void*>(watched), reinterpret_cast<void*>(stateVec),
        kLastHitLocation, wn, active ? "ACTIVE" : "STOPPED/EXPIRED",
        hitsOnWatched, dlvr::WatchWritesStatus());

    bool sawWriter = false;
    for (int i = 0; i < wn; ++i) {
        char mod[64] = {};
        uintptr_t rva = 0;
        ModuleOf(w[i].rip, mod, sizeof(mod), &rva);
        const bool isWriter = (strstr(mod, "gamedll") != nullptr) &&
                              rva >= kSetLocLo && rva < kSetLocHi;
        if (isWriter) { sawWriter = true; }
        LOG("damagewatch: [LOCREAD]   rip=%p  %s+0x%zX  count=%llu%s",
            reinterpret_cast<void*>(w[i].rip), mod, rva,
            static_cast<unsigned long long>(w[i].count),
            isWriter ? "   <== SetLastHitLocation, THE KNOWN WRITER "
                       "(positive control)" : "");
    }

    if (!sawWriter) {
        LOG("damagewatch: [LOCREAD] *** VOID: THE KNOWN WRITER DID NOT APPEAR. *** "
            "SetLastHitLocation (gamedll+0x%zX..0x%zX) writes this exact address "
            "on every melee hit, so its absence means the WATCH is not seeing "
            "this address - not that nothing touches it. Do NOT read the list "
            "above as a result. Check: did any melee hit land on THIS body "
            "(count above), and did the watch stay ACTIVE?",
            kSetLocLo, kSetLocHi);
        return;
    }
    if (wn == 1) {
        LOG("damagewatch: [LOCREAD] >>> CLEAN NEGATIVE SO FAR: the ONLY thing "
            "touching m_LastHitLocation is its own writer. Nothing reads it. "
            "That is INV-106 confirmed live - but it is only as strong as the "
            "hit count on this body (%llu) and the time armed (%lu ms). <<<",
            hitsOnWatched, static_cast<unsigned long>(elapsedMs));
    } else {
        LOG("damagewatch: [LOCREAD] >>> %d DISTINCT TOUCHERS, so at least one is "
            "NOT the writer. INV-106's static negative is WRONG and the reader(s) "
            "are named above - that changes R5's write half. <<<", wn);
    }
}

void locreadTick(const dlvr::ExecHit* hits, int n)
{
    static bool      s_armed = false;
    static bool      s_done = false;
    static uintptr_t s_watched = 0;
    static uintptr_t s_stateVec = 0;
    static DWORD     s_start = 0;
    static DWORD     s_lastReport = 0;
    static DWORD     s_lastRefusal = 0;

    if (s_done) { return; }

    if (!s_armed) {
        uintptr_t sv = 0;
        for (int i = 0; i < n; ++i) {
            if (hits[i].rcxValue) { sv = hits[i].rcxValue; break; }
        }
        if (!sv) {
            return;   // no melee hit yet - nothing to watch. Not a failure.
        }
        const uintptr_t addr = sv + kLastHitLocation;
        dlvr::WatchWritesSetIncludeReads(true);
        if (!dlvr::WatchWritesStart(addr)) {
            // Rate-limited, and it does NOT latch: DR0's other owner is the bone
            // probe's exec client 1, which is a ONE-SHOT model capture that stops
            // itself - so the slot frees up and the next window can take it. A
            // probe that gave up on the first refusal would report nothing and
            // look identical to one that found nothing.
            const DWORD now = GetTickCount();
            if (now - s_lastRefusal >= 5000) {
                s_lastRefusal = now;
                LOG("damagewatch: [LOCREAD] could not arm at %p - %s. NOT giving "
                    "up: DR0's other owner is the bone probe's exec client 1, "
                    "which is one-shot and releases the register. Retrying.",
                    reinterpret_cast<void*>(addr), dlvr::WatchWritesStatus());
            }
            return;
        }
        s_armed = true;
        s_watched = addr;
        s_stateVec = sv;
        s_start = GetTickCount();
        s_lastReport = s_start;
        LOG("damagewatch: [LOCREAD] ARMED on %p = stateVec %p + 0x%zX, "
            "READ-OR-WRITE, for %ld ms. IT WRITES NOTHING. >>> THE WATCH IS 4 "
            "BYTES WIDE, so it covers m_LastHitLocation.x ONLY - a vec3 consumer "
            "touches .x, which is why that is enough. <<< >>> IT WATCHES ONE "
            "BODY: if this zombie dies its memory can be recycled and a later "
            "hit may belong to a different object, so read the 'melee hits "
            "recorded on THIS body' count before trusting a long window. <<< "
            "Status: %s",
            reinterpret_cast<void*>(s_watched), reinterpret_cast<void*>(sv),
            kLastHitLocation, static_cast<long>(LocReadWindowMs),
            dlvr::WatchWritesStatus());
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD elapsed = now - s_start;
    const long windowMs = static_cast<long>(LocReadWindowMs);

    if (now - s_lastReport >= 5000) {
        s_lastReport = now;
        locreadReport(s_watched, s_stateVec, elapsed, hits, n, "PROGRESS");
    }

    const bool expired = !dlvr::WatchWritesActive();
    const bool windowOver = (windowMs > 0) &&
                            (elapsed >= static_cast<DWORD>(windowMs));
    if (expired || windowOver) {
        locreadReport(s_watched, s_stateVec, elapsed, hits, n,
                      expired ? "THE WATCH STOPPED (hit cap or explicit stop)"
                              : "WINDOW CLOSED");
        dlvr::WatchWritesStop();
        dlvr::WatchWritesSetIncludeReads(false);
        s_armed = false;
        s_done = true;
        LOG("damagewatch: [LOCREAD] DISARMED. DR0 is free again.");
    }
}

DWORD WINAPI Thread(LPVOID)
{
    // Nothing here runs unless explicitly asked for - this takes the single
    // process-wide WatchExec slot.
    //
    // REPEATING WINDOWS, NOT ONE-SHOT (2026-09-11). The first version armed
    // exactly once, as soon as weaponanchor published a health module, and then
    // latched g_reported whatever the result. MEASURED COST: on the 11:10 run it
    // armed at 64 s and disarmed at 84 s after launch, reported "0 distinct
    // writer(s)", and latched - while the session ran on to 4m47s with the user
    // still sprinting at 4m18s. The user needs ~30 s from spawn to reach combat
    // and the level load eats the rest, so THE WINDOW CLOSED BEFORE THE FIRST
    // ZOMBIE. A one-shot probe whose single window is chosen by module-discovery
    // timing rather than by when the event happens cannot observe a user-driven
    // event, and its zero is indistinguishable from a real negative - which is
    // exactly the ambiguous-silence trap this project keeps paying for.
    //
    // So: keep re-arming, and only latch when writers are actually FOUND.
    unsigned long windowNo = 0;
    // ===== R5 STEP 2: THE RETURN-ADDRESS CENSUS ON THE BONE ARGUMENT =====
    //
    // Deliberately ABOVE the DiagDamageWatch gate below, so this can run in
    // FLATSCREEN. It needs no weaponanchor, no ILevel and no horizon lock - only
    // the engine module - and the cost question it answers (what does trapping
    // GetMeshElemFromBoneID cost?) is answerable without a headset. The MELEE
    // return addresses do need combat, but flatscreen combat is combat.
    // PROBE_DEPS.md carries the row.
    static bool  s_censusArmed = false;
    static bool  s_censusDone = false;
    static DWORD s_censusStart = 0;
    static DWORD s_censusLastTick = 0;
    for (;;) {
        if (DiagMeleeBoneCensus && !s_censusArmed && !s_censusDone) {
            censusArm(&s_censusArmed, &s_censusStart, &s_censusLastTick,
                      &s_censusDone);
        } else if (s_censusArmed) {
            censusPoll(&s_censusArmed, &s_censusStart, &s_censusLastTick,
                       &s_censusDone);
        }
        // ===== R5 STEP 3: THE HIT-RECORD PROBE, ALSO ABOVE THE GATE =====
        //
        // It needs the melee input thunk (for g_meleeSelf) and nothing else, so
        // it must not inherit diag_damage_watch's exec-watch slot just to get an
        // install. InstallMeleeTrigger() is idempotent - it returns immediately
        // once g_origMeleeInput is set - and retries here once a second until
        // RttiVtableOf("WeaponMeleeController") resolves, which it cannot do
        // before a controller exists.
        // R5 step 4. Idempotent, and retried here for the same reason the
        // trigger is: the gamedll may not be mapped on the first pass. It is a
        // DIFFERENT address from the element hook at 0xC4D920 with its own
        // trampoline, so the two do not contend.
        if (DiagMeleePoseProbe) { dlvr::MeleePoseHookInstall(); }
        // SPEC-VR-SWING-ATTACK-2026-09-29. Idempotent; retried once a second until the WeaponMeleeController
        // vtable resolves. Its own install path - NOT behind diag_damage_watch (U-31's dependency note).
        if (dlvr::MeleeSwingMode != 0) { dlvr::SwingInstall(); }
        if (dlvr::MeleeSwingMode != 0) { dlvr::SwingStatsTick(); }
        // Melee throw (spec 2026-09-30-melee-throw-design): unconditional - it logs NOT ARMED naming the setting, once
        // per change. Cheap: a few bool tests; GetModuleHandleA only until the game functions resolve.
        dlvr::ThrowTick();
        // Lockpick position P1 (lockpitch.cpp): one vtable slot swap, once; retried here until the gamedll is mapped.
        dlvr::LockPitchInstall();
        // The lower body turns with the stick when looking down (pelvisturn.cpp, INV-749, trace 20261119_1530): one vtable slot swap,
        // once; retried here until the gamedll is mapped. A fix, no key.
        dlvr::PelvisTurnInstall();
        // The dialogue view offset (dlgview.cpp, INV-619): one vtable slot swap, once; retried here until the gamedll is mapped.
        dlvr::DlgViewInstall();
        // The crossbow bolt wheel (boltwheel.cpp, INV-678): one detour + one vtable slot swap, once; retried here until the gamedll is
        // mapped. manual_reload=0: one bool test and one NOT INSTALLED line ever.
        dlvr::BoltWheelInstall();
        // Inventory keep-built (invkeep.cpp, INV-526 addendum 2026-11-14): two call-site rewrites + three vtable slot swaps, once;
        // retried here until the gamedll is mapped. A fix, no key.
        dlvr::InvKeepInstall();
        dlvr::BuggyWheelInstall();   // buggy_hand_steer - the key + the gripped wheel (plan 2026-11-10-buggy-wheel-and-key)
        // The cutscene look (cinlook.cpp, INV-620 (a)) and side bars (cinbars.cpp, (b)): one vtable slot swap each, once; retried here
        // until the gamedll is mapped.
        dlvr::CinLookInstall();
        dlvr::CinBarsInstall();
        // anim_suppress_melee_idle (INV-319): refusal by name only - a few bool tests.
        dlvr::MeleeIdleSuppressTick();
        if (DiagMeleeRecordProbe) {
            InstallMeleeTrigger();
            if (InterlockedExchange(&g_recordProbeStarted, 1) == 0) {
                const HANDLE th = CreateThread(nullptr, 0, RecordProbeThread,
                                               nullptr, 0, nullptr);
                if (!th) {
                    // A discarded CreateThread failure is a probe that is simply
                    // absent from the log with no reason given.
                    LOG("recordprobe: *** CreateThread FAILED, GetLastError=%lu. "
                        "NOTHING WILL BE SAMPLED THIS RUN. ***", GetLastError());
                    InterlockedExchange(&g_recordProbeStarted, 0);
                } else {
                    CloseHandle(th);
                    LOG("recordprobe: sampler thread started - read-only, 2 ms, "
                        "no slot and no debug register. diag_damage_watch is %s "
                        "and is NOT required by this probe.",
                        DiagDamageWatch ? "ON" : "off");
                }
            }
        }
        if (!DiagDamageWatch) {
            Sleep(1000);
            continue;
        }
        // Set ONLY on a window that actually named a writer - see the end of the
        // loop. A zero-writer window now costs one more window, not the run.
        if (g_reported) {
            Sleep(2000);
            continue;
        }
        HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!gamedll) {
            Sleep(2000);
            continue;
        }

        // ===== REFUSE TO ARM WHILE THE BONE PROBE HOLDS THE SLOT =====
        //
        // MEASURED 2026-09-10, and it invalidated an entire run. This watch
        // armed ONCE on gamedll+0x2A2830 across 167 threads - and the bone probe
        // then re-armed the SAME single slot on gamedll+0xC62AC0
        // (CorrectItemPosition) TEN times inside our 20 s window, silently
        // overwriting our breakpoint. The result was "0 distinct hits" while the
        // user was landing light hits, heavy hits and kills: a pure instrument
        // failure that reads exactly like the negative finding "AddDamageToBody
        // is not the melee damage path". Reporting it as a negative would have
        // discarded the correct target.
        //
        // THE COMPETING CLIENT IS NOT IN THIS PROJECT'S DOCUMENTED LIST. That
        // list names diag_world_writer, diag_visibility_probe,
        // diag_player_movemod, diag_watch_line_add and diag_kill_physics_debug -
        // all five were off, and verified off, before the run. The bone probe /
        // weapon anchor is the SIXTH, and it is driven by diag_bone_dump and
        // diag_bone_blend_probe: the two MASTER ENABLES the rig cannot run
        // without. So this is a structural conflict, not a stray diagnostic
        // somebody forgot to switch off.
        //
        // Option A, chosen deliberately over making the bone probe yield: refuse
        // to arm, and say so WITH the exact remedy. Losing the rig for one scan
        // is cheap, and melee still works so the capture stays valid. (The bone
        // probe re-arming ten times for what is a ONE-SHOT model-pointer capture
        // looks like a latent bug in its own right; fixing that later would make
        // this guard unnecessary rather than wrong.)
        // >>> CORRECTED 2026-09-17. THE PREDICATE IS "WOULD THE BONE PROBE TAKE
        // CLIENT 0", NOT "IS THE RIG ON". <<<
        //
        // D-45 gave the bone probe a SECOND exec client on DR0. With
        // rig_exec_client2=1 it routes to WatchExec2Start and never touches
        // client 0 at all - and on failure it retries CLIENT 1, it does not fall
        // back to client 0 (camhook.cpp:6362-6376). This guard was not updated
        // with that refactor, so it went on refusing against a condition the
        // refactor had removed.
        //
        // MEASURED, and it cost the run it exists to protect: 2026-09-17 11:10,
        // the coexistence run. `watchexec2: watching execution at ... CLIENT 1,
        // on DR0. Client 0 (DR1) is unaffected` armed happily while THIS line
        // refused 16 times in 80 s. Client 0 armed ZERO times, `[ELEM] ARMED`
        // never printed, and the run was void - the rig and the trap were
        // already coexisting and this predicate was the only thing stopping it.
        if ((DiagBoneDump || DiagBoneBlendProbe) && !RigExecClient2) {
            LOG("damagewatch: REFUSING TO ARM - the bone probe holds the single "
                "process-wide WatchExec slot (it re-arms gamedll+0xC62AC0, "
                "CorrectItemPosition, repeatedly). It WILL overwrite this "
                "breakpoint, and the run then reports 0 hits while damage is "
                "actually happening - indistinguishable from a real negative. "
                "TWO REMEDIES, AND THE FIRST IS NOW THE RIGHT ONE: (1) set "
                "rig_exec_client2=1 - the bone probe moves to CLIENT 1 on DR0 "
                "and the two coexist in one session (D-45); or (2) set "
                "diag_bone_dump=0 AND diag_bone_blend_probe=0, which reverts the "
                "rig to animation for that run - melee still works, so the "
                "capture is valid - then set both back to 1 afterwards.");
            Sleep(5000);
            continue;
        }

        // RULE 0: THE BYPASS MUST SAY SOMETHING. Silence on this path is exactly
        // what made the stale predicate expensive - `watchexec2:` arming in the
        // log looked like success while client 0 never armed at all. Said ONCE,
        // on the first pass that takes it, naming both clients so the next
        // reader can tell a coexistence run from a half-armed one without
        // re-deriving it.
        static bool s_saidCoexist = false;
        if (!s_saidCoexist && (DiagBoneDump || DiagBoneBlendProbe)) {
            s_saidCoexist = true;
            LOG("damagewatch: NOT refusing - the bone probe is enabled but "
                "rig_exec_client2=1, so it holds CLIENT 1 (DR0) and this watch "
                "takes CLIENT 0 (DR1). BOTH must appear in this log: "
                "`watchexec2: watching execution at` for the rig and "
                "`watchexec: watching execution at` for the trap. If either is "
                "missing, or either says REFUSING, this is NOT a coexistence run "
                "whatever else it produces.");
        }

        // ===== DATA WATCH ON THE PLAYER'S OWN HEALTH =====
        //
        // Direction reversed on purpose: instead of guessing which function
        // DEALS damage, watch the health value and let it name every function
        // that writes it. LET A ZOMBIE HIT YOU. The receiving side is the same
        // pipeline, and this needs no hypothesis about which class or vtable
        // slot is involved - which is precisely what three exec runs got wrong.
        // TAKE THE HEALTH MODULE weaponanchor PUBLISHED, rather than re-deriving
        // it from a player entity.
        //
        // The previous version required weaponanchor to have identified THE
        // PLAYER first - and that resolver failed on its own terms (nearest
        // entity at distance 654 with a null modules[] slot, the other 63 never
        // examined). Identifying the player is a separate and documented-fragile
        // problem, and THIS PROBE DOES NOT NEED IT: the writers of health are
        // the same pipeline whichever body takes the hit. So weaponanchor now
        // publishes the closest entity with an RTTI-confirmed HealthModule,
        // player or not, and we watch that.
        // ===== PICK THE BODY BY MEASUREMENT, NOT BY PROXIMITY =====
        //
        // WHY THIS REPLACED "watch the nearest HealthModule": two runs reported
        // 0 writers, and the 09-11 window-close re-read finally said why - the
        // watched value sat FROZEN at 114.1406 across seven windows and 2+
        // minutes while the user was taking AND dealing damage. The watch was
        // never broken; it was aimed at an inert prop that "closest entity
        // carrying a HealthModule" had selected once and never revisited.
        //
        // Proximity is a guess about which body matters. So: poll EVERY
        // published candidate's health field as plain guarded reads - no debug
        // register needed, DR0 stays free - and arm the watch only on an address
        // whose value has been OBSERVED to move. That turns the choice into a
        // measurement, and it also means a zero from the armed window can no
        // longer be blamed on the target.
        // The trigger must arm BEFORE the candidate gate, because the gate now
        // depends on it: weaponanchor waits for the first swing, and the swing
        // is only seen if this hook is already in place. Installing it after the
        // gate would deadlock the two against each other.
        const bool triggerOn = InstallMeleeTrigger();

        const LONG candN = weaponanchor::g_healthCandCount;
        if (candN <= 0) {
            LOG("damagewatch: no health-module candidates published yet - REFUSING "
                "to guess. Trigger is %s. weaponanchor now WAITS FOR THE FIRST "
                "MELEE SWING before capturing (the capture is one-shot, and "
                "scanning in a safe zone would freeze a list of NPCs who never "
                "take damage), so THIS LINE IS EXPECTED UNTIL YOU SWING AT THE "
                "FIGHT. Needs diag_player_movemod=1. Watch for 'first melee swing "
                "seen' then 'health-module CANDIDATE(S) published'; if it instead "
                "reports 'NO captured entity had a valid HealthModule', the EE "
                "slot indices differ from ai_modules.def's 22/23.",
                triggerOn ? "ARMED" : "NOT ARMED - see its reason above; without "
                                      "it the swing can never be seen and this "
                                      "will never clear");
            Sleep(5000);
            continue;
        }

        // ===== ONE-SHOT: READ BACK THE LIVE MESH-CULL VALUES =====
        //
        // Settles whether a <map>.scr varlist override actually reaches the
        // engine. Experiment A (f_engine_mesh_cull_size 4 -> 40,
        // f_engine_mesh_cull_dist_threshold 200 -> 50) produced no clearly
        // visible change, which leaves its negative result UNINTERPRETABLE: a
        // 10x cull change that did nothing looks identical to a setting that
        // never applied.
        //
        // Deliberately NOT via camhook's lodoverride harness: that acquires its
        // ILevel with WatchExecStart, i.e. it is ANOTHER DR1 client, and arming
        // it during a damage run would re-create exactly the exec-watch conflict
        // that already invalidated one run. The player entity is already
        // resolved here, and IGSObject::GetLevel is a plain non-virtual export,
        // so no debug register is needed at all.
        //
        // The internal getters take the CLevel IMPL, not the ILevel: read from
        // ILevel::SetMeshCullSize, which does `v2 = *((_QWORD*)this + 1)` and
        // then indexes v2+768. So impl == *(ILevel + 8).
        {
            static LONG s_cullReported = 0;
            const uintptr_t playerEntForLevel = weaponanchor::g_entityBase;
            if (playerEntForLevel && InterlockedCompareExchange(&s_cullReported, 1, 0) == 0) {
                HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
                typedef void* (*GetLevelFnLocal)(void*);
                typedef float (*GetFloatFnLocal)(uintptr_t);
                GetLevelFnLocal getLevelFn = eng ? reinterpret_cast<GetLevelFnLocal>(
                    GetProcAddress(eng, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ")) : nullptr;
                if (!eng || !getLevelFn) {
                    LOG("damagewatch: cull readback skipped - IGSObject::GetLevel "
                        "not resolvable");
                } else {
                    void* lvl = nullptr;
                    bool got = false;
                    __try {
                        lvl = getLevelFn(reinterpret_cast<void*>(playerEntForLevel));
                        got = true;
                    } __except (EXCEPTION_EXECUTE_HANDLER) { got = false; }
                    uintptr_t impl = 0;
                    if (got && lvl) {
                        __try {
                            impl = *reinterpret_cast<uintptr_t*>(
                                reinterpret_cast<uintptr_t>(lvl) + 8);
                        } __except (EXCEPTION_EXECUTE_HANDLER) { impl = 0; }
                    }
                    if (!impl) {
                        LOG("damagewatch: cull readback - could not reach the CLevel "
                            "impl from ILevel=%p (entity %p)", lvl,
                            reinterpret_cast<void*>(playerEntForLevel));
                    } else {
                        const uintptr_t engBase = reinterpret_cast<uintptr_t>(eng);
                        GetFloatFnLocal getCullSize =
                            reinterpret_cast<GetFloatFnLocal>(engBase + 0x2B2040);
                        GetFloatFnLocal getCullDist =
                            reinterpret_cast<GetFloatFnLocal>(engBase + 0x2B20B0);
                        // ===== [VARLIST] - READ BACK EVERY RANGE WE EDIT =====
                        //
                        // WHY THIS REPLACED THE TWO-VALUE [CULLREAD]. The 20 m
                        // objects+terrain cut still left "things rendering past
                        // 20 m", which makes that run's result UNINTERPRETABLE
                        // for the same reason Experiment A was: a cut that did
                        // not reach the engine is indistinguishable from a cut
                        // that reached it and did nothing. Reading ONE key back
                        // proves that key applied and says nothing about the
                        // others - and we are now editing six.
                        //
                        // The index map is read straight out of level.cpp's
                        // varlist binder (engine+0x2B2690), which stores each
                        // entry pointer at CLevel_impl + 8*index:
                        //   77 terrain   78 objects  79 fx   80 decals
                        //   81 dynlights 82 envprobes
                        //   96 mesh_cull_size  97 mesh_cull_dist_threshold
                        //   98 shadow_mesh_cull_size   250 simrange_fx
                        //
                        // Value fetch mirrors the engine's own getters exactly:
                        //   blobOwner = *(impl + 240)
                        //   P         = sub_18003E7B0(blobOwner)
                        //   off       = *(DWORD*)(*(impl + 8*idx) + 80)
                        //   value     = *(float*)(*(QWORD*)P + off)
                        // with the same bounds check the getters perform
                        // (*(DWORD*)(P+8) is the blob size).
                        struct VarRow { unsigned idx; const char* name; };
                        static const VarRow kVars[] = {
                            {  77, "f_engine_visrange_terrain"          },
                            {  78, "f_engine_visrange_objects"          },
                            {  79, "f_engine_visrange_fx"               },
                            {  80, "f_engine_visrange_decals"           },
                            {  81, "f_engine_visrange_dynlights"        },
                            {  82, "f_engine_visrange_envprobes"        },
                            {  96, "f_engine_mesh_cull_size"            },
                            {  97, "f_engine_mesh_cull_dist_threshold"  },
                            {  98, "f_engine_shadow_mesh_cull_size"     },
                        };
                        typedef uintptr_t (*BlobFn)(uintptr_t);
                        BlobFn blobFn =
                            reinterpret_cast<BlobFn>(engBase + 0x3E7B0);
                        LOG("damagewatch: [VARLIST] live level varlist values - "
                            "these are what the ENGINE holds, so a value that does "
                            "not match the shipped .scr means the override did NOT "
                            "reach it and any result from that run is VOID:");
                        for (size_t vi = 0; vi < sizeof(kVars)/sizeof(kVars[0]); ++vi) {
                            float val = -1.0f;
                            bool ok = false;
                            __try {
                                const uintptr_t owner =
                                    *reinterpret_cast<uintptr_t*>(impl + 240);
                                const uintptr_t entry = *reinterpret_cast<uintptr_t*>(
                                    impl + 8 * static_cast<uintptr_t>(kVars[vi].idx));
                                if (owner && entry) {
                                    const uintptr_t P = blobFn(owner);
                                    const unsigned off =
                                        *reinterpret_cast<unsigned*>(entry + 80);
                                    if (P && off + 4 <= *reinterpret_cast<unsigned*>(P + 8)) {
                                        const uintptr_t base = *reinterpret_cast<uintptr_t*>(P);
                                        if (base) {
                                            val = *reinterpret_cast<float*>(base + off);
                                            ok = true;
                                        }
                                    }
                                }
                            } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                            if (ok && kVars[vi].idx == 96) {
                                LOG("damagewatch: [VARLIST]   %-34s = %10.4f   "
                                    "(= %.3f%% of screen height)",
                                    kVars[vi].name, static_cast<double>(val),
                                    static_cast<double>(val) * 100.0 / 720.0);
                            } else {
                                LOG("damagewatch: [VARLIST]   %-34s = %10.4f%s",
                                    kVars[vi].name, static_cast<double>(val),
                                    ok ? "" : "   <== UNREADABLE");
                            }
                        }
                    }
                }
            }
        }

        // ---- build the scan target list ----
        //
        // >>> CLASS-FILTERED SINCE THE 16:09 RUN. THIS IS THE DEFECT THAT RUN
        //     FOUND, AND IT PRODUCED 38 CONFIDENT FALSE POSITIVES. <<<
        //
        // (a) previously took EVERY non-null module on the player entity - all
        // 64 slots, whatever they were. That made sense for the BLOCK-DIFF,
        // where casting a wide net WAS the method. It is actively wrong for a
        // FIXED-OFFSET poll: reading +0xC8 "m_HealthArmR" on a MimicModule just
        // reads whatever float happens to live at that offset in an unrelated
        // object. The 16:09 run reported damage on MimicModule (17x),
        // StandingUpModule (8x), MovementModule (6x), WeaponModule, IdleModule,
        // HumanLogicModule and TrackingModule - and on ZERO health modules -
        // with values like 6046 -> -1351, 0 -> -4.3e12 and one at -9.7e37.
        // Every one of them was noise dressed up as a hit.
        //
        // The offsets are only meaningful on the classes that DECLARE them, so
        // the target list is now restricted to exactly those three. Anything
        // else is not a health module and its +0xC8 is not health.
        auto isHealthClass = [](const char* c) -> bool {
            return c && (strstr(c, "BodyPartsHealthModule") ||
                         strstr(c, "ArmorHealthModule")     ||
                         strstr(c, "HealthModule"));
        };

        g_scanTargetCount = 0;
        int rejectedByClass = 0;
        const uintptr_t playerEnt = weaponanchor::g_entityBase;
        if (playerEnt) {
            for (unsigned s = 0; s < 64 && g_scanTargetCount < kMaxScanTargets; ++s) {
                uintptr_t m = 0;
                if (!weaponanchor::ReadPtr(
                        playerEnt + weaponanchor::kModulesArrayOffset +
                            s * sizeof(uintptr_t), m) || !m) {
                    continue;
                }
                char cls[96];
                if (!RttiClassNameOf(m, cls, sizeof(cls))) {
                    strncpy_s(cls, sizeof(cls), "<no rtti>", _TRUNCATE);
                }
                // No RTTI name means we CANNOT establish it is a health module,
                // and "cannot establish" is a reject, not a maybe.
                if (!isHealthClass(cls)) { ++rejectedByClass; continue; }
                ScanTarget& t = g_scanTargets[g_scanTargetCount];
                t.entity = playerEnt;
                t.base   = m;
                t.slot   = s;
                t.vptr   = 0;
                weaponanchor::ReadPtr(m, t.vptr);
                strncpy_s(t.cls, sizeof(t.cls), cls, _TRUNCATE);
                ++g_scanTargetCount;
            }
        }
        // (b) plus every other entity's health modules, so a zombie we hit can
        //     answer even if the player never takes a scratch.
        for (LONG i = 0; i < candN && g_scanTargetCount < kMaxScanTargets; ++i) {
            const uintptr_t m = weaponanchor::g_healthCands[i].module;
            bool dup = false;
            for (int k = 0; k < g_scanTargetCount; ++k) {
                if (g_scanTargets[k].base == m) { dup = true; break; }
            }
            if (dup) { continue; }
            // Same filter as (a). weaponanchor selects these by SLOT INDEX
            // (22/23 from ai_modules.def) and a slot index is not a class - if
            // the EE layout differs, that slot holds something else entirely.
            if (!isHealthClass(weaponanchor::g_healthCands[i].cls)) {
                ++rejectedByClass;
                continue;
            }
            ScanTarget& t = g_scanTargets[g_scanTargetCount];
            t.entity = weaponanchor::g_healthCands[i].entity;
            t.base   = m;
            t.slot   = weaponanchor::g_healthCands[i].slot;
            t.vptr   = 0;
            weaponanchor::ReadPtr(m, t.vptr);
            strncpy_s(t.cls, sizeof(t.cls), weaponanchor::g_healthCands[i].cls, _TRUNCATE);
            ++g_scanTargetCount;
        }
        if (g_scanTargetCount == 0) {
            LOG("damagewatch: no scan targets - neither a player entity nor any "
                "health candidate resolved. Needs diag_player_movemod=1.");
            Sleep(5000);
            continue;
        }

        ++windowNo;

        // (the trigger was already armed above, before the candidate gate)

        // ================= KNOWN-OFFSET POLL, 2026-09-15 =================
        //
        // THE BLOCK-DIFF IS GONE. It was a memory scan looking for a field whose
        // offset the BINARY ALREADY DECLARES, and it cost three runs and produced
        // two confident false positives (AttractorModule+0x098, a pointer being
        // nulled by a linked-list unlink; IdleModule+0x010, a counter resetting to
        // zero). Both beat every real health field because the player's ~28 modules
        // churn constantly and the diff had no way to prefer health.
        //
        // WHERE THE OFFSETS COME FROM - read, not guessed:
        // sub_1802848F0 (gamedll RVA 0x2848F0) is BodyPartsHealthModule::
        // RegisterRTTI(), and it registers its own fields BY NAME with their
        // offsets:
        //     +0xC8 m_HealthHead   +0xCC m_HealthLegR  +0xD0 m_HealthLegL
        //     +0xD4 m_HealthArmR   +0xD8 m_HealthArmL
        //     +0xDC m_BodyCutType  +0xE0 m_CutOffBones +0xE4 m_BodyHoles
        //     +0xE8 m_DestroyedBones +0xEC m_HeadSmashed +0xED m_HeadSmashedInHalf
        //     +0xF0 m_CutOffTypes
        //
        // AND THIS EXPLAINS A THREE-RUN MYSTERY. The old watch sat on +0x78
        // (m_HealthBody, inherited from the base HealthModule) and recorded that it
        // "never moved under damage across three runs". Correct - and not a bug in
        // the offset. Zombies are BodyPartsHealthModule, so damage lands on the
        // PER-LIMB fields above, never on the base body field. The offset was right;
        // the FIELD was wrong.
        //
        // ⚠ THIS BLOCK USED TO SAY "a decrease in any of these IS a damage event
        // BY CONSTRUCTION". THAT WAS FALSE AND IT COST THE 2026-09-18 RUN.
        // A free, a death reset and a level unload are all decreases: that run
        // logged 46 "*** DAMAGE ***" lines and not one was a blow - 44 of them
        // were `Level OnDestroy <wasteland>` reaching seven modules in 5 ms.
        // The offsets below are still right and still read from the registrar
        // rather than scanned; what was wrong was believing the SIGN alone.
        // Two guards now stand in front of the report - a vptr identity check
        // and an all-fields-to-zero shape check - and they are down at the poll
        // loop, not here.
        struct HealthField { size_t off; const char* name; };
        static const HealthField kFields[] = {
            { 0x0C8, "m_HealthHead" }, { 0x0CC, "m_HealthLegR" },
            { 0x0D0, "m_HealthLegL" }, { 0x0D4, "m_HealthArmR" },
            { 0x0D8, "m_HealthArmL" },
            // The base-class body health, kept so a plain HealthModule (which has no
            // limb fields) can still answer, and so "which field moved" stays a
            // measurement rather than an assumption.
            { 0x078, "m_HealthBody (base)" },
        };
        constexpr int kFieldCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));

        int okCount = 0;
        static float prevVal[kMaxScanTargets][kFieldCount];
        static bool  prevOk[kMaxScanTargets][kFieldCount];
        // One *** STALE MODULE *** line per target per window, not per sample:
        // the poll runs at 10 Hz and a dead body would otherwise take the log
        // lock several times a second for the rest of the window. INV-9 is the
        // entry about a throttle that was not one.
        static bool  staleSaid[kMaxScanTargets];
        memset(staleSaid, 0, sizeof(staleSaid));
        for (int i = 0; i < g_scanTargetCount; ++i) {
            bool any = false;
            for (int k = 0; k < kFieldCount; ++k) {
                float v = 0.0f;
                prevOk[i][k] = ReadFieldFloat(
                    g_scanTargets[i].base + kFields[k].off, v);
                prevVal[i][k] = v;
                if (prevOk[i][k]) { any = true; }
            }
            if (any) { ++okCount; }
        }
        // ================= [BONEWATCH] - WATCH THE BONE SETTER =================
        //
        // The poll below derives a pointer chain and hopes to catch a counter
        // change. This does neither: it traps the one-line setter that writes
        // m_LastHitBone and reads its ARGUMENTS. RCX is the HumanAIStateVector,
        // RDX is the bone. The game hands us both.
        //
        // SEQUENCING, because there are SEVEN WatchExec clients and ONE global
        // slot: weaponanchor's scan uses it transiently and has already finished
        // by here; arming this sets g_damageWatchArmed, which makes weaponanchor
        // stand down from re-capturing; and the health-field arming path below
        // is disabled outright so it can never contend. Two clients fighting
        // over this slot is what invalidated the 2026-09-10 melee run.
        // ============ [ELEM] THE MELEE ELEMENT PROBE / OVERRIDE ============
        //
        // Takes the SINGLE WatchExec slot, so it REPLACES [BONEWATCH] rather
        // than running alongside it. That is the trade: we stop seeing the bone
        // at the setter and instead see - and optionally rewrite - the element
        // at its source, which is upstream of all nine attack paths.
        //
        // TWO STAGES, and the first is READ-ONLY:
        //   diag_melee_elem_probe=1, melee_force_elem=-1  -> observe only.
        //     Logs the record pointer, the model at [a3+0] and the element at
        //     [a3+8] for every melee hit. That gives the element<->bone mapping
        //     empirically, which nothing has ever logged.
        //   melee_force_elem=<n>                          -> ARMS THE WRITE.
        //     Every melee hit's element becomes n.
        //
        // >>> THE SECOND STAGE IS THE FIRST TIME THIS PROJECT WRITES INTO THE
        // GAME'S MEMORY. Everything before it observes. Leave it at -1 until the
        // read-only stage has shown the values look like what we expect. <<<
        static bool s_elemArmed = false;
        if (DiagMeleeElemProbe && !s_elemArmed) {
            const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
            if (gd) {
                const uintptr_t addr =
                    reinterpret_cast<uintptr_t>(gd) + humanai::kSlastHitBuilderRva;
                InterlockedExchange(&weaponanchor::g_damageWatchArmed, 1);
                dlvr::WatchExecSetStackFloatOffset(0);
                dlvr::WatchExecSetStackUseRsp(false);
                dlvr::WatchExecSetFieldCapture(0, 0);   // RCX here is the OUT struct
                // melee_force_part (per-model, by NAME) SUPERSEDES
                // melee_force_elem (a constant index). INV-72: a constant cannot
                // mean a body part, because the same index is different anatomy
                // on different models.
                const bool byPart = (MeleeForcePart > 0);
                const bool force  = byPart || (MeleeForceElem >= 0.0f);
                if (byPart) {
                    // key = the model at [R8+0x0], target = the element at
                    // [R8+0x8]. The table is filled by [ELEMDUMP] as models are
                    // discovered; until a model is in it, its hits MISS and
                    // write nothing.
                    dlvr::WatchExecPokeMapArm(humanai::kPokeRegR8, 0x0,
                                              humanai::kRecordElemOffset, true);
                    // D-44's fix: arm the TYPE fallback with the element
                    // count's location. Constants and their citation live on
                    // the declaration in boneblend.h so this site and the
                    // validator cannot drift apart.
                    dlvr::WatchExecPokeTypeArm(
                        static_cast<int>(kModelInnerOff),
                        static_cast<int>(kModelContainerOff),
                        static_cast<int>(kModelCountOff), true);
                } else {
                    dlvr::WatchExecPokeMapArm(humanai::kPokeRegR8, 0x0,
                                              humanai::kRecordElemOffset, false);
                    dlvr::WatchExecSetPoke(humanai::kPokeRegR8,
                                           humanai::kRecordElemOffset,
                                           static_cast<uint32_t>(MeleeForceElem), force);
                }
                // >>> LIFT THE 400-HIT CAP. THIS IS WHAT VOIDED THE 10:52 RUN. <<<
                // The watch self-disarms on the 401st hit and only WatchExecActive()
                // turns that into words - which this poll never calls. So the
                // override died within seconds of the first fight and the rest of
                // the run measured an unmodified game while looking identical.
                // INV-53 licenses the session-long hold; the [ELEM] stats line
                // below is what makes a silent death impossible to repeat.
                dlvr::WatchExecSetMaxHits(0);
                // >>> U-27'S ONE VARIABLE: WHICH MECHANISM CARRIES THE POKE. <<<
                // Both paths have already armed the SAME poke map and type map
                // above, and both run the SAME body (WatchExecPokeApplyAt), so
                // the only difference here is whether a debug register is used
                // at all. INV-120 is why that is the question.
                const bool inlineMode = (MeleeHookInline != 0);
                const bool armed = inlineMode ? MeleeElemInlineHookInstall()
                                              : dlvr::WatchExecStart(addr);
                if (inlineMode && !armed) {
                    // Named, not silent: a failed byte-compare means the game
                    // moved and we must NOT fall back to the breakpoint, because
                    // that would quietly turn a one-variable test into no test.
                    LOG("damagewatch: [ELEM] *** INLINE HOOK FAILED TO INSTALL *** - "
                        "melee_hook_inline=1 but the hook is not live, and this does "
                        "NOT fall back to the DR1 breakpoint on purpose: a silent "
                        "fallback would make the run look like a test of the inline "
                        "path when it was not. NOTHING IS ARMED. Treat the run as "
                        "void and read the [MELEEHOOK] line above for which check "
                        "refused.");
                }
                if (armed) {
                    s_elemArmed = true;
                    // Self-reporting on its own clock - see ElemStatsThread. The
                    // handle is checked, not discarded: a CreateThread whose
                    // result nobody looks at was a real defect in this file once,
                    // and a silent failure here would take the denominator with it.
                    {
                        HANDLE th = CreateThread(nullptr, 0, ElemStatsThread,
                                                 nullptr, 0, nullptr);
                        if (th) {
                            CloseHandle(th);
                        } else {
                            LOG("damagewatch: [ELEM] *** CreateThread FAILED for the "
                                "stats reporter (err %lu). THE PROBE IS ARMED BUT WILL "
                                "REPORT NOTHING - there will be no fires= denominator, "
                                "so anything observed this run cannot be attributed. "
                                "Treat the run as void.***", GetLastError());
                        }
                    }
                    LOG("damagewatch: [ELEM] ARMED on gamedll+0x%zX (the SLastHit "
                        "builder). R8 is the contact record; the element is at "
                        "[R8+0x%X]. Mode: %s. >>> THIS REPLACES [BONEWATCH] - one "
                        "global WatchExec slot - so there will be no *** BONE HIT *** "
                        "lines this run, by design. <<<%s",
                        humanai::kSlastHitBuilderRva, humanai::kRecordElemOffset,
                        force ? "OVERRIDE (writing into game memory)" : "OBSERVE ONLY",
                        force ? "  >>> THE WRITE IS LIVE: every melee hit's element "
                                "is being replaced. If the game misbehaves, set "
                                "melee_force_elem=-1 and relaunch. <<<" : "");
                } else {
                    LOG("damagewatch: [ELEM] WatchExecStart FAILED at gamedll+0x%zX - "
                        "the slot is taken or the address is bad. NOTHING is being "
                        "watched or written; a silent run means THIS.",
                        humanai::kSlastHitBuilderRva);
                }
            } else {
                LOG("damagewatch: [ELEM] gamedll not mapped - cannot arm.");
            }
        }

        static bool s_boneArmed = false;
        // DiagMeleeBoneCensus takes the same client 0 slot (option (a),
        // DESIGN_REGWRITE.md section 7), so it replaces this exactly as
        // diag_melee_elem_probe does. Suppressed here rather than letting
        // WatchExecStart fight over the slot: two clients racing for one
        // register is the contention INV-59 and D-45 exist to prevent.
        if (!DiagMeleeElemProbe && !DiagMeleeBoneCensus && !s_boneArmed) {
            const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
            if (gd) {
                const uintptr_t addr =
                    reinterpret_cast<uintptr_t>(gd) + humanai::kSetBoneRva;
                InterlockedExchange(&weaponanchor::g_damageWatchArmed, 1);
                dlvr::WatchExecSetStackFloatOffset(0);
                dlvr::WatchExecSetStackUseRsp(false);
                // >>> CAPTURE THE WHOLE m_LastHit* BLOCK AT THE INSTRUCTION. <<<
                // RCX is the HumanAIStateVector, and the block runs 0x680
                // (m_HitCount) .. 0x6CC (end of m_LastHitForcedCutTypes) = 76
                // bytes. 80 covers it. Read inside the handler, so every field
                // on a hit line belongs to THAT hit - see INV-5 and the
                // 2026-09-15 21:28 run where four bones shared one location
                // because these were read up to 100 ms late.
                dlvr::WatchExecSetFieldCapture(
                    static_cast<int32_t>(humanai::kHitCount), 0x50);
                if (dlvr::WatchExecStart(addr)) {
                    s_boneArmed = true;
                    LOG("damagewatch: [BONEWATCH] ARMED on gamedll+0x%zX "
                        "(SetLastHitBone: *(BYTE*)(a1+1687) = a2). RCX will be the "
                        "HumanAIStateVector, RDX the bone index. >>> IF THE FRAME "
                        "RATE DEGRADES FROM HERE, THIS IS THE CAUSE - quit and say "
                        "so; a watch held for a whole session is untested (the "
                        "longest prior hold was 750 ms). <<<", humanai::kSetBoneRva);
                } else {
                    LOG("damagewatch: [BONEWATCH] WatchExecStart FAILED at "
                        "gamedll+0x%zX - the slot is taken or the address is bad. "
                        "NOTHING is being watched; a silent run below means THIS, "
                        "not an absence of hits.", humanai::kSetBoneRva);
                }
            } else {
                LOG("damagewatch: [BONEWATCH] gamedll not mapped - cannot arm.");
            }
        }

        // ================= [HITCOUNT] - THE R5 POLL =================
        //
        // Runs on ENTITIES, not modules. Every entity is class-checked by RTTI
        // BEFORE a single offset is read, because whether +0x680 applies to the
        // pointer we hold is the one thing INV-50 leaves [UNKNOWN]: HumanAI's
        // COL reports offset-in-class 0xf58, so a subobject may be adjusted from
        // the complete object. The class name is logged for EVERY entity,
        // matched or not - if none is a HumanAI, that is the finding, and it is
        // stated rather than left as an empty result.
        // `vptr` is U-21's fix: the vtable pointer this base had when its class
        // was RTTI-CONFIRMED. Re-checking it at READ time is a single pointer
        // compare - no RTTI walk, no cost - and a recycled heap block will
        // almost never carry the same vptr. See the guard below.
        struct HaiTarget { uintptr_t ent; uintptr_t base; uint32_t off;
                   uint32_t hits; bool ok; char cls[96]; uintptr_t vptr; };
        static HaiTarget s_hai[kMaxScanTargets] = {};
        static int       s_haiCount = 0;
        static bool      s_haiReported = false;

        s_haiCount = 0;
        int haiRejected = 0;
        // Declared out here, not inside the block below, so [BONEWATCH]'s
        // reverse-search can still see the candidate entities from inside the
        // detect loop. Rebuilt every window before use.
        uintptr_t ents[kMaxScanTargets] = {};
        int nEnt = 0;
        {
            if (playerEnt) { ents[nEnt++] = playerEnt; }
            for (LONG i = 0; i < candN && nEnt < kMaxScanTargets; ++i) {
                const uintptr_t e = weaponanchor::g_healthCands[i].entity;
                if (!e) { continue; }
                bool dup = false;
                for (int k = 0; k < nEnt; ++k) { if (ents[k] == e) { dup = true; break; } }
                if (!dup) { ents[nEnt++] = e; }
            }
            // ============ THE CORRECTED CHAIN + THE FOUR BUNDLED CHECKS ============
            //
            // INV-56. Every hop below was read BY NAME out of a CRTTI registrar in
            // our own binary, not inferred. The four checks are Jay's: each states
            // its PREDICTION and then prints what it actually got, so a miss is
            // loud rather than absent. NOTHING here is on a per-frame path - this
            // runs in damagewatch's own worker, once per window, and every RTTI
            // walk is bounded by nEnt (<= kMaxScanTargets).
            //
            // CHECK 1  RttiClassNameAndOffsetOf(entity)   expect HumanAI / 0x28
            // CHECK 2  entity vptr                        expect gamedll+0x151E600
            // CHECK 3  *(entity - 0x28 + 0x970)           expect HumanAIStateVector
            // CHECK 4  modules[] -> m_AI(+0x40) -> +0x970 expect the SAME pointer
            //
            // Check 4 is the one that cannot lie: it reaches the same object by a
            // completely different route, so agreement is real corroboration and
            // disagreement names which hop is wrong.
            const uintptr_t gdBase =
                reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
            const uintptr_t predVt = gdBase ? (gdBase + humanai::kHumanAiIgsVtableRva) : 0;
            if (!s_haiReported) {
                LOG("damagewatch: [CHAIN2] chain under test: entity %+d -> complete AI, "
                    "then AI+0x%03zX (the AIStateVector* the ENGINE itself reads in "
                    "sub_1802FCE20) or AI+0x%03zX (m_StateVector field start, CRTTI "
                    "0x970 + CRTTIObject mdisp 0x28). Composed: entity %+d. If both "
                    "miss, a bounded sweep of AI+0x900..0xA80 RTTI-checks every slot "
                    "and NAMES the real offset. >>> 20:34 read AI+0x970 and got two "
                    "floats (52.760, -651.907) - world coords - because CRTTI offsets "
                    "are measured from the CRTTIObject subobject, which for AI/HumanAI "
                    "is at mdisp 0x28 and for AIModule/HumanAIStateVector is at 0. <<< "
                    "gamedll base %p, predicted entity vptr %p.",
                    (int)humanai::kEntityToHumanAI, humanai::kAiToStateVecPtr,
                    humanai::kAiToStateVecFld, (int)humanai::kEntityToStateVec,
                    reinterpret_cast<void*>(gdBase), reinterpret_cast<void*>(predVt));
            }

            int chkClassOk = 0, chkVtOk = 0, chkSvOk = 0, chkModAgree = 0, chkModFound = 0;
            int chkSweep = 0;

            for (int ei = 0; ei < nEnt && s_haiCount < kMaxScanTargets; ++ei) {
                const uintptr_t ent = ents[ei];

                // ---- CHECK 1: what IS the captured entity? Never logged before. ----
                char entCls[96];
                uint32_t entColOff = 0xFFFFFFFFu;
                const bool entNamed =
                    RttiClassNameAndOffsetOf(ent, entCls, sizeof(entCls), entColOff);
                if (!entNamed) {
                    strncpy_s(entCls, sizeof(entCls), "<no rtti>", _TRUNCATE);
                }
                const bool c1 = entNamed && strstr(entCls, "HumanAI") != nullptr &&
                                entColOff == 0x28;
                if (c1) { ++chkClassOk; }

                // ---- CHECK 2: the entity's vptr against the predicted COL vtable ----
                uintptr_t entVt = 0;
                const bool vtOk = weaponanchor::ReadPtr(ent, entVt);
                const bool c2 = vtOk && predVt && entVt == predVt;
                if (c2) { ++chkVtOk; }

                // ---- CHECK 3: MEASURE the state vector, do not predict it ----
                //
                // The 20:34 run predicted ONE offset and got floats, and a single
                // predicted offset cannot tell "wrong number" from "wrong idea".
                // So this tries the two candidates the binary actually justifies
                // and, failing both, SWEEPS a bounded window RTTI-checking every
                // 8-byte slot. Whatever the sweep prints is the real offset,
                // derived from the game rather than from me.
                const uintptr_t aiObj =
                    static_cast<uintptr_t>(static_cast<ptrdiff_t>(ent) +
                                           humanai::kEntityToHumanAI);
                struct Cand { size_t off; const char* why; };
                static const Cand kCands[] = {
                    { humanai::kAiToStateVecPtr, "AI+0x9A0 SmartPtr payload (what the engine reads)" },
                    { humanai::kAiToStateVecFld, "AI+0x998 m_StateVector field start (CRTTI 0x970)" },
                };
                uintptr_t sv = 0;
                size_t    svFrom = 0;
                const char* svWhy = "";
                char cls[96];
                uint32_t colOff = 0;
                bool named = false;
                for (int ci = 0; ci < 2 && !named; ++ci) {
                    uintptr_t cand = 0;
                    if (!weaponanchor::ReadPtr(aiObj + kCands[ci].off, cand) || !cand) {
                        continue;
                    }
                    char c[96];
                    uint32_t co = 0;
                    if (RttiClassNameAndOffsetOf(cand, c, sizeof(c), co) &&
                        strstr(c, "HumanAIStateVector")) {
                        sv = cand; svFrom = kCands[ci].off; svWhy = kCands[ci].why;
                        colOff = co; named = true;
                        strncpy_s(cls, sizeof(cls), c, _TRUNCATE);
                    }
                }
                // The sweep. Bounded, and it runs ONLY when both candidates miss,
                // so it costs nothing on the expected path.
                size_t sweepFound = 0;
                if (!named) {
                    for (size_t off = 0x900; off + sizeof(uintptr_t) <= 0xA80; off += 8) {
                        uintptr_t cand = 0;
                        if (!weaponanchor::ReadPtr(aiObj + off, cand) || !cand) { continue; }
                        char c[96];
                        uint32_t co = 0;
                        if (RttiClassNameAndOffsetOf(cand, c, sizeof(c), co) &&
                            strstr(c, "HumanAIStateVector")) {
                            sv = cand; svFrom = off; sweepFound = off; colOff = co;
                            named = true;
                            svWhy = "FOUND BY SWEEP - neither predicted offset was right";
                            strncpy_s(cls, sizeof(cls), c, _TRUNCATE);
                            break;
                        }
                    }
                }
                if (!named) {
                    strncpy_s(cls, sizeof(cls), "<none in AI+0x900..0xA80>", _TRUNCATE);
                }
                const bool c3 = named;
                if (c3) { ++chkSvOk; }
                if (sweepFound) { ++chkSweep; }

                // ---- CHECK 4: the same object via modules[] -> AIModule::m_AI ----
                uintptr_t svViaMod = 0, usedMod = 0;
                unsigned  usedSlot = 0;
                char      modCls[96];
                modCls[0] = '\0';
                for (int s = 0; s < humanai::kLogicModuleSlotCount && !svViaMod; ++s) {
                    uintptr_t mod = 0;
                    if (!weaponanchor::ReadPtr(
                            ent + weaponanchor::kModulesArrayOffset +
                                humanai::kLogicModuleSlots[s] * sizeof(uintptr_t), mod) ||
                        !mod) {
                        continue;
                    }
                    uintptr_t ai2 = 0;
                    if (!weaponanchor::ReadPtr(mod + humanai::kAiModuleToAi, ai2) || !ai2) {
                        continue;
                    }
                    // Use whatever offset C3 actually PROVED on this entity, so
                    // the two routes are compared at the same field rather than
                    // at two different guesses. Falls back to the engine's own
                    // read when C3 found nothing.
                    const size_t modOff = svFrom ? svFrom : humanai::kAiToStateVecPtr;
                    uintptr_t sv2 = 0;
                    if (!weaponanchor::ReadPtr(ai2 + modOff, sv2) || !sv2) {
                        continue;
                    }
                    svViaMod = sv2;
                    usedMod  = mod;
                    usedSlot = humanai::kLogicModuleSlots[s];
                    if (!RttiClassNameOf(mod, modCls, sizeof(modCls))) {
                        strncpy_s(modCls, sizeof(modCls), "<no rtti>", _TRUNCATE);
                    }
                }
                if (svViaMod) { ++chkModFound; }
                const bool c4 = svViaMod && sv && svViaMod == sv;
                if (c4) { ++chkModAgree; }

                // ---- ONE LINE PER ENTITY, PASS OR FAIL. No silent path. ----
                if (!s_haiReported) {
                    LOG("damagewatch: [CHAIN2] entity %p  C1 class='%s' COL=0x%X %s | "
                        "C2 vptr=%p %s | C3 sv=%p '%s' @AI+0x%03zX (entity%+d) [%s] %s "
                        "| C4 slot=%u mod=%p '%s' sv=%p %s",
                        reinterpret_cast<void*>(ent), entCls, entColOff,
                        c1 ? "PASS" : ">>>MISS (expected HumanAI / 0x28)",
                        reinterpret_cast<void*>(entVt),
                        c2 ? "PASS" : ">>>MISS (expected the predicted COL vtable)",
                        reinterpret_cast<void*>(sv), cls, svFrom,
                        (int)(humanai::kEntityToHumanAI + (ptrdiff_t)svFrom),
                        svWhy[0] ? svWhy : "-",
                        c3 ? (sweepFound ? ">>>PASS BUT ONLY VIA THE SWEEP" : "PASS")
                           : ">>>MISS (no HumanAIStateVector anywhere in AI+0x900..0xA80)",
                        usedSlot, reinterpret_cast<void*>(usedMod),
                        modCls[0] ? modCls : "-",
                        reinterpret_cast<void*>(svViaMod),
                        c4 ? "AGREES with C3"
                           : (svViaMod ? ">>>DISAGREES with C3 - one hop is wrong"
                                       : ">>>no logic module resolved on this entity"));
                }

                // THE GATE is unchanged: nothing is read off an unverified base.
                if (!c3) {
                    ++haiRejected;
                    continue;
                }

                HaiTarget& t = s_hai[s_haiCount];
                t.ent  = ents[ei];
                t.off  = colOff;
                t.base = sv;
                t.ok   = ReadFieldU32(t.base + humanai::kHitCount, t.hits);
                strncpy_s(t.cls, sizeof(t.cls), cls, _TRUNCATE);
                // U-21: latch the vptr NOW, while the class is RTTI-confirmed.
                t.vptr = 0;
                weaponanchor::ReadPtr(t.base, t.vptr);

                if (!s_haiReported) {
                    uint8_t bone = 0, part = 0;
                    uint32_t cuts = 0;
                    ReadFieldBytes(t.base + humanai::kLastHitBone, &bone, 1);
                    ReadFieldBytes(t.base + humanai::kLastHitPart, &part, 1);
                    // R5's gore field has NEVER been read live - print it the first
                    // time a base is confirmed, so it stops being registrar-only.
                    ReadFieldU32(t.base + humanai::kLastHitForcedCutTypes, cuts);
                    LOG("damagewatch: [HITCOUNT] entity %p -> AI %p -> stateVec %p "
                        "'%s' (COL 0x%X)  m_HitCount=%u bone=%u part=%u "
                        "m_LastHitForcedCutTypes=0x%08X   <== RTTI CONFIRMED, polling",
                        reinterpret_cast<void*>(ents[ei]),
                        reinterpret_cast<void*>(aiObj),
                        reinterpret_cast<void*>(sv), cls, colOff, t.hits, bone, part,
                        cuts);
                }
                if (t.ok) { ++s_haiCount; }
            }

            if (!s_haiReported) {
                LOG("damagewatch: [CHAIN2] TOTALS over %d entit(y/ies): C1 class+COL %d "
                    "(was 18/18 at 20:34), C2 vptr %d (was 18/18), C3 state vector %d "
                    "of which %d needed the SWEEP, C4 module route resolved %d and "
                    "AGREED %d. >>> C3 IS THE RESULT. C3 == %d means the chain is "
                    "confirmed live and U-20 closes. C3 nonzero WITH sweeps means the "
                    "offset is right but not the one predicted - the per-entity lines "
                    "name the real one, hardcode THAT. C3 == 0 means no "
                    "HumanAIStateVector exists anywhere in AI+0x900..0xA80 and the "
                    "CRTTIObject-relative reading is wrong, not merely mis-numbered. "
                    "C1/C2 regressing from 18/18 would mean the capture changed, not "
                    "the chain. <<<",
                    nEnt, chkClassOk, chkVtOk, chkSvOk, chkSweep, chkModFound,
                    chkModAgree, nEnt);
            }

            if (!s_haiReported) {
                s_haiReported = true;
                LOG("damagewatch: [HITCOUNT] %d RTTI-CONFIRMED HumanAIStateVector(s) will be polled at "
                    "+0x%03zX (m_HitCount, from sub_18060E9E0), %d "
                    "refused. >>> IF THIS IS 0, NOTHING IS BEING WATCHED AND ANY 'no hits' "
                    "BELOW IS ABOUT THE POINTER CHAIN, NOT ABOUT THE GAME. <<<",
                    s_haiCount, humanai::kHitCount, haiRejected);
            }
        }

        LOG("damagewatch: WINDOW %lu - KNOWN-OFFSET poll over %d module(s) (%d "
            "readable) x %d health fields. Offsets read from "
            "BodyPartsHealthModule::RegisterRTTI (gamedll+0x2848F0), NOT scanned. "
            "*** FIGHT NOW *** hit a zombie - any DECREASE in a limb field is a hit. "
            "%d s. Attack trigger: %s. %d non-health module(s) REJECTED BY CLASS "
            "(the 16:09 run polled those and produced 38 false positives).",
            windowNo, g_scanTargetCount, okCount, kFieldCount, kDetectSeconds,
            triggerOn ? "ARMED" : "NOT ARMED - correlation column "
                                  "will read n/a, see reason above",
            rejectedByClass);

        if (g_scanTargetCount == 0) {
            LOG("damagewatch: WINDOW %lu - NO health-class module survived the "
                "filter (%d rejected). This is NOT 'nothing took damage' - there "
                "is nothing being watched at all. Either weaponanchor captured no "
                "damageable body, or RTTI names none of them as HealthModule / "
                "BodyPartsHealthModule / ArmorHealthModule.",
                windowNo, rejectedByClass);
            Sleep(2000);
            continue;
        }

        int      hitTarget = -1, hitDword = -1;
        unsigned hitFrom = 0, hitTo = 0;
        int      reported = 0;
        int      hitScore = 0;
        for (int tick = 0; tick < kDetectSeconds * 10; ++tick) {
            Sleep(100);

            // ---- [ELEM] ----
            // BOTH the STATS and the *** MELEE ELEM *** capture logging have MOVED
            // to ElemStatsThread. They used to sit here, in a loop that belongs to
            // damagewatch's health-DETECT phase rather than to the probe.
            //
            // 2026-09-16 11:44:24: damagewatch moved to its write-watch window, this
            // loop stopped ticking, and the override went unreported for 3m 45s. The
            // run was void. The STATS were moved then and the CAPTURES were NOT -
            // so the 12:23 run lost every capture in its second phase (fires climbed
            // 12 -> 24 with zero *** MELEE ELEM *** lines) while the fixed half
            // reported perfectly. HALF A FIX IS ITS OWN FAILURE MODE.
            //
            // Do not put either of them back here.

            // ---- [BONEWATCH] first: this is the authoritative signal ----
            if (s_boneArmed) {
                // 32 -> 64. g_execHits is capped at 64 (watchwrite.cpp:338), so a
                // 32-entry buffer SILENTLY returned only the first half of the
                // table once more than 32 distinct (body, bone, site) triples had
                // been seen - entries 33..64 were never handed back at all and
                // nothing said so. Matching the buffer to the table removes that.
                dlvr::ExecHit hits[64] = {};
                const int n = dlvr::WatchExecGet(hits, 64);
                // And the table itself stops ACCEPTING new entries at 64, also
                // silently. Say it once, so "no hits from that zombie" can never
                // be read as "that zombie was never hit".
                {
                    const int tableN = dlvr::WatchExecCount();
                    static volatile LONG saidFull = 0;
                    if (tableN >= 64 &&
                        InterlockedCompareExchange(&saidFull, 1, 0) == 0) {
                        LOG("damagewatch: [BONEWATCH] the exec-hit table is FULL at %d "
                            "entries (watchwrite.cpp:338 refuses beyond 64). Existing "
                            "entries keep counting, but any NEW (body, bone, call-site) "
                            "combination from here on is DROPPED AND NOT RECORDED. "
                            "A body that stops appearing after this line may simply "
                            "never have been admitted.", tableN);
                    }
                }
                // ====== [LOCREAD] - who READS m_LastHitLocation? ======
                // Rides this table because it is the only place a real
                // HumanAIStateVector pointer exists (INV-103: RCX at the melee
                // trap). Read-only, arms once, disarms itself.
                if (DiagLastHitLocReadWatch) {
                    locreadTick(hits, n);
                }

                for (int h = 0; h < n; ++h) {
                    const uintptr_t sv = hits[h].rcxValue;
                    const uintptr_t bone = hits[h].rdxValue & 0xFF;
                    if (!sv) { continue; }

                    // ============ THE COUNT-DELTA GATE ============
                    //
                    // >>> WatchExecGet() COPIES, IT DOES NOT DRAIN
                    // (watchwrite.cpp:705). g_execHits is an AGGREGATING table
                    // keyed on (rcx, rdx, returnAddr), capped at 64, with a per
                    // -entry `count`. So every 100 ms poll hands back the SAME
                    // entries forever. Logging one line per returned entry per
                    // poll produced 25,031 lines from 13 real bodies on
                    // 2026-09-15 21:15 - and worse, it kept re-reading state
                    // vectors whose bodies had died, printing freed memory
                    // (LOCATION.x == physDmg == 7644.2, y = z = 0). <<<
                    //
                    // The old s_seen dedup had been hiding all of that. It was
                    // removed without reading what it was compensating for.
                    //
                    // `count` is the watch's own per-entry call counter, so the
                    // honest signal is ITS DELTA. Static check, 2026-09-15:
                    // SetLastHitBone (gamedll+0x2CF4E0) has EXACTLY ONE call site,
                    // sub_1802ABE70 at 0x2AD9A0 - the hit-registration function -
                    // so one increment IS one registered hit, not an approximation.
                    //
                    // A dead body's entry stops incrementing, so stale entries go
                    // silent on their own. That is the stale-read fix as well.
                    //
                    // Keyed by INDEX because g_execHits only ever grows (push_back,
                    // capped, never erased except on a full clear) - and the
                    // identity check below catches the clear-and-refill case
                    // rather than trusting that.
                    static uintptr_t s_slotSv[64]   = {};
                    static uintptr_t s_slotBone[64] = {};
                    static uint32_t  s_slotCount[64] = {};
                    uint32_t newCalls = 1;
                    if (h < 64) {
                        if (s_slotSv[h] != sv || s_slotBone[h] != bone) {
                            s_slotSv[h]    = sv;
                            s_slotBone[h]  = bone;
                            s_slotCount[h] = 0;
                        }
                        if (hits[h].count <= s_slotCount[h]) {
                            continue;              // nothing new in this slot
                        }
                        newCalls = hits[h].count - s_slotCount[h];
                        s_slotCount[h] = hits[h].count;
                    }
                    // ================= THE BODY CACHE =================
                    //
                    // >>> WHAT THIS REPLACED, AND WHY IT MATTERED FOR R5: a
                    // `static uintptr_t s_seen[64]` that was never cleared and
                    // `if (dup) continue;`. Each state vector was therefore
                    // reported ONCE EVER, so the 2nd and later hits on the same
                    // zombie were dropped on the floor. The 11-16 "hits" in every
                    // run to date were 11-16 BODIES. For gore that is useless -
                    // dismemberment is a property of a PARTICULAR BLOW. <<<
                    //
                    // The dedup is not deleted, it is DEMOTED: it now gates only
                    // the EXPENSIVE, PER-BODY work (the <=291-read owner sweep and
                    // two RTTI walks), which genuinely does not change between hits
                    // on the same body. EVERY HIT IS REPORTED.
                    //
                    // The array also wraps now instead of silently refusing to
                    // record body 65 onwards - which would have quietly turned the
                    // cache off mid-session and made every later hit pay full price.
                    struct Body {
                        uintptr_t sv, ai, ent;
                        // TWO separate COLs. Sharing one field would silently lose
                        // the state vector's (which INV-57 predicts is 0x0) behind
                        // the entity's (0x28) - two different facts, two slots.
                        uint32_t  svCol, entCol;
                        char      cls[96], entCls[96];
                        size_t    backOff;
                        uint32_t  nHits;
                        int       id;
                        // ---- R5's gore half. Dismemberment is PERSISTENT body
                        // state, not a per-hit transient: once an arm is off it
                        // stays off. So the useful signal is the DELTA between
                        // blows, which is what this snapshot exists for. The
                        // module lives on the ENTITY, not the state vector, so
                        // it cannot ride in the synchronous blob - and it does
                        // not need to.
                        uintptr_t bpMod;        // the BodyParts/Armor health module
                        uintptr_t bpVptr;       // U-21: latched, re-checked per read
                        char      bpCls[96];
                        uint32_t  goreCut, goreOff, goreHoles, goreDestroyed,
                                  goreTypes;
                        uint8_t   goreHead, goreHeadHalf;
                        bool      goreValid;
                    };
                    static Body s_body[64] = {};
                    static int  s_bodyN = 0, s_bodyNext = 0, s_bodyIds = 0;

                    Body* b = nullptr;
                    for (int k = 0; k < s_bodyN; ++k) {
                        if (s_body[k].sv == sv) { b = &s_body[k]; break; }
                    }
                    const bool firstSight = (b == nullptr);
                    if (firstSight) {
                        b = &s_body[s_bodyNext];
                        s_bodyNext = (s_bodyNext + 1) % 64;
                        if (s_bodyN < 64) { ++s_bodyN; }
                        memset(b, 0, sizeof(*b));
                        b->sv = sv;
                        b->id = ++s_bodyIds;
                        b->entCol = 0xFFFFFFFFu;
                        if (!RttiClassNameAndOffsetOf(sv, b->cls, sizeof(b->cls),
                                                      b->svCol)) {
                            strncpy_s(b->cls, sizeof(b->cls), "<no rtti>", _TRUNCATE);
                        }
                        // ---- the owner sweep, INV-58. Once per body. ----
                        for (size_t off = 0;
                             off + sizeof(uintptr_t) <= humanai::kStateVectorSize;
                             off += sizeof(uintptr_t)) {
                            uintptr_t cand = 0;
                            if (!weaponanchor::ReadPtr(sv + off, cand) || !cand) { continue; }
                            uintptr_t back = 0;
                            if (!weaponanchor::ReadPtr(cand + humanai::kAiToStateVecPtr,
                                                       back) || back != sv) {
                                continue;
                            }
                            b->ai      = cand;
                            b->backOff = off;
                            b->ent     = static_cast<uintptr_t>(
                                static_cast<ptrdiff_t>(cand) - humanai::kEntityToHumanAI);
                            if (!RttiClassNameAndOffsetOf(b->ent, b->entCls,
                                                          sizeof(b->entCls), b->entCol)) {
                                strncpy_s(b->entCls, sizeof(b->entCls), "<no rtti>",
                                          _TRUNCATE);
                            }
                            break;
                        }
                    }
                    // ---- resolve this body's health module, once ----
                    // Slot 23 first (BODY_PARTS_HEALTH_MODULE), then 22. The
                    // class is RTTI-CHECKED before any offset is read: a slot
                    // index is not a class, and if the EE layout differs that
                    // slot holds something else entirely.
                    if (b->ent) {
                        const unsigned slots[2] = { humanai::kBodyPartsHealthSlot,
                                                    humanai::kHealthSlot };
                        for (int si = 0; si < 2 && !b->bpMod; ++si) {
                            uintptr_t m = 0;
                            if (!weaponanchor::ReadPtr(
                                    b->ent + weaponanchor::kModulesArrayOffset +
                                        slots[si] * sizeof(uintptr_t), m) || !m) {
                                continue;
                            }
                            char mc[96];
                            if (!RttiClassNameOf(m, mc, sizeof(mc))) { continue; }
                            // Only classes that actually CONTAIN the gore fields.
                            // ArmorHealthModule derives from BodyPartsHealthModule
                            // at mdisp 0, so it qualifies; a plain HealthModule is
                            // 200 bytes and does NOT - the fields would be past
                            // its end.
                            if (!strstr(mc, "BodyPartsHealthModule") &&
                                !strstr(mc, "ArmorHealthModule")) {
                                continue;
                            }
                            b->bpMod = m;
                            strncpy_s(b->bpCls, sizeof(b->bpCls), mc, _TRUNCATE);
                            weaponanchor::ReadPtr(m, b->bpVptr);
                            // >>> PUBLISH IT TO THE HEALTH POLL. See the comment
                            // on PublishHitBody. This body was just HIT, so it
                            // is exactly what the poll has been failing to
                            // watch, and it arrives without touching the
                            // WatchExec slot [BONEWATCH] holds. <<<
                            {
                                static bool s_capSaid = false;
                                const bool added = weaponanchor::PublishHitBody(
                                    b->ent, m, slots[si], mc);
                                if (added) {
                                    LOG("damagewatch: [HITBODY] published body#%d's "
                                        "%s %p (entity %p) to the health poll - it "
                                        "took a blow, so it is a target the poll can "
                                        "trust. Candidates now %ld.",
                                        b->id, mc, reinterpret_cast<void*>(m),
                                        reinterpret_cast<void*>(b->ent),
                                        weaponanchor::g_healthCandCount);
                                } else if (weaponanchor::g_healthCandCount >=
                                               weaponanchor::kMaxHealthCandidates &&
                                           !s_capSaid) {
                                    s_capSaid = true;
                                    LOG("damagewatch: [HITBODY] *** THE CANDIDATE "
                                        "TABLE IS FULL at %d *** - bodies hit from "
                                        "here on are NOT being added to the poll, so "
                                        "a later window's silence is about the CAP, "
                                        "not about the fields. Raise "
                                        "kMaxHealthCandidates.",
                                        weaponanchor::kMaxHealthCandidates);
                                }
                            }
                        }
                    }
                    b->nHits += newCalls;

                    // ===== DECODE THE SYNCHRONOUS BLOB, NOT A LATE RE-READ =====
                    //
                    // The blob was captured inside the breakpoint handler at the
                    // instant of THIS hit, at [RCX + 0x680], so every field below
                    // belongs to this blow. The live-read fallback stays for the
                    // case where the capture faulted - and it SAYS which one it
                    // used, because "read late" and "read at the hit" are
                    // different quantities (INV-5) and must never be confusable.
                    uint32_t hc = 0, cuts = 0;
                    uint8_t part = 0, dir = 0;
                    float loc[3] = {}, vec[3] = {}, phys = 0.0f, dmgSum = 0.0f;
                    const bool sync = hits[h].fieldBlobValid &&
                                      hits[h].fieldBlobBytes >= 0x50;
                    if (sync) {
                        const uint8_t* fb = hits[h].fieldBlob;   // fb[0] == sv+0x680
                        memcpy(&hc,     fb + (humanai::kHitCount            - 0x680), 4);
                        memcpy(&dmgSum, fb + (humanai::kLastDamageSum       - 0x680), 4);
                        memcpy(loc,     fb + (humanai::kLastHitLocation     - 0x680), 12);
                        memcpy(&dir,    fb + (humanai::kLastHitDir          - 0x680), 1);
                        memcpy(&part,   fb + (humanai::kLastHitPart         - 0x680), 1);
                        memcpy(vec,     fb + (humanai::kLastHitVector       - 0x680), 12);
                        memcpy(&phys,   fb + (humanai::kLastHitPhysicDamage - 0x680), 4);
                        memcpy(&cuts,   fb + (humanai::kLastHitForcedCutTypes - 0x680), 4);
                    } else {
                        ReadFieldU32  (sv + humanai::kHitCount, hc);
                        ReadFieldBytes(sv + humanai::kLastHitPart, &part, 1);
                        ReadFieldBytes(sv + humanai::kLastHitDir,  &dir,  1);
                        ReadFieldBytes(sv + humanai::kLastHitLocation, loc, sizeof(loc));
                        ReadFieldBytes(sv + humanai::kLastHitVector,   vec, sizeof(vec));
                        ReadFieldFloat(sv + humanai::kLastHitPhysicDamage, phys);
                        ReadFieldFloat(sv + humanai::kLastDamageSum,       dmgSum);
                        ReadFieldU32  (sv + humanai::kLastHitForcedCutTypes, cuts);
                    }

                    LOG("damagewatch: *** BONE HIT *** body#%d hit#%u (+%u new, watch "
                        "count=%u)  BONE=%llu part=%u "
                        "dir=%u  CUTS=0x%08X  m_HitCount=%u  LOCATION=(%.3f, %.3f, %.3f) "
                        " VECTOR=(%.3f, %.3f, %.3f)  physDmg=%.3f dmgSum=%.3f  "
                        "entity=%p '%s'  stateVec=%p '%s'%s",
                        b->id, b->nHits, newCalls,
                        (h < 64) ? s_slotCount[h] : 0u,
                        static_cast<unsigned long long>(bone), part, dir,
                        cuts, hc,
                        static_cast<double>(loc[0]), static_cast<double>(loc[1]),
                        static_cast<double>(loc[2]),
                        static_cast<double>(vec[0]), static_cast<double>(vec[1]),
                        static_cast<double>(vec[2]),
                        static_cast<double>(phys), static_cast<double>(dmgSum),
                        reinterpret_cast<void*>(b->ent),
                        b->entCls[0] ? b->entCls : "<unattributed>",
                        reinterpret_cast<void*>(sv), b->cls,
                        sync ? (firstSight ? "  [SYNC]  <== FIRST HIT ON THIS BODY"
                                           : "  [SYNC]")
                             : (firstSight
                                    ? "  >>>LATE READ (capture faulted)  <== FIRST HIT"
                                    : "  >>>LATE READ (capture faulted)"));

                    // ================= R5's GORE DELTA, PER BLOW =================
                    //
                    // Read the body's dismemberment state and report WHAT CHANGED
                    // since the previous blow on this same body. A running total
                    // answers "is this zombie damaged"; the DELTA answers "what
                    // did THIS blow take off", which is what R5 asks for.
                    //
                    // Read at poll time deliberately: unlike m_LastHit*, this is
                    // persistent state - an arm that came off stays off - so the
                    // ~100 ms latency that ruined the CUTS reading is harmless
                    // here. What would NOT be harmless is a stale module pointer,
                    // so the vptr latched at capture is re-checked first (U-21).
                    if (b->bpMod) {
                        uintptr_t liveVptr = 0;
                        const bool alive =
                            weaponanchor::ReadPtr(b->bpMod, liveVptr) &&
                            liveVptr == b->bpVptr;
                        if (!alive) {
                            static volatile LONG saidGoreStale = 0;
                            if (InterlockedCompareExchange(&saidGoreStale, 1, 0) == 0) {
                                LOG("damagewatch:   [GORE] body#%d module %p vptr changed "
                                    "%p -> %p: freed and reused. NOT reading gore fields "
                                    "off it. Reported once (U-21).",
                                    b->id, reinterpret_cast<void*>(b->bpMod),
                                    reinterpret_cast<void*>(b->bpVptr),
                                    reinterpret_cast<void*>(liveVptr));
                            }
                            b->bpMod = 0;
                        } else {
                            uint32_t gCut = 0, gOff = 0, gHoles = 0, gDest = 0, gTypes = 0;
                            uint8_t  gHead = 0, gHeadHalf = 0;
                            const bool ok =
                                ReadFieldU32(b->bpMod + humanai::kBpBodyCutType,    gCut) &&
                                ReadFieldU32(b->bpMod + humanai::kBpCutOffBones,    gOff) &&
                                ReadFieldU32(b->bpMod + humanai::kBpBodyHoles,      gHoles) &&
                                ReadFieldU32(b->bpMod + humanai::kBpDestroyedBones, gDest) &&
                                ReadFieldU32(b->bpMod + humanai::kBpCutOffTypes,    gTypes) &&
                                ReadFieldBytes(b->bpMod + humanai::kBpHeadSmashed,
                                               &gHead, 1) &&
                                ReadFieldBytes(b->bpMod + humanai::kBpHeadSmashedHalf,
                                               &gHeadHalf, 1);
                            if (!ok) {
                                LOG("damagewatch:   [GORE] body#%d - a gore field read "
                                    "FAILED on module %p '%s'. No values reported; this "
                                    "is not 'nothing changed'.",
                                    b->id, reinterpret_cast<void*>(b->bpMod), b->bpCls);
                            } else if (!b->goreValid) {
                                b->goreCut = gCut; b->goreOff = gOff;
                                b->goreHoles = gHoles; b->goreDestroyed = gDest;
                                b->goreTypes = gTypes; b->goreHead = gHead;
                                b->goreHeadHalf = gHeadHalf; b->goreValid = true;
                                LOG("damagewatch:   [GORE] body#%d BASELINE on %p '%s': "
                                    "m_BodyCutType=0x%08X m_CutOffBones=0x%08X "
                                    "m_BodyHoles=0x%08X m_DestroyedBones=0x%08X "
                                    "m_CutOffTypes=0x%08X m_HeadSmashed=%u/%u. Deltas "
                                    "from here are what each blow did.",
                                    b->id, reinterpret_cast<void*>(b->bpMod), b->bpCls,
                                    gCut, gOff, gHoles, gDest, gTypes, gHead, gHeadHalf);
                            } else if (gCut != b->goreCut || gOff != b->goreOff ||
                                       gHoles != b->goreHoles ||
                                       gDest != b->goreDestroyed ||
                                       gTypes != b->goreTypes || gHead != b->goreHead ||
                                       gHeadHalf != b->goreHeadHalf) {
                                LOG("damagewatch:   *** GORE CHANGED *** body#%d hit#%u "
                                    "BONE=%llu :  m_BodyCutType 0x%08X->0x%08X  "
                                    "m_CutOffBones 0x%08X->0x%08X  m_BodyHoles "
                                    "0x%08X->0x%08X  m_DestroyedBones 0x%08X->0x%08X  "
                                    "m_CutOffTypes 0x%08X->0x%08X  m_HeadSmashed "
                                    "%u->%u / %u->%u   <<< THIS BLOW DID SOMETHING",
                                    b->id, b->nHits,
                                    static_cast<unsigned long long>(bone),
                                    b->goreCut, gCut, b->goreOff, gOff,
                                    b->goreHoles, gHoles, b->goreDestroyed, gDest,
                                    b->goreTypes, gTypes, b->goreHead, gHead,
                                    b->goreHeadHalf, gHeadHalf);
                                b->goreCut = gCut; b->goreOff = gOff;
                                b->goreHoles = gHoles; b->goreDestroyed = gDest;
                                b->goreTypes = gTypes; b->goreHead = gHead;
                                b->goreHeadHalf = gHeadHalf;
                            }
                        }
                    } else if (firstSight) {
                        LOG("damagewatch:   [GORE] body#%d - NO BodyParts/Armor health "
                            "module on entity %p (slots %u and %u checked, class "
                            "RTTI-verified). Gore cannot be read for this body, and "
                            "that is a stated absence, not a silent zero.",
                            b->id, reinterpret_cast<void*>(b->ent),
                            humanai::kBodyPartsHealthSlot, humanai::kHealthSlot);
                    }

                    // Everything below is per-BODY detail and only prints on first
                    // sight. The hit line above prints for EVERY hit.
                    if (!firstSight) { continue; }

                    // >>> ATTRIBUTION. We hold a KNOWN-GOOD state-vector pointer
                    // straight from the setter, so the question is no longer
                    // "what is the offset" (INV-56 answered that statically) but
                    // "WHICH BODY was hit". Test the composed offset first; only
                    // if that misses, fall back to the brute scan that names the
                    // real offset. <<<
                    //
                    // 2026-09-15: THE OLD VERSION OF THIS LOOP LOGGED ONLY ON A
                    // MATCH. No else branch. So when it found nothing - which is
                    // what happened on every one of the 13 hits in the 20:00 run -
                    // it printed NOTHING AT ALL, and the silence had to be
                    // diagnosed from the surrounding log instead of being read off
                    // the line that should have stated it. RULE 0's general form,
                    // in our own probe. It now always says something.
                    // ======= D-39 FIX: ATTRIBUTE WITHOUT AN ENTITY LIST =======
                    //
                    // The candidate list froze the moment [BONEWATCH] armed, so
                    // searching it fails for any body that spawned afterwards -
                    // 0 of 16 on 2026-09-15 20:51. Rather than rewrite the single
                    // -slot arbitration, INVERT THE CHAIN, which needs no list at
                    // all and cannot contend with anything:
                    //
                    //     for off in 0 .. kStateVectorSize step 8:
                    //         cand = *(sv + off)
                    //         if *(cand + 0x9A0) == sv  ->  cand IS THE AI
                    //
                    // It is SELF-VALIDATING: the only pointer that reaches back to
                    // this exact state vector through AI::m_StateVector's payload
                    // is its own owner. Then entity = AI + 0x28 (INV-55), and the
                    // entity's RTTI is checked before it is believed.
                    //
                    // COST: <= 291 guarded qword reads, once per NEW state vector
                    // (s_seen dedups), on damagewatch's worker thread. Nothing
                    // per-frame, no breakpoint, no slot.
                    // The sweep already ran, once, when this body was first seen and
                    // cached above. Reuse it - repeating <=291 reads and two RTTI
                    // walks per hit would be exactly the per-frame-cost mistake
                    // RULE 0 names, on a path that now fires on EVERY blow.
                    const uintptr_t ownerAi  = b->ai;
                    const uintptr_t ownerEnt = b->ent;
                    const size_t    backOff  = b->backOff;
                    const uint32_t  ownerCol = b->entCol;
                    const char*     ownerCls = b->entCls[0] ? b->entCls : "<no rtti>";
                    if (ownerAi) {
                        LOG("damagewatch:   [OWNER] *** ATTRIBUTED WITHOUT THE LIST *** "
                            "stateVec %p is owned by AI %p (back-pointer found at "
                            "stateVec+0x%03zX), so entity = AI + 0x28 = %p, RTTI '%s' "
                            "COL 0x%X %s. >>> D-39 DOES NOT BLOCK THIS PATH - no "
                            "candidate list, no re-capture, no WatchExec slot. <<<",
                            reinterpret_cast<void*>(sv), reinterpret_cast<void*>(ownerAi),
                            backOff, reinterpret_cast<void*>(ownerEnt), ownerCls, ownerCol,
                            (strstr(ownerCls, "HumanAI") && ownerCol == 0x28)
                                ? "PASS" : ">>>UNEXPECTED (wanted HumanAI / 0x28)");
                    } else {
                        LOG("damagewatch:   [OWNER] no back-pointer - swept stateVec %p "
                            "+0x000..0x%03zX and NOTHING in it points at an object whose "
                            "+0x%03zX reaches back here. So AIStateVector does not store "
                            "its owner where a plain qword scan can see it, and "
                            "attribution has to come from the candidate list below.",
                            reinterpret_cast<void*>(sv), humanai::kStateVectorSize,
                            humanai::kAiToStateVecPtr);
                    }

                    int attrHit = -1, attrBrute = -1;
                    size_t bruteOff = 0;
                    for (int ei = 0; ei < nEnt; ++ei) {
                        uintptr_t probe = 0;
                        if (weaponanchor::ReadPtr(
                                static_cast<uintptr_t>(
                                    static_cast<ptrdiff_t>(ents[ei]) +
                                    humanai::kEntityToStateVec), probe) &&
                            probe == sv) {
                            attrHit = ei;
                            break;
                        }
                    }
                    if (attrHit < 0) {
                        for (int ei = 0; ei < nEnt && attrBrute < 0; ++ei) {
                            for (size_t off = 0; off + sizeof(uintptr_t) <= 0x1800;
                                 off += sizeof(uintptr_t)) {
                                uintptr_t v = 0;
                                if (weaponanchor::ReadPtr(ents[ei] + off, v) && v == sv) {
                                    attrBrute = ei;
                                    bruteOff  = off;
                                    break;
                                }
                            }
                        }
                    }
                    if (attrHit >= 0) {
                        LOG("damagewatch:   [CHAIN] *** ATTRIBUTED *** entity %p + %+d "
                            "== this state vector. INV-56 CONFIRMED LIVE, and this hit "
                            "belongs to that body.",
                            reinterpret_cast<void*>(ents[attrHit]),
                            (int)humanai::kEntityToStateVec);
                    } else if (attrBrute >= 0) {
                        LOG("damagewatch:   [CHAIN] *** FOUND AT A DIFFERENT OFFSET *** "
                            "entity %p holds this state vector at +0x%03zX, NOT the "
                            "predicted %+d. INV-56's composed offset is WRONG - "
                            "hardcode what this line says.",
                            reinterpret_cast<void*>(ents[attrBrute]), bruteOff,
                            (int)humanai::kEntityToStateVec);
                    } else {
                        // THE LINE THAT DID NOT EXIST BEFORE.
                        // 2026-09-15: this call had TWO format specifiers and ONE
                        // argument, so it printed "at the predicted +0" instead of
                        // the real offset - a missing vararg, undefined behaviour,
                        // and it got through a build that promotes printf mismatches
                        // to errors. Both arguments are passed now.
                        LOG("damagewatch:   [CHAIN] not in the candidate list - this "
                            "state vector is in NONE of the %d frozen candidate "
                            "entit(y/ies), at entity%+d or at any offset in their first "
                            "6 KB. EXPECTED, and it is D-39: the list froze when "
                            "[BONEWATCH] armed (camhook.cpp ~976) so a body that spawned "
                            "later cannot be in it. >>> THIS IS NO LONGER THE "
                            "ATTRIBUTION PATH - read the [OWNER] line above, which "
                            "inverts the chain and needs no list. This line is only the "
                            "cross-check. <<<",
                            nEnt, (int)humanai::kEntityToStateVec);
                    }
                }
            }

            // ---- [HITCOUNT] next: the polled chain, kept as a cross-check ----
            for (int i = 0; i < s_haiCount; ++i) {
                // ============ U-21 FIX 1 OF 2: RE-VALIDATE THE BASE ============
                //
                // The base was RTTI-confirmed ONCE, at capture. Bodies die and
                // their heap gets recycled, and the candidate list is frozen
                // (D-39), so by the time this reads, s_hai[i].base can point at
                // reused memory. On 2026-09-15 20:51 that produced two *** HIT ***
                // lines off an entity that was never in the confirmed set.
                //
                // Re-running RTTI here would be an uncached walk of BOTH DLLs on
                // every target every 100 ms - the exact per-frame cost mistake
                // that cost two runs. So instead compare the VPTR latched at
                // capture: one guarded pointer read, and a recycled block will
                // essentially never carry the same vtable.
                uintptr_t liveVptr = 0;
                if (!weaponanchor::ReadPtr(s_hai[i].base, liveVptr) ||
                    liveVptr != s_hai[i].vptr) {
                    static volatile LONG saidStale = 0;
                    if (InterlockedCompareExchange(&saidStale, 1, 0) == 0) {
                        LOG("damagewatch: [HITCOUNT] STALE BASE - entity %p base %p had "
                            "vptr %p at capture and reads %p now, so the object has been "
                            "freed and its memory reused. REFUSING to poll it, and this "
                            "is U-21: the candidate list is frozen by D-39 while bodies "
                            "die. Reported ONCE; other targets may be stale too.",
                            reinterpret_cast<void*>(s_hai[i].ent),
                            reinterpret_cast<void*>(s_hai[i].base),
                            reinterpret_cast<void*>(s_hai[i].vptr),
                            reinterpret_cast<void*>(liveVptr));
                    }
                    continue;
                }

                uint32_t now = 0;
                if (!ReadFieldU32(s_hai[i].base + humanai::kHitCount, now)) { continue; }
                if (now == s_hai[i].hits) { continue; }
                const uint32_t was = s_hai[i].hits;
                s_hai[i].hits = now;
                // ============ U-21 FIX 2 OF 2: AN ABSOLUTE BOUND ============
                //
                // m_HitCount is MONOTONIC. A decrease, or a jump of thousands,
                // means this is not a counter - refuse to report it as a hit.
                //
                // >>> THAT WAS NOT ENOUGH. The guard checked DELTAS and never
                // MAGNITUDE, so on 2026-09-15 a counter sitting at 2,607,319,223
                // sailed through because it moved by only 96. A hit counter in
                // the billions is refusable on sight. Confirmed live 20:51:
                // fresh bodies read m_HitCount=0, and real hits read 1, 2, 3. <<<
                constexpr uint32_t kMaxPlausibleHits = 100000;
                if (now > kMaxPlausibleHits || was > kMaxPlausibleHits) {
                    static volatile LONG saidTooBig = 0;
                    if (InterlockedCompareExchange(&saidTooBig, 1, 0) == 0) {
                        float asF = 0.0f;
                        memcpy(&asF, &now, sizeof(asF));
                        LOG("damagewatch: [HITCOUNT] REFUSING (MAGNITUDE) - entity %p "
                            "base %p: m_HitCount reads %u, above the plausible bound of "
                            "%u. The delta was only %d, which is why the old monotonic "
                            "check passed it - it tested DELTAS and never MAGNITUDE. "
                            "Same bits as float: %.6g. NOT reporting these as hits. "
                            "This is U-21.",
                            reinterpret_cast<void*>(s_hai[i].ent),
                            reinterpret_cast<void*>(s_hai[i].base), now,
                            kMaxPlausibleHits, (int)(now - was),
                            static_cast<double>(asF));
                    }
                    continue;
                }
                if (now < was || (now - was) > 1000) {
                    static volatile LONG saidNotCounter = 0;
                    if (InterlockedCompareExchange(&saidNotCounter, 1, 0) == 0) {
                        float asF = 0.0f;
                        memcpy(&asF, &now, sizeof(asF));
                        LOG("damagewatch: [HITCOUNT] REFUSING - entity %p base %p "
                            "(COL 0x%X): value went %u -> %u. A monotonic counter "
                            "cannot do that, so +0x%03zX is NOT m_HitCount on this "
                            "object (same bits as float: %.6g). NOT reporting these "
                            "as hits.",
                            reinterpret_cast<void*>(s_hai[i].ent),
                            reinterpret_cast<void*>(s_hai[i].base), s_hai[i].off,
                            was, now, humanai::kHitCount,
                            static_cast<double>(asF));
                    }
                    continue;
                }

                float  loc[3] = {}, vec[3] = {}, dmgSum = 0.0f, phys = 0.0f;
                uint8_t part = 0, bone = 0, dir = 0, death = 0;
                uint32_t cuts = 0;
                const uintptr_t e = s_hai[i].base;   // complete object, not the subobject
                ReadFieldBytes(e + humanai::kLastHitLocation, loc, sizeof(loc));
                ReadFieldBytes(e + humanai::kLastHitVector,   vec, sizeof(vec));
                ReadFieldFloat(e + humanai::kLastDamageSum,       dmgSum);
                ReadFieldFloat(e + humanai::kLastHitPhysicDamage, phys);
                ReadFieldBytes(e + humanai::kLastHitPart,  &part,  1);
                ReadFieldBytes(e + humanai::kLastHitBone,  &bone,  1);
                ReadFieldBytes(e + humanai::kLastHitDir,   &dir,   1);
                ReadFieldBytes(e + humanai::kDeathType,    &death, 1);
                ReadFieldU32  (e + humanai::kLastHitForcedCutTypes, cuts);

                char corr[96] = "n/a (trigger not armed)";
                if (triggerOn) {
                    if (g_lastPressQpc == 0) {
                        strncpy_s(corr, sizeof(corr), "NO ATTACK PRESS yet", _TRUNCATE);
                    } else {
                        LARGE_INTEGER nq = {}, fq = {};
                        QueryPerformanceCounter(&nq);
                        QueryPerformanceFrequency(&fq);
                        const double ms = fq.QuadPart
                            ? (double)(nq.QuadPart - g_lastPressQpc) * 1000.0
                                  / (double)fq.QuadPart : -1.0;
                        _snprintf_s(corr, sizeof(corr), _TRUNCATE,
                                    "%.0f ms after ATTACK PRESS code=%d%s", ms,
                                    g_lastPressCode,
                                    (ms >= 0.0 && ms <= 400.0) ? "  <== CORRELATED" : "");
                    }
                }
                // Dump the recent input events beside the hit. THIS is how the
                // attack code gets identified - by what actually precedes a hit,
                // not by a filter I picked in advance.
                {
                    LARGE_INTEGER nq = {}, fq = {};
                    QueryPerformanceCounter(&nq);
                    QueryPerformanceFrequency(&fq);
                    const LONG seen = InterlockedCompareExchange(&g_evtSeq, 0, 0);
                    char ring[512];
                    int rl = 0;
                    const int first = (seen > kEvtRing) ? (seen - kEvtRing) : 0;
                    for (int k = first; k < seen && rl < (int)sizeof(ring) - 48; ++k) {
                        const InputEvt& e = g_evtRing[k % kEvtRing];
                        const double ago = fq.QuadPart
                            ? (double)(nq.QuadPart - e.qpc) * 1000.0 / (double)fq.QuadPart
                            : -1.0;
                        if (ago < 0.0 || ago > 1200.0) { continue; }
                        rl += _snprintf_s(ring + rl, sizeof(ring) - rl, _TRUNCATE,
                                          "%s%d@%.0fms=%.2f", rl ? ", " : "",
                                          e.code, ago, (double)e.val);
                    }
                    if (!rl) {
                        strncpy_s(ring, sizeof(ring),
                                  "(no input events in the last 1200 ms)", _TRUNCATE);
                    }
                    LOG("damagewatch:   [EVTRING] inputs before this hit: %s", ring);
                }
                LOG("damagewatch: *** HIT *** entity %p '%s'  m_HitCount %u -> %u"
                    "  BONE=%u PART=%u DIR=%u  LOCATION=(%.3f, %.3f, %.3f)  "
                    "VECTOR=(%.3f, %.3f, %.3f)  damageSum=%.3f physDmg=%.3f  "
                    "cutTypes=0x%08X deathType=%u  [%s]",
                    reinterpret_cast<void*>(e), s_hai[i].cls, was, now,
                    bone, part, dir,
                    (double)loc[0], (double)loc[1], (double)loc[2],
                    (double)vec[0], (double)vec[1], (double)vec[2],
                    (double)dmgSum, (double)phys, cuts, death, corr);
            }

            for (int i = 0; i < g_scanTargetCount; ++i) {
                // ===== TWO GUARDS BEFORE ANY DECREASE IS CALLED DAMAGE =====
                //
                // WHAT THEY COST US TO LEARN, 2026-09-18 12:56-13:01: the poll
                // reported 46 "*** DAMAGE ***" lines and NOT ONE was damage.
                // Forty-four of them landed inside FIVE MILLISECONDS across
                // seven different modules at 13:01:07, 1.1 s after the game's
                // own log wrote `Level OnDestroy <wasteland>` - the level
                // unloading. The other two were all six fields of one module
                // going 94.5180 -> 0.0000 together, ten seconds before the
                // first observed hit.
                //
                // The old comment above kFields claimed "a decrease in any of
                // these IS a damage event by construction". IT IS NOT. A free,
                // a death reset and a level unload are all decreases, and the
                // probe could not tell them from a blow. That is INV-6's
                // fail-closed rule broken in our own instrument.
                //
                // GUARD 1 - IDENTITY. Re-read the module's vptr. A health
                // module belongs to a body that dies; the block is then freed
                // and reused, and a recycled pointer reads as a huge drop.
                // Same test weaponanchor applies to the movement module.
                uintptr_t vnow = 0;
                const bool vok = weaponanchor::ReadPtr(g_scanTargets[i].base, vnow);
                if (g_scanTargets[i].vptr && (!vok || vnow != g_scanTargets[i].vptr)) {
                    if (!staleSaid[i]) {
                        staleSaid[i] = true;
                        LOG("damagewatch:   *** STALE MODULE - NOT DAMAGE *** %p "
                            "class '%s': vptr was %p, now %s. The body died and "
                            "this block belongs to somebody else. Dropping it "
                            "from this window rather than reporting the drop.",
                            reinterpret_cast<void*>(g_scanTargets[i].base),
                            g_scanTargets[i].cls,
                            reinterpret_cast<void*>(g_scanTargets[i].vptr),
                            vok ? "different" : "unreadable");
                    }
                    for (int k = 0; k < kFieldCount; ++k) { prevOk[i][k] = false; }
                    continue;
                }
                // GUARD 2 - SHAPE. Count the fields that went from a live value
                // to EXACTLY zero this sample. Independent limb pools do not
                // empty together; when every readable one does, that is a reset
                // or a free, not a blow. Reported as its own thing so it is
                // never silent - a death IS information, it is just not the
                // information this window is asking for.
                int zeroed = 0, live = 0;
                for (int k = 0; k < kFieldCount; ++k) {
                    if (!prevOk[i][k]) { continue; }
                    float probe = 0.0f;
                    if (!ReadFieldFloat(g_scanTargets[i].base + kFields[k].off, probe)) {
                        continue;
                    }
                    ++live;
                    if (prevVal[i][k] > 0.0f && probe == 0.0f) { ++zeroed; }
                }
                if (live >= 2 && zeroed == live) {
                    LOG("damagewatch:   *** ALL %d FIELDS -> 0 - RESET OR FREE, NOT "
                        "DAMAGE *** %p class '%s'. Independent pools do not empty "
                        "in one sample. This is a death reset or the level "
                        "unloading; it is NOT a blow and is not counted as one.",
                        live, reinterpret_cast<void*>(g_scanTargets[i].base),
                        g_scanTargets[i].cls);
                    for (int k = 0; k < kFieldCount; ++k) {
                        float nv = 0.0f;
                        if (ReadFieldFloat(g_scanTargets[i].base + kFields[k].off, nv)) {
                            prevVal[i][k] = nv;
                        }
                    }
                    continue;
                }
                for (int k = 0; k < kFieldCount; ++k) {
                    if (!prevOk[i][k]) { continue; }
                    float now = 0.0f;
                    if (!ReadFieldFloat(
                            g_scanTargets[i].base + kFields[k].off, now)) {
                        prevOk[i][k] = false;
                        continue;
                    }
                    const float was = prevVal[i][k];
                    prevVal[i][k] = now;
                    if (!(now < was)) { continue; }          // only DECREASES
                    if (!(was > 0.0f && was < 100000.0f)) { continue; }
                    if ((was - now) < 0.01f) { continue; }   // ignore float dust

                    const bool isPlayer = (g_scanTargets[i].entity == playerEnt);

                    // Correlation, the thing Jay asked for: how long before this
                    // health drop did the player press a melee input? A drop a few
                    // tens of ms after a swing is OUR hit. A drop with no recent
                    // swing is fire, a fall, or another zombie - and saying so is
                    // the whole point of having the trigger.
                    char corr[96] = "n/a (trigger not armed)";
                    if (triggerOn) {
                        if (g_lastPressQpc == 0) {
                            strncpy_s(corr, sizeof(corr),
                                      "NO ATTACK PRESS seen yet this session "
                                      "(axis/look events do not count)",
                                      _TRUNCATE);
                        } else {
                            LARGE_INTEGER nowQ = {}, freq = {};
                            QueryPerformanceCounter(&nowQ);
                            QueryPerformanceFrequency(&freq);
                            const double ms =
                                freq.QuadPart
                                    ? (double)(nowQ.QuadPart - g_lastPressQpc)
                                          * 1000.0 / (double)freq.QuadPart
                                    : -1.0;
                            _snprintf_s(corr, sizeof(corr), _TRUNCATE,
                                        "%.0f ms after ATTACK PRESS code=%d%s",
                                        ms, g_lastPressCode,
                                        (ms >= 0.0 && ms <= 400.0)
                                            ? "  <== CORRELATED" : "");
                        }
                    }

                    if (reported < 40) {
                        LOG("damagewatch:   *** DAMAGE *** module %p slot %u class "
                            "'%s'  %s (+0x%03zX) : %.4f -> %.4f  (-%.4f)%s  [%s]",
                            reinterpret_cast<void*>(g_scanTargets[i].base),
                            g_scanTargets[i].slot, g_scanTargets[i].cls,
                            kFields[k].name, kFields[k].off,
                            static_cast<double>(was), static_cast<double>(now),
                            static_cast<double>(was - now),
                            isPlayer ? "   <== PLAYER (you took a hit)"
                                     : "   <== NOT the player - THIS IS THE ONE",
                            corr);
                        ++reported;
                    }
                    // A limb field on something that is NOT the player outranks
                    // everything: that is a zombie losing health to our weapon,
                    // which is the entire question. Player hits score lower - they
                    // are the same pipeline but the wrong direction for R5.
                    const int score = (isPlayer ? 1 : 3) + (k < 5 ? 1 : 0);
                    if (score > hitScore) {
                        hitScore  = score;
                        hitTarget = i;
                        hitDword  = static_cast<int>(kFields[k].off / 4);
                        memcpy(&hitFrom, &was, 4);
                        memcpy(&hitTo, &now, 4);
                    }
                }
            }
        }

        if (hitTarget < 0) {
            LOG("damagewatch: WINDOW %lu - NO health field decreased on ANY of the %d "
                "modules in %d s. That is a real negative about THESE modules, not "
                "about the offsets: they come from the game's own field registrar. "
                "Either nothing took damage in view, or the bodies being hit are not "
                "among the scanned candidates. %d damage event(s) were printed "
                "above; %d HumanAI entit(y/ies) were polled for m_HitCount and "
                "NONE of their counters moved; %ld ATTACK PRESS(es) and %ld input events total were "
                "seen - if the PRESS count is 0 the negative is about the TRIGGER, "
                "not about the health fields.",
                windowNo, g_scanTargetCount, kDetectSeconds, reported,
                s_haiCount, g_pressSeq, g_swingSeq);
            // Nothing moved anywhere this window - the list is probably bodies
            // that are dead or far away. Ask for ONE fresh capture; the scan
            // thread is parked and costs nothing until asked.
            InterlockedExchange(&weaponanchor::g_recaptureReq, 1);
            Sleep(1000);
            continue;
        }

        // >>> DISABLED 2026-09-15: [BONEWATCH] owns the single WatchExec slot.
        // Arming a second client here is exactly the contention that invalidated
        // the 2026-09-10 melee run. The field-finding half still reports what it
        // saw; it simply does not take the slot. <<<
        if (s_boneArmed) {
            LOG("damagewatch: a health field moved, but NOT arming an exec watch "
                "on it - [BONEWATCH] holds the single WatchExec slot this "
                "session. The field-finding half still reported what it saw.");
            Sleep(1000);
            continue;
        }
        InterlockedExchange(&weaponanchor::g_damageWatchArmed, 1);
        const uintptr_t hm = g_scanTargets[hitTarget].base;
        char cls[96] = {};
        strncpy_s(cls, sizeof(cls), g_scanTargets[hitTarget].cls, _TRUNCATE);
        {
            float ff = 0.0f, tf = 0.0f;
            memcpy(&ff, &hitFrom, sizeof(ff));
            memcpy(&tf, &hitTo, sizeof(tf));
            LOG("damagewatch: *** LIVE HEALTH FIELD FOUND *** module %p slot %u "
                "class '%s' OFFSET +0x%03X : %.4f -> %.4f%s. Arming on THIS "
                "address - found by diffing memory, not by trusting a field name.",
                reinterpret_cast<void*>(hm), g_scanTargets[hitTarget].slot, cls,
                hitDword * 4, static_cast<double>(ff), static_cast<double>(tf),
                (g_scanTargets[hitTarget].entity == playerEnt) ? " (PLAYER)" : "");
        }
        const uintptr_t healthAddr = hm + static_cast<uintptr_t>(hitDword) * 4;
        unsigned int healthNow = 0;
        {
            __try {
                healthNow = *reinterpret_cast<const unsigned int*>(healthAddr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG("damagewatch: m_HealthBody at %p unreadable - refusing to arm",
                    reinterpret_cast<void*>(healthAddr));
                Sleep(5000);
                continue;
            }
        }
        // PRINT IT BOTH WAYS. The field registrar types m_HealthBody as a
        // CRTTIFieldUInt, and the previous version of this line printed %u only
        // and then told the reader to STOP if the number looked like garbage.
        // That advice would have aborted a CORRECT address: the 2026-09-10 run
        // read 1122092745, which is meaningless as an integer and is exactly
        // 112.8883 reinterpreted as IEEE-754. The registrar's declared type is
        // not evidence about the live representation, and one misread of this
        // line discards the right offset.
        //
        // Same bit pattern, two interpretations, no reinterpretation of memory
        // beyond the one read already performed - so this cannot itself fault.
        float healthAsFloat = 0.0f;
        memcpy(&healthAsFloat, &healthNow, sizeof(healthAsFloat));
        LOG("damagewatch: arming on module %p class '%s', field @ %p (module+0x%03X) "
            "which currently reads %u as uint / %.4f as float. This offset was "
            "MEASURED by block-diff this window, not taken from the field "
            "registrar - +0x%zx (`m_HealthBody`) was the registrar's answer and it "
            "never moved under damage across three runs.",
            reinterpret_cast<void*>(hm), cls,
            reinterpret_cast<void*>(healthAddr),
            static_cast<unsigned>(healthAddr - hm), healthNow,
            static_cast<double>(healthAsFloat), kHealthBodyOffset);

        if (!dlvr::WatchWritesStart(healthAddr)) {
            LOG("damagewatch: WatchWritesStart FAILED at %p (%s)",
                reinterpret_cast<void*>(healthAddr), dlvr::WatchExecStatus());
            Sleep(5000);
            continue;
        }
        // windowNo was already incremented by the detect phase above - this is
        // the SAME window, now armed on a body proven to be live.
        LOG("damagewatch: WINDOW %lu ARMED on m_HealthBody @ %p (class '%s'), a body "
            "whose health was JUST OBSERVED CHANGING. Keep fighting - hit a zombie "
            "or take a hit. Every function that writes this value will be named. "
            "Collecting 20 s, then re-arming automatically.",
            windowNo, reinterpret_cast<void*>(healthAddr), cls);
        Sleep(20000);
        dlvr::WatchWritesStop();
        {
            // ===== RE-READ THE VALUE, AND REPORT WHETHER IT MOVED =====
            //
            // THE DISCRIMINATOR THE 11:10 RUN DID NOT HAVE. "0 writers" had two
            // completely different explanations and no way to separate them:
            // (a) nothing damaged this body during the window, or (b) the watch
            // itself is broken / the address is not the live health field.
            // Reading the same 4 bytes again at window close separates them
            // outright, and costs one guarded read:
            //
            //   value MOVED but 0 writers  -> the WATCH is at fault. The store
            //                                 happened and DR0 did not trap it.
            //   value STILL but 0 writers  -> nothing wrote it. Either no damage
            //                                 reached THIS body (it is whatever
            //                                 entity was closest when
            //                                 weaponanchor published, not
            //                                 necessarily the player), or this is
            //                                 not the live field.
            //
            // Neither reading is a verdict on its own - both are printed with the
            // raw numbers beside them, per this project's "a verdict word is not
            // a result" rule.
            unsigned int healthEnd = healthNow;
            bool healthEndOk = false;
            __try {
                healthEnd = *reinterpret_cast<const unsigned int*>(healthAddr);
                healthEndOk = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                healthEndOk = false;
            }
            float healthEndF = 0.0f;
            memcpy(&healthEndF, &healthEnd, sizeof(healthEndF));

            dlvr::WriteHit whits[64] = {};
            const int wn = dlvr::WatchWritesGet(whits, 64);
            LOG("damagewatch: WINDOW %lu DISARMED - %d distinct writer(s) of "
                "m_HealthBody. health at window START %u / %.4f -> END %s%u / "
                "%.4f (%s). READ THE TWO TOGETHER: moved + 0 writers means the "
                "WATCH failed; unchanged + 0 writers means nothing wrote this "
                "body - it is the closest entity carrying a HealthModule, NOT "
                "necessarily you. Re-arming.",
                windowNo, wn, healthNow, static_cast<double>(healthAsFloat),
                healthEndOk ? "" : "(unreadable) ", healthEnd,
                static_cast<double>(healthEndF),
                !healthEndOk      ? "END UNREADABLE"
                : healthEnd != healthNow ? "*** VALUE CHANGED ***"
                                         : "unchanged");
            for (int i = 0; i < wn; ++i) {
                const dlvr::WriteHit& w = whits[i];
                // RVA computed here rather than left to the reader - WriteHit
                // already carries the module and its base, so there is no reason
                // to make anyone do the subtraction by hand.
                const unsigned long long rvaOf =
                    (w.moduleBase && w.rip >= w.moduleBase)
                        ? static_cast<unsigned long long>(w.rip - w.moduleBase)
                        : 0ull;
                LOG("damagewatch:   writer %d: %s+0x%llX (RIP %p) x%llu hits",
                    i, w.module[0] ? w.module : "<unknown module>", rvaOf,
                    reinterpret_cast<void*>(w.rip),
                    static_cast<unsigned long long>(w.count));
            }
            if (wn > 0) {
                LOG("damagewatch: HOW TO USE THIS - each RVA above is a function "
                    "to decompile against the staged .i64. NOTE the RIP is the "
                    "instruction AFTER the store, so the writing instruction is "
                    "just before it. These are OBSERVED writers, not nominated "
                    "candidates - unlike the last three runs there is no "
                    "hypothesis here to be wrong about.");
                // LATCH ONLY ON SUCCESS. The whole point of the repeating window:
                // a zero costs another 20 s, not the run.
                InterlockedExchange(&g_reported, 1);
            }
        }
        // Short gap between windows so coverage is effectively continuous - the
        // event we are waiting for is user-driven and we cannot predict it.
        Sleep(1000);
    }
    // Unreachable - the loop above never exits. Present only to satisfy the
    // compiler.
    //
    // THE EXEC-BREAKPOINT PATH THAT USED TO LIVE HERE WAS REMOVED, NOT DISABLED.
    // It nominated a function address and trapped its entry, and it produced
    // three verified-clean zeros on three reasoned candidates (0x2A2830 and
    // 0x279AF0, slot verified exclusive and binary md5 matched each time). The
    // APPROACH was wrong, not the addresses - so dead code kept "just in case"
    // would only invite the same guessing loop back. The WatchExec machinery
    // itself is untouched in watchwrite.cpp and stays available: once the data
    // watch above names a writer worth trapping BY NAME, arm it then.
    // diag_damage_watch_rva remains registered for exactly that case.
    return 0;
}

}  // namespace damagewatch

// Starts the damage-argument capture worker once per process. A cheap no-op
// while DiagDamageWatch is off, so it is safe to start unconditionally.
void StartDamageWatchOnce()
{
    if (InterlockedCompareExchange(&damagewatch::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, damagewatch::Thread, nullptr, 0, nullptr);
    }
}

// ---- RIGGING STAGE 0: dump the player's skeleton (2026-09-07) ---------------
//
// READ-ONLY. Changes no engine state; it only calls const accessors and logs.
//
// Everything the arm-rigging work needs downstream depends on one thing we do
// not have: the bone map. Which element index is R_Hand, what its parent is,
// and how long each segment is. Without that, nothing else can be written.
//
// WHY LOCAL MATRICES, NOT WORLD - the correction to how this was attempted
// before. The skeleton is a 3ds-Max Biped rig (the string "Bip01" is in the
// binary, alongside Clavicle/UpperArm/Forearm/L_HandHolder/R_HandHolder), so
// R_Hand is a CHILD of R_Forearm. Forcing the hand's WORLD matrix to the
// controller while the forearm stays wherever animation put it forces the
// skinned mesh to stretch between them - that is precisely the "breaks the
// forearms" symptom reported from prior attempts. Driving the chain's LOCAL
// rotations instead (upperarm, then forearm) never breaks the parent-child
// relationship at all: the hand inherits its position through the hierarchy,
// so there is nothing to stretch. `SetElementLocalMatrix`/`SetElementLocalMatrices`
// are exported for exactly this, which is why this probe records PARENT and
// LOCAL matrices rather than just world positions.
//
// Capture point: `CorrectItemPosition` (gamedll+0xC62AC0), which runs every
// frame with RCX = the player entity and RDX = hand index (0=right, 1=left).
// Per ENGINE_ARCHITECTURE.md the character's own body/skeleton object hangs off
// `entity+0x40`. That pointer is the IModelObject candidate; it is validated by
// calling GetMeshElementsMatricesCount() on it and sanity-checking the result
// rather than assuming, since "the pointer at a fixed offset is the class I
// expect" has been wrong repeatedly in this project.
//
// Partial EBones enum recovered statically (FUN_180aeced0, a CEnumInfo::Add
// series) for cross-checking whatever names come back:
//     SPINE=1 SPINE1=2 SPINE2=3 SPINE3=4 HEAD=8
//     L_UPPERARM=0x0B(11)  R_FOREARM=0x10(16)  L_THIGH=0x12(18)  R_CALF=0x15(21)
// Note EBones IDs and element indices are DIFFERENT numbering - GetMeshElemFromBoneID
// maps between them - and HackPABlender_BlockAnimation's mask uses the raw
// bone-array index, a third numbering again. This probe dumps all three so the
// relationship is measured, not inferred.
//
// EXEC-WATCH CONFLICT: this uses the single process-wide WatchExec slot. Every
// other diag_* that arms it (diag_visibility_probe, diag_player_movemod,
// diag_watch_line_add, diag_kill_physics_debug, smaarunwatch) MUST be off, or
// the two silently overwrite each other's breakpoint and both read the same
// results table - see CLAUDE.md's retraction of the HumanAI debug-line finding
// for what that looks like.
namespace boneprobe {

typedef unsigned int (*GetCountFn)(void* model);
typedef const char* (*GetNameFn)(void* model, int index);
typedef int (*GetParentFn)(void* model, int index);
typedef const float* (*GetLocalMtxFn)(void* model, int index);
typedef void (*SetLocalMtxFn)(void* model, int index, const float* m);
typedef int (*GetElemFromBoneIdFn)(void* model, unsigned char boneId);
typedef void* (*GetAnimBindFn)(void* model);
typedef const float* (*GetWorldMtxFn)(void* model, int index);
typedef void (*SetWorldMtxFn)(void* model, int index, const float* m);

volatile LONG g_started = 0;
volatile LONG g_done = 0;

// True while Thread() is running. BoneProbeRecapture() refuses to start a
// second one, and it cannot tell from g_done alone: g_done is set the moment
// the skeleton is captured, but the thread carries on afterwards into Stage 1
// and Stage 3 when those are enabled. Set on entry and cleared on EVERY exit
// by a scope guard rather than by hand - Thread() has four returns.
volatile LONG g_threadAlive = 0;

// Confirmed element indices for the right arm chain (Stage 0). Left arm is
// 9/12/13/17 in the same order. See CLAUDE.md's bone map.
constexpr int kRUpperArm = 48;
constexpr int kRForearm  = 49;
constexpr int kRHand     = 53;

// The live model, cached once Stage 0 has identified it: *(entity+0x50),
// RTTI PlayerDI, 93 elements. Only valid while that entity lives - a level
// load invalidates it, which the SEH guards turn into a logged fault rather
// than a crash.
void* g_model = nullptr;
// The entity CorrectItemPosition was called with - PlayerFppVis, the object
// IGSObject::Raytrace also takes as its ignore-argument, i.e. something with an
// IControlObject view. Kept because the rigging work needs the character''s own
// world transform rather than an animated bone''s.
uintptr_t g_entity = 0;

GetCountFn          g_getCount   = nullptr;
GetNameFn           g_getName    = nullptr;
GetParentFn         g_getParent  = nullptr;
GetLocalMtxFn       g_getLocal   = nullptr;
SetLocalMtxFn       g_setLocal   = nullptr;
GetElemFromBoneIdFn g_elemFromId = nullptr;
GetAnimBindFn       g_getAnimBind = nullptr;
GetWorldMtxFn       g_getWorld = nullptr;
SetWorldMtxFn       g_setWorld = nullptr;

// Each of these is isolated in its own tiny function so the SEH guard has no
// C++ objects to unwind (MSVC C2712) - same reason rtticam.cpp's SafeRead is
// split out.
uintptr_t ProbeReadPtr(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<volatile uintptr_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ProbeCount(void* model, unsigned int& out)
{
    __try {
        out = g_getCount(model);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeName(void* model, int i, const char*& out)
{
    __try {
        out = g_getName(model, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeParent(void* model, int i, int& out)
{
    __try {
        out = g_getParent(model, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// mtx34 is row-major 3x4: columns {0,4,8}/{1,5,9}/{2,6,10} are the local basis
// vectors, column {3,7,11} is the translation. For a bone's LOCAL matrix the
// translation is its offset from its parent, so its magnitude is the segment
// length - which is what an IK solve needs.
bool ProbeLocalTranslation(void* model, int i, float& x, float& y, float& z)
{
    __try {
        const float* m = g_getLocal(model, i);
        if (!m) {
            return false;
        }
        x = m[3]; y = m[7]; z = m[11];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies all 12 floats of a bone's local mtx34 out. Separate from
// ProbeLocalTranslation because Stage 1 needs the whole matrix, not just the
// translation column.
bool ProbeLocalMatrix(void* model, int i, float* out12)
{
    __try {
        const float* m = g_getLocal(model, i);
        if (!m) {
            return false;
        }
        for (int k = 0; k < 12; ++k) {
            out12[k] = m[k];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeSetLocalMatrix(void* model, int i, const float* m12)
{
    __try {
        g_setLocal(model, i, m12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THE STEP THAT WAS MISSING FOR THREE RUNS.
//
// ENGINE_ARCHITECTURE.md documents that every engine caller of
// SetMeshElementsMatrices follows the write with `IModelObject::GetAnimBind()`
// then a virtual call at `IAnimBind + 0x248`, passing a float that looks like
// a blend weight - i.e. "every writer explicitly notifies the animation-blend
// system immediately after overwriting the raw matrices, rather than silently
// poking bone data behind the animation system's back."
//
// Stages 1 and 2 did exactly that silent poking. The pose landed and held, the
// animation locked, and everything downstream (body-vs-camera, the weapon on
// r_handholder) desynced - which is what you would predict from a skeleton
// nobody told the blend system about. Rate, thread and frequency were all
// ruled out by measurement; this was never tested because it was dropped from
// the plan when the implementation switched to per-element local writes.
//
// vtable slot index = 0x248 / 8 = 73.
constexpr size_t kAnimBindNotifyVtIndex = 0x248 / 8;

// Defined further down; declared here so Stage 3 can use it.
bool ProbeCopyName(const char* src, char* dst, size_t cap);

uint32_t ProbeReadU32(uintptr_t addr)
{
    __try {
        return *reinterpret_cast<volatile uint32_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

// ---- STAGE 3 (read-only): resolve the THIRD numbering ----------------------
//
// Stages 1-2 established that neither matrix view is the authoritative pose, so
// taking control needs HackPABlender's per-bone block mask - and that mask is
// indexed by RAW BONE-ARRAY INDEX (0-57), not mesh-element index (0-92). Those
// are different index spaces and nothing we have maps between them.
//
// The engine's own Block(i)/Unblock(i) primitives (FUN_18015c920/FUN_18015c880)
// document the layout:
//   * bone array base = *(model + 0x388), stride 0x78 per bone
//   * "animation blocked" = bit 0x4000000 in a dword at bone + 0x38
//   * a bone-index -> mesh-element-index lookup at model + 0x408, where -1
//     means "this bone has no renderable mesh element"
//
// That lookup IS the mapping we need: dump it, cross-reference each entry
// against the Stage 0 element names, and the arm bones' mask indices fall out.
// Purely read-only - no writes, no engine state touched.
// Heap pointers on x64 Windows sit well below the module range; loaded modules
// live at 0x00007FF.......... So a 0x7FF... value where an object pointer is
// expected is a VTABLE, not an object. That tell has now appeared three times
// in this session (entity+0x40 in Stage 0, *(model+0x388) here) and cost a run
// each time - so it is a checked predicate now, not something to notice in the
// log afterwards.
bool LooksLikeHeapPtr(uintptr_t p)
{
    return p > 0x10000 && p < 0x00007F0000000000ULL;
}

void Stage3BoneArrayDump()
{
    const uintptr_t model = reinterpret_cast<uintptr_t>(g_model);

    // The documented offsets are relative to `modelInner` - the decompile of
    // Block(i) is FUN_18015c920(modelInner, boneIndex), NOT the IModelObject we
    // hold. Reading them off the model directly gave a MODULE-range pointer (a
    // vtable) for the bone array and null for the lookup. So sweep the model's
    // fields for a heap pointer that, treated as modelInner, yields a plausible
    // bone array AND a plausible lookup - validated on the lookup's CONTENTS
    // being element indices, not on an offset merely being non-null. The old
    // guard only fired when BOTH reads were null, so a wrong-but-non-null value
    // sailed straight through it.
    LOG("rig3: model=%p - resolving modelInner", g_model);
    uintptr_t inner = 0, boneBase = 0, lookup = 0;

    // RESOLVED STATICALLY (2026-09-07) - the sweep below is now only a fallback.
    // Hand-decoded HackPABlender_BlockAnimation (engine RVA 0x144B30):
    //     mov rsi,[rcx+8]        <- modelInner = *(this + 8)
    //     mov rdi,rdx            <- the mask
    //     cmp [rdx+8],ebx        <- mask->count at +0x08
    //   loop:
    //     mov rax,[rdi]          <- mask->data  at +0x00
    //     cmp byte [rbx+rax],0   <- data[i]==0 ? Block(inner,i) : Unblock(inner,i)
    // Both call targets compute to 0x15C920 / 0x15C880, matching the
    // FUN_18015c920 / FUN_18015c880 addresses recorded independently in
    // ENGINE_ARCHITECTURE.md - so this decode is cross-checked, not assumed.
    {
        const uintptr_t cand = ProbeReadPtr(model + 8);
        if (LooksLikeHeapPtr(cand)) {
            const uintptr_t bb = ProbeReadPtr(cand + 0x388);
            const uintptr_t lk = ProbeReadPtr(cand + 0x408);
            LOG("rig3:   *(model+8) = %p -> +0x388 = %p, +0x408 = %p  "
                "[decoded from HackPABlender_BlockAnimation]",
                reinterpret_cast<void*>(cand), reinterpret_cast<void*>(bb),
                reinterpret_cast<void*>(lk));
            // inner is confirmed by the decode; do NOT reject it just because a
            // sub-offset is not a heap pointer. inner+0x388 came back as a
            // MODULE address, which means the bone array is very likely
            // EMBEDDED at inner+0x388 (each bone a 0x78-byte object whose first
            // field is its own vtable) rather than pointed to. Same
            // address-vs-pointer ambiguity that has now cost three runs - so
            // evaluate BOTH forms instead of choosing one.
            // FOLLOW THE DECOMPILE, do not second-guess it with a heuristic.
            // The previous version scored candidates by reading `tbl + i*4` as
            // if tbl were the mapping array - but tbl is a STRUCT whose map
            // array lives at *(tbl+0x60). So the correct pointer scored 7/24,
            // an incorrect one scored 16/24, and the correct answer was thrown
            // away. Block(i) says plainly: tbl = *(inner + 0x408).
            inner = cand;
            lookup = lk;
            boneBase = 0;  // not needed: the mapping lives entirely in tbl
            LOG("rig3:   inner=%p, tbl=*(inner+0x408)=%p  (per Block(i) decompile)",
                reinterpret_cast<void*>(inner), reinterpret_cast<void*>(lookup));
        } else {
            LOG("rig3:   *(model+8) = %p is not a heap pointer - falling back to sweep",
                reinterpret_cast<void*>(cand));
        }
    }

    for (uintptr_t off = 0; off <= 0x800 && !inner; off += 8) {
        const uintptr_t cand = ProbeReadPtr(model + off);
        if (!LooksLikeHeapPtr(cand)) {
            continue;
        }
        const uintptr_t bb = ProbeReadPtr(cand + 0x388);
        const uintptr_t lk = ProbeReadPtr(cand + 0x408);
        if (!LooksLikeHeapPtr(bb) || !LooksLikeHeapPtr(lk)) {
            continue;
        }
        int sane = 0;
        for (int i = 0; i < 24; ++i) {
            const int v = static_cast<int>(ProbeReadU32(lk + static_cast<uintptr_t>(i) * 4));
            if (v == -1 || (v >= 0 && v < 256)) {
                ++sane;
            }
        }
        LOG("rig3:   candidate model+0x%03zX -> inner=%p boneBase=%p lookup=%p  sane %d/24",
            static_cast<size_t>(off), reinterpret_cast<void*>(cand),
            reinterpret_cast<void*>(bb), reinterpret_cast<void*>(lk), sane);
        if (sane >= 20) {
            inner = cand;
            boneBase = bb;
            lookup = lk;
        }
    }
    if (!inner) {
        const uintptr_t bb = ProbeReadPtr(model + 0x388);
        const uintptr_t lk = ProbeReadPtr(model + 0x408);
        if (LooksLikeHeapPtr(bb) && LooksLikeHeapPtr(lk)) {
            inner = model; boneBase = bb; lookup = lk;
            LOG("rig3:   the model IS its own inner");
        }
    }
    if (!inner) {
        LOG("rig3: no modelInner candidate. Heap pointers on the model, +0x000..0x800, "
            "with what each yields at +0x388/+0x408 (for manual inspection):");
        for (uintptr_t off = 0; off <= 0x800; off += 8) {
            const uintptr_t v = ProbeReadPtr(model + off);
            if (LooksLikeHeapPtr(v)) {
                LOG("rig3:   model+0x%03zX = %p  (+0x388 -> %p, +0x408 -> %p)",
                    static_cast<size_t>(off), reinterpret_cast<void*>(v),
                    reinterpret_cast<void*>(ProbeReadPtr(v + 0x388)),
                    reinterpret_cast<void*>(ProbeReadPtr(v + 0x408)));
            }
        }
        return;
    }
    LOG("rig3: RESOLVED  inner=%p  boneArrayBase=%p  lookup=%p",
        reinterpret_cast<void*>(inner), reinterpret_cast<void*>(boneBase),
        reinterpret_cast<void*>(lookup));

    // CORRECTED from the real Block(i) decompile
    // (re_analysis/hackpablender_primitives_decompiled.txt), which was already
    // on disk. Both my earlier guesses were wrong:
    //
    //   tbl      = *(inner + 0x408)
    //   count    = *(int*)(tbl + 0x38)
    //   mapArray = *(ptr*)(tbl + 0x60)
    //   elem     = *(int*)(mapArray + boneIndex * 8)      // stride 8, not 4
    //
    // I had been reading `lookup + i*4` - wrong indirection level AND wrong
    // stride. -1 means "this bone has no renderable mesh element".
    //
    // Also note inner+0x388 is NOT a pointer to the bone array: Block(i) calls
    // FUN_18009a520(inner + 0x388) and then takes *(result) + 0x18. It is a
    // handle you resolve through a function, which is why a raw read there
    // returned a vtable. We do not need it for the mapping, and for actual
    // blocking the exported wrapper does all of this internally.
    {
        const uintptr_t tbl = lookup;
        const int count = static_cast<int>(ProbeReadU32(tbl + 0x38));
        const uintptr_t mapArray = ProbeReadPtr(tbl + 0x60);
        LOG("rig3: --- REAL mapping: tbl=%p count=%d mapArray=*(tbl+0x60)=%p ---",
            reinterpret_cast<void*>(tbl), count, reinterpret_cast<void*>(mapArray));
        if (LooksLikeHeapPtr(mapArray) && count > 0 && count <= 256) {
            LOG("rig3:   boneIdx  ->  elem   elementName");
            for (int i = 0; i < count; ++i) {
                const int elem =
                    static_cast<int>(ProbeReadU32(mapArray + static_cast<uintptr_t>(i) * 8));
                if (elem < 0) {
                    continue;  // -1 = no mesh element for this bone
                }
                char nm[96] = {};
                const char* raw = nullptr;
                const bool named = ProbeName(g_model, elem, raw) &&
                                   ProbeCopyName(raw, nm, sizeof(nm));
                LOG("rig3:   %5d      %5d   %s", i, elem, named ? nm : "-");
            }
            LOG("rig3:   --- WANT: r_upperarm(48) r_forearm(49) r_hand(53) / "
                "l_upperarm(12) l_forearm(13) l_hand(17) ---");
        } else {
            LOG("rig3:   mapArray/count implausible - dumping tbl+0x00..0x80 raw:");
            for (uintptr_t o = 0; o <= 0x80; o += 8) {
                LOG("rig3:     tbl+0x%02zX = %p", static_cast<size_t>(o),
                    reinterpret_cast<void*>(ProbeReadPtr(tbl + o)));
            }
        }
    }

    // Legacy sweep below, kept only as a cross-check.
    LOG("rig3: --- (legacy cross-check sweep) ---");
    LOG("rig3:   boneIdx  elem   flags@+0x38   blocked  elementName");
    int mapped = 0;
    for (int i = 0; i < 64; ++i) {
        int elem = -1;
        if (lookup) {
            elem = static_cast<int>(ProbeReadU32(lookup + static_cast<uintptr_t>(i) * 4));
        }
        uint32_t flags = 0xFFFFFFFFu;
        if (boneBase) {
            flags = ProbeReadU32(boneBase + static_cast<uintptr_t>(i) * 0x78 + 0x38);
        }
        char nm[96] = {};
        bool named = false;
        if (elem >= 0 && elem < 512) {
            const char* raw = nullptr;
            if (ProbeName(g_model, elem, raw) && ProbeCopyName(raw, nm, sizeof(nm))) {
                named = true;
                ++mapped;
            }
        }
        if (named || (elem >= 0 && elem < 512) || (flags != 0xFFFFFFFFu && flags != 0)) {
            LOG("rig3:   %5d  %5d   0x%08X   %-7s  %s",
                i, elem, flags,
                (flags != 0xFFFFFFFFu && (flags & 0x4000000u)) ? "YES" : "no",
                named ? nm : "-");
        }
    }
    LOG("rig3: --- end, %d rows resolved to a named element. Wanted: r_upperarm(elem 48) "
        "r_forearm(49) r_hand(53) / l_upperarm(12) l_forearm(13) l_hand(17) ---", mapped);
}

// ---- STAGE 4: the engine's own idiom, end to end ---------------------------
//
// Stages 1-2 wrote bone matrices with the animation system still owning those
// bones and no flag handling - i.e. exactly what the engine's own code goes out
// of its way NOT to do. FUN_181136aa0 (gamedll), the only real caller of
// HackPABlender, shows the full sequence:
//
//     flags = IControlObject::GetFlags(model + 0x18);
//     IControlObject::ClearFlag(model + 0x18, 4);          // <- never done before
//     HackPABlender_UnblockAnimation(model, &mask);        // capture + clear all
//     ... force the pose ...
//     GetAnimBind(model) -> vtable +0x38 (int arg)         // <- NOT +0x248
//     SetMeshElementsState(...)
//     GetAnimBind(model) -> vtable +0x160 (no arg)
//     HackPABlender_BlockAnimation(model, &mask);          // restore
//     flags & 4 ? SetFlag(model+0x18, 4) : ClearFlag(...);
//
// The engine's use is ONE-SHOT (it restores the captured mask afterwards). Ours
// is a persistent override, so the usage differs: we BlockAnimation with a mask
// that is `false` at the arm bones and `true` everywhere else, which stops the
// animation system writing exactly those bones, then write the pose per frame.
//
// Mask semantics are inverted from the intuitive reading: false = BLOCKED,
// true = left under normal animation control.
//
// Bone-array indices (Stage 3, via *(tbl+0x60) + i*8):
//     left  arm: l_upperarm 14, l_forearm 15, l_hand 19
//     right arm: r_upperarm 57, r_forearm 58, r_hand 62
// NOTE the mask type is vectorm<58,bool> but the skeleton has 86 bones and the
// RIGHT arm sits past 58. That is fine for us - BlockAnimation loops against
// mask->count (our field) and Block(i) bounds-checks against the table's own
// count of 86 - but it does mean the engine's own callers cannot reach these
// bones, so their usage is not a template for ours.
constexpr int kBoneRUpperArm = 57;
constexpr int kBoneRForearm  = 58;
constexpr int kBoneRHand     = 62;
constexpr int kBoneCount     = 86;

// ttl::vectorm<N,bool> as BlockAnimation actually reads it: a data pointer and
// a count. It only ever reads those two fields, so a fabricated one is fine.
struct FakeVectorm {
    const unsigned char* data;
    unsigned int count;
    unsigned int pad;
};

typedef void (*BlockAnimFn)(void* model, const FakeVectorm* mask);
typedef unsigned int (*GetFlagsFn)(void* controlObj);
typedef void (*FlagFn)(void* controlObj, unsigned int flag);

BlockAnimFn g_blockAnim = nullptr;
// engine+0x15C920, void(longlong modelInner, uint boneIndex). Not exported;
// address cross-checked against BlockAnimation's own call target and the
// on-disk decompile. Touches exactly one bone - unlike the mask API.
typedef void (*BlockOneFn)(void* modelInner, unsigned int boneIndex);
BlockOneFn g_blockOne = nullptr;
GetFlagsFn  g_getFlags = nullptr;
FlagFn      g_setFlag = nullptr;
FlagFn      g_clearFlag = nullptr;

bool ProbeBlockOne(void* inner, int boneIndex)
{
    __try {
        g_blockOne(inner, static_cast<unsigned int>(boneIndex));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeBlockAnim(void* model, const FakeVectorm* mask)
{
    __try {
        g_blockAnim(model, mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeGetFlags(void* co, unsigned int& out)
{
    __try {
        out = g_getFlags(co);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeFlagCall(FlagFn fn, void* co, unsigned int flag)
{
    __try {
        fn(co, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The two AnimBind notifies the real idiom uses, at vtable +0x38 (int arg) and
// +0x160 (no arg). Distinct from the +0x248 used by SetMeshElementsMatrices
// callers, which is what Stage 2 called - landing, but on the wrong path.
bool ProbeAnimBindNotify2(void* model, int arg)
{
    __try {
        void* ab = g_getAnimBind(model);
        if (!ab) {
            return false;
        }
        void** vt = *reinterpret_cast<void***>(ab);
        if (!vt) {
            return false;
        }
        typedef void(*Fn38)(void*, int);
        typedef void(*Fn160)(void*);
        reinterpret_cast<Fn38>(vt[0x38 / 8])(ab, arg);
        reinterpret_cast<Fn160>(vt[0x160 / 8])(ab);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// WORLD-matrix variants. Unlike the local path, this one is PROVEN to be a
// route the engine itself uses: CorrectItemPosition drives this very skeleton
// every frame through SetElementWorldMatrix without breaking anything.
bool ProbeWorldMatrix(void* model, int i, float* out12)
{
    __try {
        const float* m = g_getWorld(model, i);
        if (!m) {
            return false;
        }
        for (int k = 0; k < 12; ++k) {
            out12[k] = m[k];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeSetWorldMatrix(void* model, int i, const float* m12)
{
    __try {
        g_setWorld(model, i, m12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeNotifyAnimBind(void* model, float weight)
{
    __try {
        void* ab = g_getAnimBind(model);
        if (!ab) {
            return false;
        }
        void** vt = *reinterpret_cast<void***>(ab);
        if (!vt) {
            return false;
        }
        typedef void(*NotifyFn)(void*, float);
        NotifyFn fn = reinterpret_cast<NotifyFn>(vt[kAnimBindNotifyVtIndex]);
        if (!fn) {
            return false;
        }
        fn(ab, weight);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeElemFromBoneId(void* model, unsigned char id, int& out)
{
    __try {
        out = g_elemFromId(model, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies a name out under SEH - the returned const char* is engine-owned and
// could be anything if the object is not really an IModelObject.
bool ProbeCopyName(const char* src, char* dst, size_t cap)
{
    __try {
        if (!src) {
            return false;
        }
        size_t n = 0;
        while (n < cap - 1 && src[n] != '\0') {
            dst[n] = src[n];
            ++n;
        }
        dst[n] = '\0';
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A plausible element COUNT proves nothing - confirmed the hard way on the
// first run: entity+0x8 returned "4 elements" purely because
// GetMeshElementsMatricesCount read some unrelated field, and every name,
// parent and matrix then came back as garbage. Validate on readable DATA
// instead: a real character skeleton has many elements, its names read as
// strings, and at least one of them looks like a Biped bone.
bool ValidateSkeleton(void* p, unsigned int& count, char* firstName, size_t cap)
{
    count = 0;
    if (!ProbeCount(p, count) || count < 8 || count > 512) {
        return false;
    }
    // 2026-11-09: the name-call probe line that stood here was removed from BETA for the tester package (Jay: "Strip
    // both"); DEV keeps it. Backup backups/20261109_225839_pre_beta_pkg_probe_strip/.
    int readable = 0;
    bool boneLike = false;
    if (cap) {
        firstName[0] = '\0';
    }
    const unsigned int probeMax = count < 24 ? count : 24;
    for (unsigned int i = 0; i < probeMax; ++i) {
        const char* raw = nullptr;
        char nm[96] = {};
        if (!ProbeName(p, static_cast<int>(i), raw) || !ProbeCopyName(raw, nm, sizeof(nm))) {
            continue;
        }
        ++readable;
        if (readable == 1 && cap) {
            _snprintf_s(firstName, cap, _TRUNCATE, "%s", nm);
        }
        // Case-insensitive search for tokens a Biped rig will always carry.
        char low[96] = {};
        for (size_t k = 0; nm[k] && k < sizeof(low) - 1; ++k) {
            low[k] = static_cast<char>(
                (nm[k] >= 'A' && nm[k] <= 'Z') ? nm[k] - 'A' + 'a' : nm[k]);
        }
        if (strstr(low, "bip") || strstr(low, "hand") || strstr(low, "arm") ||
            strstr(low, "spine") || strstr(low, "head") || strstr(low, "holder")) {
            boneLike = true;
        }
    }
    return readable >= 3 && boneLike;
}

bool DumpSkeleton(void* model, const char* label)
{
    unsigned int count = 0;
    if (!ProbeCount(model, count)) {
        LOG("boneprobe: %s (%p) - GetMeshElementsMatricesCount faulted, not an IModelObject",
            label, model);
        return false;
    }
    // Sanity bound. 58 is HackPABlender's mask capacity, so a real character
    // skeleton should land near that; anything wild means we resolved the
    // wrong pointer and must not be trusted.
    if (count == 0 || count > 512) {
        LOG("boneprobe: %s (%p) - implausible element count %u, rejecting",
            label, model, count);
        return false;
    }

    LOG("boneprobe: ===== %s (%p): %u elements =====", label, model, count);
    LOG("boneprobe:   idx  parent  name                                  localOffset(x,y,z)  len");
    for (unsigned int i = 0; i < count; ++i) {
        const char* raw = nullptr;
        char name[96] = {};
        if (!ProbeName(model, static_cast<int>(i), raw) ||
            !ProbeCopyName(raw, name, sizeof(name))) {
            name[0] = '?';
            name[1] = '\0';
        }
        int parent = -2;
        ProbeParent(model, static_cast<int>(i), parent);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        const bool gotMtx = ProbeLocalTranslation(model, static_cast<int>(i), x, y, z);
        const float len = gotMtx ? sqrtf(x * x + y * y + z * z) : -1.0f;
        LOG("boneprobe:   %3u  %6d  %-36s  %8.4f %8.4f %8.4f  %7.4f",
            i, parent, name, x, y, z, len);
    }

    // EBones id -> element index, for the ids recovered statically plus a
    // sweep either side so the full arm/leg blocks can be inferred.
    LOG("boneprobe:   --- GetMeshElemFromBoneID(id) -> element index ---");
    for (int id = 0; id <= 31; ++id) {
        int elem = -99;
        if (!ProbeElemFromBoneId(model, static_cast<unsigned char>(id), elem)) {
            continue;
        }
        if (elem < 0) {
            continue;  // unmapped ids are the norm; only log real hits
        }
        char name[96] = {};
        const char* raw = nullptr;
        if (ProbeName(model, elem, raw)) {
            ProbeCopyName(raw, name, sizeof(name));
        }
        LOG("boneprobe:   boneID %2d (0x%02X) -> element %3d  %s", id, id, elem, name);
    }
    LOG("boneprobe: ===== end %s =====", label);
    return true;
}

// ---- STAGE 1: the null round-trip -----------------------------------------
//
// Three questions, in order, each a prerequisite for the next:
//   Q1 Are we reading LIVE data? A bone's local matrix must CHANGE over time
//      while the character animates. If it never changes we are reading a
//      static bind pose (or the wrong memory) and everything downstream is
//      built on nothing. Checked first, costs nothing.
//   Q2 Does the ABI round-trip? Write a matrix straight back unchanged, read
//      it again, compare. A mismatch means our mtx34 layout or the calling
//      convention is wrong - caught here rather than after a real pose write
//      has visibly mangled the character.
//   Q3 Does a write SURVIVE? Whether the value we wrote is still there on the
//      next pass. If the animation system has already overwritten it, a write
//      from this thread can never hold a pose, and Stage 2 needs either the
//      per-bone block mask or a write point on the engine's own frame.
//
// Deliberately writes UNCHANGED data, so even a torn write is harmless - the
// bytes going in are the bytes already there. That is what makes it safe from
// a worker thread; Stage 2 (a real offset) must move to a per-frame site on
// the engine's own thread.
void Stage1NullRoundTrip()
{
    const int elems[3] = { kRUpperArm, kRForearm, kRHand };
    const char* names[3] = { "r_upperarm", "r_forearm", "r_hand" };
    static float prev[3][12] = {};
    static bool havePrev = false;

    for (int e = 0; e < 3; ++e) {
        float before[12] = {};
        if (!ProbeLocalMatrix(g_model, elems[e], before)) {
            LOG("rig1: %s (elem %d) - GetElementLocalMatrix faulted (entity gone?)",
                names[e], elems[e]);
            return;
        }

        // Q1: is this live, animating data?
        bool changed = false;
        if (havePrev) {
            for (int k = 0; k < 12; ++k) {
                if (fabsf(before[k] - prev[e][k]) > 1e-6f) {
                    changed = true;
                    break;
                }
            }
        }

        // Q2: write it back unchanged, read again, compare.
        if (!ProbeSetLocalMatrix(g_model, elems[e], before)) {
            LOG("rig1: %s (elem %d) - SetElementLocalMatrix FAULTED - write path unusable",
                names[e], elems[e]);
            return;
        }
        float after[12] = {};
        if (!ProbeLocalMatrix(g_model, elems[e], after)) {
            LOG("rig1: %s - read-back after write faulted", names[e]);
            return;
        }
        int mismatches = 0;
        float worst = 0.0f;
        for (int k = 0; k < 12; ++k) {
            const float d = fabsf(after[k] - before[k]);
            if (d > 1e-6f) {
                ++mismatches;
                if (d > worst) {
                    worst = d;
                }
            }
        }

        LOG("rig1: %-11s elem %2d | live=%-3s roundtrip=%-4s (%d/12 differ, worst %.6f) | "
            "basisX %.4f %.4f %.4f  trans %.4f %.4f %.4f",
            names[e], elems[e],
            havePrev ? (changed ? "YES" : "no") : "?",
            mismatches == 0 ? "OK" : "BAD", mismatches, worst,
            before[0], before[4], before[8],
            before[3], before[7], before[11]);

        for (int k = 0; k < 12; ++k) {
            prev[e][k] = before[k];
        }
    }
    havePrev = true;
}

// ---- STAGE 2: a deliberate static pose write, ON THE ENGINE'S THREAD -------
//
// Stage 1 proved the API works (live data, exact round-trip) but ALSO proved
// the write is not inert: driving it from a worker thread at 2 Hz desynced the
// bone cache and detached the body/weapon from the camera. The matrix system
// is a lazy, dirty-flagged, per-thread-double-buffered cache with an
// attachment cascade, so the WRITE ITSELF forces a recompute - doing that out
// of phase with the engine's own tick is the problem, not the data.
//
// So this runs from dlvr_OnCameraCopied: the engine's own render thread, at
// the point camhook.h's header describes as "the engine has just decided where
// the camera points, and nothing has read it yet".
//
// It pins r_forearm to a FIXED pose - a reference basis captured once, with a
// constant rotation applied - rather than nudging the live value. That matters:
// re-reading and re-rotating each frame would compound and spin the arm, while
// a fixed absolute target makes the outcome unambiguous:
//
//   * forearm visibly STUCK in a wrong pose while the rest of the arm animates
//     -> our write holds; this point is after the animation tick. No block
//        mask needed and Stage 3 can drive from here.
//   * forearm animates normally, log says clobbered
//     -> the animation tick runs later and overwrites us. We then need
//        HackPABlender's per-bone block mask, which reopens the unresolved
//        element-index -> bone-array-index mapping.
//   * flicker / mixed counts
//     -> racing the tick; needs a different point in the frame.
//
// Translation is never written from our own data. Stage 1 showed r_forearm's
// local translation is rigid (0.2865 every sample) but r_upperarm's is NOT -
// the shoulder slides - so "only write what you actually computed" is the
// habit to start with rather than retrofit.
volatile LONG g_stage2Ref = 0;
float g_stage2RefMtx[12] = {};
float g_stage2LastWritten[12] = {};
float g_stage2PreWrite[12] = {};
volatile LONG g_stage2HaveWritten = 0;
LONG64 g_stage2LastLogMs = 0;
LONG g_stage2Held = 0;
LONG g_stage2Clobbered = 0;

// RATE GUARD - added after the first run measured what this actually costs.
// The first version was called from dlvr_OnCameraCopied and logged 5,196,878
// writes in ~132s = ~35,000/sec (~300 camera copies per frame at 120fps).
// Each write dirties the bone and forces a recompute cascade, and that volume
// is the likely cause of the body detaching from the camera - NOT the calling
// thread, which was the earlier (probably wrong) Stage 1 diagnosis: Stage 1
// wrote at 2 Hz off-thread and Stage 2 at 35 kHz on-thread, and both detached.
// The common factor is the write, not the thread.
//
// This project already had a standing rule about exactly this - "check real
// hit frequency before hooking" - learned from WeaponFireControllerRight
// (30-60k hits/sec) and the melee dispatcher. Assuming a camera hook is
// per-frame was the error; g_calls was available to measure it.
//
// So: bound it here regardless of what the call site does. ~1 write per 8ms
// is once per frame at 120Hz, which is all a pose override ever needs.
constexpr LONG64 kStage2MinIntervalMs = 8;
LONG64 g_stage2LastWriteMs = 0;

// ONCE PER FRAME, NOT ONCE PER EYE. LevelCamera runs for each actively-driven
// camera, i.e. both eyes. Writing on both means the pose can change BETWEEN
// the two eyes' renders, which is exactly the "weapon rapidly flickers"
// symptom the user reported (confirmed rapid alternation, not stable-
// monocular). Anchoring on whichever camera object we see first pins the write
// to one consistent point in the frame. Re-anchors if that camera goes quiet
// for half a second, so a level load or camera swap cannot strand it.
uintptr_t g_stage2FrameAnchor = 0;

void Stage2StaticPose(uintptr_t obj)
{
    if (!g_model || !g_setLocal || !g_getLocal) {
        return;
    }
    const LONG64 nowMs = static_cast<LONG64>(GetTickCount64());
    if (g_stage2FrameAnchor == 0 || nowMs - g_stage2LastWriteMs > 500) {
        g_stage2FrameAnchor = obj;
    }
    if (obj != g_stage2FrameAnchor) {
        return;
    }
    if (nowMs - g_stage2LastWriteMs < kStage2MinIntervalMs) {
        return;
    }
    g_stage2LastWriteMs = nowMs;

    // NARROW TEST (2026-09-07): local vs WORLD.
    // Evidence so far says SetElementLocalMatrix writes into a representation
    // the skinning path does not read - our data persisted (held 99.8%), the
    // animbind notify landed (fail=0), and the arm still animated normally.
    // What the local write DID do was dirty the cache, producing the load-time
    // body freak-out and the one-eye rivalry on hand+gun. So test the world
    // path, which CorrectItemPosition itself uses on this skeleton every frame.
    const bool useWorld = RigStage2UseWorld;

    float cur[12] = {};
    const bool gotCur = useWorld ? ProbeWorldMatrix(g_model, kRForearm, cur)
                                 : ProbeLocalMatrix(g_model, kRForearm, cur);
    if (!gotCur) {
        return;
    }

    // COMPOUNDING GUARD. In world mode we rotate the CURRENT matrix rather
    // than a frozen reference, so the arm keeps following the body instead of
    // being pinned in world space. That is only safe if the engine recomputes
    // the world matrix from animation each frame; if it does not, we would be
    // re-rotating our own output and the arm would spin up. So: if what we read
    // is what we wrote last frame, the engine did NOT recompute - fall back to
    // the pre-rotation value we saved, which keeps the offset fixed at exactly
    // one rotation either way. The held/clobbered counters record which case
    // we are actually in, which is itself part of the answer.
    if (useWorld && InterlockedCompareExchange(&g_stage2HaveWritten, 0, 0) != 0) {
        int same = 0;
        for (int k = 0; k < 12; ++k) {
            if (fabsf(cur[k] - g_stage2LastWritten[k]) <= 1e-5f) {
                ++same;
            }
        }
        if (same == 12) {
            for (int k = 0; k < 12; ++k) {
                cur[k] = g_stage2PreWrite[k];
            }
        }
    }
    for (int k = 0; k < 12; ++k) {
        g_stage2PreWrite[k] = cur[k];
    }

    if (InterlockedCompareExchange(&g_stage2Ref, 1, 0) == 0) {
        for (int k = 0; k < 12; ++k) {
            g_stage2RefMtx[k] = cur[k];
        }
        LOG("rig2: reference basis captured for r_forearm (elem %d): "
            "basisX %.4f %.4f %.4f trans %.4f %.4f %.4f",
            kRForearm, cur[0], cur[4], cur[8], cur[3], cur[7], cur[11]);
    }

    // Did last frame's write survive into this frame?
    if (InterlockedCompareExchange(&g_stage2HaveWritten, 0, 0) != 0) {
        int diff = 0;
        for (int k = 0; k < 12; ++k) {
            if (fabsf(cur[k] - g_stage2LastWritten[k]) > 1e-5f) {
                ++diff;
            }
        }
        if (diff == 0) {
            ++g_stage2Held;
        } else {
            ++g_stage2Clobbered;
        }
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - g_stage2LastLogMs >= 1000) {
            g_stage2LastLogMs = now;
            LOG("rig2: held=%ld clobbered=%ld (%s) | current basisX %.4f %.4f %.4f",
                g_stage2Held, g_stage2Clobbered,
                g_stage2Clobbered > g_stage2Held
                    ? "ANIMATION TICK RUNS LATER - write overwritten, block mask needed"
                    : "WRITE HOLDS - this point is after the animation tick",
                cur[0], cur[4], cur[8]);
        }
    }

    // reference basis * Rz(angle). Columns are the basis vectors:
    // col0 = (m[0],m[4],m[8]), col1 = (m[1],m[5],m[9]), col2 = (m[2],m[6],m[10]).
    // Post-multiplying rotates the bone about its OWN local Z.
    // Local mode pins to the frozen reference (as before); world mode rotates
    // the live matrix so the arm still follows the body while being visibly
    // bent - otherwise pinning a WORLD matrix would leave the forearm behind
    // in world space as the player walks away from it.
    const float* base = useWorld ? cur : g_stage2RefMtx;

    const float rad = RigStage2AngleDeg * 3.14159265358979f / 180.0f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);
    float out[12] = {};
    for (int row = 0; row < 3; ++row) {
        const float c0 = base[row * 4 + 0];
        const float c1 = base[row * 4 + 1];
        const float c2 = base[row * 4 + 2];
        out[row * 4 + 0] = c0 * cs + c1 * sn;
        out[row * 4 + 1] = -c0 * sn + c1 * cs;
        out[row * 4 + 2] = c2;
        out[row * 4 + 3] = cur[row * 4 + 3];  // never our own translation
    }

    const bool wrote = useWorld ? ProbeSetWorldMatrix(g_model, kRForearm, out)
                                : ProbeSetLocalMatrix(g_model, kRForearm, out);
    if (!wrote) {
        LOG("rig2: Set%sMatrix faulted - disabling", useWorld ? "ElementWorld" : "ElementLocal");
        DiagRigStage2 = false;
        return;
    }

    // The engine's own idiom: never leave the blend system unaware of a raw
    // matrix write. Missing this for three runs is the leading explanation for
    // the body/weapon desync - see ProbeNotifyAnimBind's comment.
    if (RigNotifyAnimBind) {
        static LONG64 lastNotifyLogMs = 0;
        static LONG notifyOk = 0;
        static LONG notifyFail = 0;
        if (ProbeNotifyAnimBind(g_model, RigNotifyWeight)) {
            ++notifyOk;
        } else {
            ++notifyFail;
        }
        const LONG64 t = static_cast<LONG64>(GetTickCount64());
        if (t - lastNotifyLogMs >= 2000) {
            lastNotifyLogMs = t;
            LOG("rig2: animbind notify ok=%ld fail=%ld (weight %.3f, vtable+0x248)",
                notifyOk, notifyFail, RigNotifyWeight);
        }
    }

    for (int k = 0; k < 12; ++k) {
        g_stage2LastWritten[k] = out[k];
    }
    InterlockedExchange(&g_stage2HaveWritten, 1);
}

// Stage 4 state. Setup runs once (clear flag 4, block the arm bones); the pose
// write then runs per frame from LevelCamera as Stage 2 did.
volatile LONG g_s4Setup = 0;
unsigned char g_s4Mask[kBoneCount];
unsigned int g_s4SavedFlags = 0;
LONG64 g_s4LastWriteMs = 0;
LONG64 g_s4LastLogMs = 0;
LONG g_s4Held = 0;
LONG g_s4Clobbered = 0;
float g_s4Ref[12] = {};
volatile LONG g_s4HaveRef = 0;
float g_s4LastWritten[12] = {};
volatile LONG g_s4HaveWritten = 0;
// eyecamera (element 8) world position sampled immediately BEFORE our write,
// so the jitter probe can measure movement caused by the write itself rather
// than by real head/player motion between ticks.
float g_s4CamBefore[3] = {};

void Stage4Setup()
{
    void* co = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_model) + 0x18);

    if (ProbeGetFlags(co, g_s4SavedFlags)) {
        LOG("rig4: IControlObject(model+0x18) flags = 0x%08X (value 4 = %s)",
            g_s4SavedFlags, (g_s4SavedFlags & 4) ? "SET" : "clear");
    } else {
        LOG("rig4: GetFlags faulted");
    }
    // The engine clears this for the duration of pose manipulation. We never
    // did across four attempts, and it is the leading candidate for the body
    // detaching from the camera.
    if (!ProbeFlagCall(g_clearFlag, co, 4)) {
        LOG("rig4: ClearFlag(4) faulted");
    }

    // DO NOT USE THE MASK API HERE.
    //
    // BlockAnimation applies the WHOLE mask - false => Block(i), true =>
    // UNBLOCK(i). So a mask of "true everywhere except my three bones" does not
    // block three bones: it force-UNBLOCKS the other 83. That is what turned
    // the character into a noodle on the first Stage 4 run - legs frozen while
    // the torso drifted - because bones the game had legitimately blocked
    // (blending, IK, ragdoll) were all released at once.
    //
    // The engine avoids this by capturing the existing mask first and restoring
    // it. We cannot reuse that: the mask type is vectorm<58,bool> but the right
    // arm sits at bones 57/58/62, so a 58-entry container cannot even address
    // our bones.
    //
    // So call the Block(i) primitive directly - it touches exactly one bone and
    // leaves every other bone's state untouched. Not exported, but the address
    // is known and cross-checked two ways: the call-target arithmetic inside
    // BlockAnimation, and re_analysis/hackpablender_primitives_decompiled.txt.
    // Signature: void(longlong modelInner, uint boneIndex), engine+0x15C920.
    const uintptr_t inner = ProbeReadPtr(reinterpret_cast<uintptr_t>(g_model) + 8);
    if (!inner || !g_blockOne) {
        LOG("rig4: no modelInner (%p) or Block(i) unresolved (%p) - cannot proceed",
            reinterpret_cast<void*>(inner), reinterpret_cast<void*>(g_blockOne));
        DiagRigStage4 = false;
        return;
    }
    const int bones[3] = { kBoneRUpperArm, kBoneRForearm, kBoneRHand };
    const char* boneNames[3] = { "r_upperarm", "r_forearm", "r_hand" };
    int ok = 0;
    for (int i = 0; i < 3; ++i) {
        if (ProbeBlockOne(reinterpret_cast<void*>(inner), bones[i])) {
            ++ok;
        } else {
            LOG("rig4: Block(%d / %s) faulted", bones[i], boneNames[i]);
        }
    }
    LOG("rig4: Block(i) applied to %d/3 bones (%d/%d/%d) - every other bone left EXACTLY "
        "as the game had it", ok, bones[0], bones[1], bones[2]);
    if (ok == 0) {
        DiagRigStage4 = false;
    }
}

void Stage4Pose(uintptr_t obj)
{
    if (!g_model || !g_setLocal || !g_blockAnim) {
        return;
    }
    // obj == 0 means "called from a non-camera site" (the scePad poll), where
    // there is no per-eye anchoring to do - the rate guard alone is enough.
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    if (obj != 0) {
        if (g_stage2FrameAnchor == 0 || now - g_s4LastWriteMs > 500) {
            g_stage2FrameAnchor = obj;
        }
        if (obj != g_stage2FrameAnchor) {
            return;
        }
    }
    if (now - g_s4LastWriteMs < kStage2MinIntervalMs) {
        return;
    }
    g_s4LastWriteMs = now;

    if (InterlockedCompareExchange(&g_s4Setup, 1, 0) == 0) {
        Stage4Setup();
        return;  // let the block settle for one frame before writing
    }

    // PER-TICK RE-BLOCK REMOVED (2026-09-07). It was added to stop the game
    // overriding our pose on weapon equip, and it FAILED at that (clobbered
    // still climbed: 429 held vs 3529 clobbered) while making everything worse:
    // the noodle deformation returned and the camera ascended into the skybox.
    //
    // It was also genuinely unsafe. It re-read inner = *(g_model + 8) every
    // tick and called Block(inner, i), which does an unguarded
    // `*(uint*)(bone+0x38) |= 0x4000000`. If g_model is ever stale, that is an
    // arbitrary memory write - and a camera flying into the skybox is what
    // memory corruption looks like. Blocking ONCE, on a freshly captured
    // pointer, does not carry that risk.
    //
    // Conclusion: the intermittent override is NOT simply the game unblocking
    // our bones, or re-asserting the block would have fixed it. Needs a real
    // diagnosis, not a bigger hammer.

    float cur[12] = {};
    if (!ProbeLocalMatrix(g_model, kRForearm, cur)) {
        return;
    }
    if (InterlockedCompareExchange(&g_s4HaveRef, 1, 0) == 0) {
        for (int k = 0; k < 12; ++k) {
            g_s4Ref[k] = cur[k];
        }
        LOG("rig4: reference captured, basisX %.4f %.4f %.4f", cur[0], cur[4], cur[8]);
    }
    if (InterlockedCompareExchange(&g_s4HaveWritten, 0, 0) != 0) {
        int diff = 0;
        for (int k = 0; k < 12; ++k) {
            if (fabsf(cur[k] - g_s4LastWritten[k]) > 1e-5f) {
                ++diff;
            }
        }
        (diff == 0) ? ++g_s4Held : ++g_s4Clobbered;
        if (now - g_s4LastLogMs >= 1000) {
            g_s4LastLogMs = now;
            LOG("rig4: held=%ld clobbered=%ld | live basisX %.4f %.4f %.4f",
                g_s4Held, g_s4Clobbered, cur[0], cur[4], cur[8]);
        }
    }

    // OSCILLATE rather than hold a static angle. With the body detached AND its
    // pose frozen, a fixed 60-degree offset is impossible to judge by eye - the
    // user's words were "hard to tell when the character pose itself is
    // frozen". A forearm sweeping back and forth over a 2s period is
    // unmistakable whether or not anything else is animating, so the probe
    // stays readable in exactly the failure mode we are actually in.
    const float phase = static_cast<float>(now % 2000) / 2000.0f;
    const float swing = sinf(phase * 6.28318530718f);  // -1 .. +1
    const float rad = RigStage2AngleDeg * swing * 3.14159265358979f / 180.0f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);
    float out[12] = {};
    for (int row = 0; row < 3; ++row) {
        const float c0 = g_s4Ref[row * 4 + 0];
        const float c1 = g_s4Ref[row * 4 + 1];
        const float c2 = g_s4Ref[row * 4 + 2];
        out[row * 4 + 0] = c0 * cs + c1 * sn;
        out[row * 4 + 1] = -c0 * sn + c1 * cs;
        out[row * 4 + 2] = c2;
        out[row * 4 + 3] = cur[row * 4 + 3];
    }
    {
        float camPre[12] = {};
        if (ProbeWorldMatrix(g_model, 8, camPre)) {
            g_s4CamBefore[0] = camPre[3];
            g_s4CamBefore[1] = camPre[7];
            g_s4CamBefore[2] = camPre[11];
        }
    }
    if (!ProbeSetLocalMatrix(g_model, kRForearm, out)) {
        LOG("rig4: SetElementLocalMatrix faulted - disabling");
        DiagRigStage4 = false;
        return;
    }
    ProbeAnimBindNotify2(g_model, -1);

    // ---- CAMERA-BONE JITTER PROBE (read-only) -----------------------------
    //
    // The user reports severe camera/world shake outside the safe zone. Two
    // candidate causes, and they need different fixes, so measure rather than
    // guess:
    //   (a) CONTENTION - the block lapsing and being re-asserted makes
    //       r_forearm flip between the animation's pose and ours at ~125 Hz.
    //       Each flip dirties the shared matrix cache, whose recompute includes
    //       eyecamera (element 8). The per-tick re-block above should kill this.
    //   (b) THE WRITE ITSELF - even an uncontested write invalidates the shared
    //       cache every tick and perturbs the camera bone regardless.
    //
    // eyecamera is element 8, parented to hspine1(7) - NOT to the arm - so its
    // world matrix has no business moving when we touch r_forearm. Sample it
    // immediately before and after our write: if the camera bone's translation
    // shifts across our own write, that is (b) and the shake is ours to fix at
    // the write. If it is stable here but the view still shakes, it is (a) or
    // something outside this path entirely.
    {
        static float prevCam[3] = {};
        static bool havePrevCam = false;
        static LONG64 lastCamLogMs = 0;
        static float maxDeltaAcross = 0.0f;
        static float maxDeltaFrame = 0.0f;

        float camAfter[12] = {};
        if (ProbeWorldMatrix(g_model, 8, camAfter)) {
            const float cx = camAfter[3], cy = camAfter[7], cz = camAfter[11];
            // How far the camera bone moved ACROSS our write this tick.
            const float dAcross = sqrtf((cx - g_s4CamBefore[0]) * (cx - g_s4CamBefore[0]) +
                                        (cy - g_s4CamBefore[1]) * (cy - g_s4CamBefore[1]) +
                                        (cz - g_s4CamBefore[2]) * (cz - g_s4CamBefore[2]));
            if (dAcross > maxDeltaAcross) {
                maxDeltaAcross = dAcross;
            }
            // How far it moved between ticks (real head/player motion lives
            // here, so this is expected to be nonzero - it is the baseline the
            // across-write number has to be judged against).
            if (havePrevCam) {
                const float dFrame = sqrtf((cx - prevCam[0]) * (cx - prevCam[0]) +
                                           (cy - prevCam[1]) * (cy - prevCam[1]) +
                                           (cz - prevCam[2]) * (cz - prevCam[2]));
                if (dFrame > maxDeltaFrame) {
                    maxDeltaFrame = dFrame;
                }
            }
            prevCam[0] = cx; prevCam[1] = cy; prevCam[2] = cz;
            havePrevCam = true;

            if (now - lastCamLogMs >= 1000) {
                lastCamLogMs = now;
                LOG("rig4cam: eyecamera(elem 8) pos %.4f %.4f %.4f | max move ACROSS our "
                    "write %.5f | max move BETWEEN ticks %.5f  (across >> 0 means the write "
                    "itself moves the camera)",
                    cx, cy, cz, maxDeltaAcross, maxDeltaFrame);
                maxDeltaAcross = 0.0f;
                maxDeltaFrame = 0.0f;
            }
        }
    }

    for (int k = 0; k < 12; ++k) {
        g_s4LastWritten[k] = out[k];
    }
    InterlockedExchange(&g_s4HaveWritten, 1);
}

DWORD WINAPI Thread(LPVOID)
{
    // Clears g_threadAlive on every exit path, including the three early
    // returns below. See the flag's own comment.
    struct AliveGuard {
        AliveGuard()  { InterlockedExchange(&g_threadAlive, 1); }
        ~AliveGuard() { InterlockedExchange(&g_threadAlive, 0); }
    } aliveGuard;

    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE gamedll = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!engine || !gamedll) {
        LOG("boneprobe: engine/gamedll not mapped");
        return 0;
    }
    g_getCount = reinterpret_cast<GetCountFn>(
        GetProcAddress(engine, "?GetMeshElementsMatricesCount@IModelObject@@QEBAIXZ"));
    g_getName = reinterpret_cast<GetNameFn>(
        GetProcAddress(engine, "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z"));
    g_getParent = reinterpret_cast<GetParentFn>(
        GetProcAddress(engine, "?GetElementParent@IModelObject@@QEBAHH@Z"));
    g_getLocal = reinterpret_cast<GetLocalMtxFn>(
        GetProcAddress(engine, "?GetElementLocalMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_setLocal = reinterpret_cast<SetLocalMtxFn>(
        GetProcAddress(engine, "?SetElementLocalMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    g_blockOne = reinterpret_cast<BlockOneFn>(
        reinterpret_cast<uintptr_t>(engine) + 0x15C920);
    g_blockAnim = reinterpret_cast<BlockAnimFn>(GetProcAddress(engine,
        "?HackPABlender_BlockAnimation@IModelObject@@QEAAXAEBV?$vectorm@$0DK@_N@ttl@@@Z"));
    g_getFlags = reinterpret_cast<GetFlagsFn>(
        GetProcAddress(engine, "?GetFlags@IControlObject@@QEBA?AW4TYPE@COFlags@@XZ"));
    g_setFlag = reinterpret_cast<FlagFn>(
        GetProcAddress(engine, "?SetFlag@IControlObject@@QEAAXW4TYPE@COFlags@@@Z"));
    g_clearFlag = reinterpret_cast<FlagFn>(
        GetProcAddress(engine, "?ClearFlag@IControlObject@@QEAAXW4TYPE@COFlags@@@Z"));
    g_getWorld = reinterpret_cast<GetWorldMtxFn>(
        GetProcAddress(engine, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_setWorld = reinterpret_cast<SetWorldMtxFn>(
        GetProcAddress(engine, "?SetElementWorldMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    g_getAnimBind = reinterpret_cast<GetAnimBindFn>(
        GetProcAddress(engine, "?GetAnimBind@IModelObject@@QEAAPEAVIAnimBind@@XZ"));
    g_elemFromId = reinterpret_cast<GetElemFromBoneIdFn>(
        GetProcAddress(engine, "?GetMeshElemFromBoneID@IModelObject@@QEBAHE@Z"));
    if (!g_getCount || !g_getName || !g_getParent || !g_getLocal || !g_setLocal ||
        !g_elemFromId) {
        LOG("boneprobe: export resolve failed - count=%p name=%p parent=%p local=%p elemFromId=%p",
            reinterpret_cast<void*>(g_getCount), reinterpret_cast<void*>(g_getName),
            reinterpret_cast<void*>(g_getParent), reinterpret_cast<void*>(g_getLocal),
            reinterpret_cast<void*>(g_elemFromId));
        return 0;
    }
    LOG("boneprobe: exports resolved, waiting for diag_bone_dump");

    const uintptr_t addr = reinterpret_cast<uintptr_t>(gamedll) + 0xC62AC0;

    // >>> THE BOUND IS A SETTING, AND THE LOG QUOTES THE SAME VALUE IT USES. <<<
    // Read ONCE into a local: the loop runs for minutes and a value that changed
    // mid-loop would make the "attempt N/M" lines disagree with the give-up line,
    // which is exactly the kind of log that gets misread later.
    const int maxAttempts = (BoneDumpAttempts > 0) ? BoneDumpAttempts : 20;
    for (int attempt = 1; attempt <= maxAttempts && !g_done; ++attempt) {
        // Stage 1 needs the same capture Stage 0 performs (it is how we get the
        // model pointer at all), so either flag arms it.
        if (!DiagBoneDump && !DiagRigStage1 && !DiagRigStage2 && !DiagRigStage3 &&
            !DiagRigStage4) {
            Sleep(1000);
            attempt = 0;  // not an attempt if the feature is off
            continue;
        }
        // >>> WHICH EXEC CLIENT. Default is client 0 (DR1) - unchanged, and the
        // regression path. With rig_exec_client2=1 this probe moves to CLIENT 1
        // (DR0) so it can coexist with the melee trap on client 0, which is the
        // ONLY thing blocking VR-directed melee (proven by the coexistence run
        // on 2026-09-16: the trap logged `REFUSING TO ARM - the bone probe holds
        // the single process-wide WatchExec slot` every 5 s and neither worked).
        //
        // Client 1 has no poke map, no field capture and no stack-float capture.
        // This probe needs none of them - it sets StackFloatOffset(0) below and
        // reads only rcx/rdx - which is why it is the one that moves.
        const bool useClient2 = RigExecClient2;
        dlvr::WatchExecSetStackFloatOffset(0);
        dlvr::WatchExecSetStackUseRsp(false);
        const bool armed = useClient2 ? dlvr::WatchExec2Start(addr)
                                      : dlvr::WatchExecStart(addr);
        if (!armed) {
            LOG("boneprobe: attempt %d/%d - %s failed at gamedll+0xc62ac0. %s",
                attempt, maxAttempts,
                useClient2 ? "WatchExec2Start" : "WatchExecStart",
                useClient2 ? "Client 1 shares DR0 with the DATA watch - it refuses "
                             "by name if that holds it (see watchwrites: REFUSING)."
                           : "Is another diag_* exec watch enabled? Only one may "
                             "hold client 0.");
            Sleep(4000);
            continue;
        }
        Sleep(1500);
        if (useClient2) { dlvr::WatchExec2Stop(); } else { dlvr::WatchExecStop(); }

        dlvr::ExecHit hits[32] = {};
        const int n = useClient2 ? dlvr::WatchExec2Get(hits, 32)
                                 : dlvr::WatchExecGet(hits, 32);
        LOG("boneprobe: attempt %d/%d - CorrectItemPosition hits: %d distinct (entity,hand) pair(s)",
            attempt, maxAttempts, n);
        if (n == 0) {
            LOG("boneprobe:   no hits - is a weapon actually equipped? CorrectItemPosition "
                "only runs for a held item.");
            Sleep(4000);
            continue;
        }

        // INV-524 fix (A): in co-op the hits also carry the OTHER player's PlayerTppVis, and "the first skeleton" took
        // it at a sleep (both machines, 2026-10-10 and 2026-10-17). Under rig_player_live_guard, accept only the
        // skeleton that IS the game's local player object (INV-365 / INV-570: the captured PlayerDI is that object).
        // Read ONCE per attempt (the loop sleeps 1.5 s + 4 s). Chain unread or no object -> the old first-skeleton
        // rule, and this line says the test could not be made (INV-6).
        int pWhy = -1;
        const uintptr_t playerNow = RigPlayerLiveGuard ? RigGamePlayerObject(&pWhy) : 0;
        const bool localTest = RigPlayerLiveGuard && pWhy == 0 && playerNow != 0;
        int skippedRemote = 0;
        // [PROBE: BONEPROBE] gate=rig_player_live_guard  site=FLAT  log-only=no
        LOG("boneprobe:   local-player test: %s (the game's player object %p, chain %d: 0 = read)",
            localTest ? "ON - only a skeleton that IS the game's player is captured"
                      : (!RigPlayerLiveGuard ? "OFF (rig_player_live_guard=0) - first skeleton wins, as before"
                                             : "CANNOT BE MADE (chain unread or no player object) - first skeleton wins, as before"),
            reinterpret_cast<void*>(playerNow), pWhy);
        for (int i = 0; i < n && !g_done; ++i) {
            const uintptr_t entity = static_cast<uintptr_t>(hits[i].rcxValue);
            const uintptr_t hand = static_cast<uintptr_t>(hits[i].rdxValue);
            char entName[128] = {};
            const bool gotEnt = dlvr::RttiClassNameOf(entity, entName, sizeof(entName));
            LOG("boneprobe:   hit %d: entity=%p hand=%llu class=%s",
                i, reinterpret_cast<void*>(entity), static_cast<unsigned long long>(hand),
                gotEnt ? entName : "<unresolved>");

            // CORRECTED 2026-09-07 after the first live run. The value at
            // entity+0x40 came back as 0x00007FF9AB6F53C8 - a MODULE address
            // range, not a heap one (the entity itself is 0x000001F5...), i.e.
            // it is a VTABLE POINTER. So +0x40 is an EMBEDDED SUB-OBJECT and
            // its address is `entity + 0x40`; dereferencing it was wrong.
            // ENGINE_ARCHITECTURE.md's "a virtual accessor on the object at
            // entityBase+0x40" means exactly that, and I read it as a pointer.
            //
            // Rather than swap one guessed offset for another, enumerate: walk
            // the entity's own bytes and RTTI-resolve every embedded sub-object
            // view (PlayerFppVis's sibling PlayerDI has 25 of them), trying
            // both interpretations at each offset. Log what is actually there,
            // then validate candidates by calling GetMeshElementsMatricesCount
            // and sanity-checking the result rather than trusting the name.
            LOG("boneprobe:     --- enumerating embedded sub-object views of this entity ---");
            void* model = nullptr;
            const char* modelLabel = nullptr;
            static char labelBuf[64];
            // Sweep the WHOLE range and log every RTTI-resolvable view, rather
            // than stopping at the first thing that passes - the first run
            // stopped on a false positive and produced no map at all.
            for (uintptr_t off = 0; off <= 0x1400; off += 8) {
                for (int mode = 0; mode < 2; ++mode) {
                    // mode 0 = object embedded AT entity+off
                    // mode 1 = pointer TO an object stored at entity+off
                    uintptr_t cand;
                    if (mode == 0) {
                        cand = entity + off;
                    } else {
                        cand = ProbeReadPtr(entity + off);
                        if (!cand || cand <= 0x10000) {
                            continue;
                        }
                    }
                    char nm[128] = {};
                    if (!dlvr::RttiClassNameOf(cand, nm, sizeof(nm))) {
                        continue;
                    }
                    unsigned int c = 0;
                    char first[96] = {};
                    const bool ok = ValidateSkeleton(reinterpret_cast<void*>(cand), c,
                                                     first, sizeof(first));
                    unsigned int rawCount = 0;
                    ProbeCount(reinterpret_cast<void*>(cand), rawCount);
                    LOG("boneprobe:       +0x%04zX (%s) class=%-40s elems=%-4u first=%-20s %s",
                        static_cast<size_t>(off), mode == 0 ? "embedded" : "pointer ", nm,
                        rawCount, first[0] ? first : "-",
                        ok ? "<-- SKELETON" : "");
                    if (ok && !model && localTest && cand != playerNow) {
                        ++skippedRemote;
                        // [PROBE: BONEPROBE] gate=rig_player_live_guard  site=FLAT  log-only=no
                        LOG("boneprobe:       ^ SKIPPED - not the game's player (co-op remote?): hit %d, entity %p "
                            "class %s, skeleton %p, the game's player %p",
                            i, reinterpret_cast<void*>(entity), gotEnt ? entName : "<unresolved>",
                            reinterpret_cast<void*>(cand), reinterpret_cast<void*>(playerNow));
                    } else if (ok && !model) {
                        model = reinterpret_cast<void*>(cand);
                        _snprintf_s(labelBuf, sizeof(labelBuf), _TRUNCATE,
                                    mode == 0 ? "entity+0x%zX (embedded)" : "*(entity+0x%zX)",
                                    static_cast<size_t>(off));
                        modelLabel = labelBuf;
                    }
                }
            }
            if (!model) {
                LOG("boneprobe:     %s",
                    skippedRemote ? "no skeleton on this entity is the game's player - not captured"
                                  : "no element-bearing sub-object found on this entity");
                continue;
            }
            if (DumpSkeleton(model, modelLabel)) {
                g_model  = model;
                g_entity = entity;
                InterlockedExchange(&g_done, 1);
                break;
            }
        }
        if (!g_done) {
            if (skippedRemote) {
                // [PROBE: BONEPROBE] gate=rig_player_live_guard  site=FLAT  log-only=no
                LOG("boneprobe:   attempt %d/%d - %d skeleton(s) skipped as not the game's player and none matched it - "
                    "NO capture this attempt, retrying",
                    attempt, maxAttempts, skippedRemote);
            }
            Sleep(4000);
        }
    }

    if (g_done) {
        LOG("boneprobe: DONE - skeleton dumped, probe disarmed. Set diag_bone_dump=0.");
    } else {
        LOG("boneprobe: gave up after %d attempts (~%d s from process start) without a "
            "usable skeleton. >>> THIS IS A WINDOW THAT CLOSED, NOT A VERDICT ON THE "
            "RIG. <<< The probe needs a HELD ITEM - CorrectItemPosition only runs for "
            "one - and the window is measured from PROCESS START, not from gameplay. "
            "If the level load outlasts it, nothing downstream can ever arm: no model, "
            "no [SHOULDER] latch, no rig. Raise bone_dump_attempts.",
            maxAttempts, maxAttempts * 6);
        return 0;
    }

    if (DiagRigStage3 && g_model) {
        LOG("rig3: ===== STAGE 3: bone-array numbering (read-only) =====");
        Stage3BoneArrayDump();
    }

    // ---- Stage 1 ----------------------------------------------------------
    // Runs at 2 Hz, not per-frame: this is a correctness probe, and a slow
    // cadence keeps the log readable while still sampling often enough for the
    // "did it change between passes" liveness check to mean something.
    if (DiagRigStage1 && g_model) {
        LOG("rig1: ===== STAGE 1: null round-trip on the right arm chain =====");
        LOG("rig1: model=%p  writing each bone's own local matrix straight back "
            "(unchanged, so a torn write is harmless)", g_model);
        LOG("rig1: live=YES means the bone is genuinely animating; roundtrip=OK means "
            "our mtx34 layout and calling convention are correct.");
        for (int i = 0; i < 120 && DiagRigStage1; ++i) {
            Stage1NullRoundTrip();
            Sleep(500);
        }
        LOG("rig1: ===== STAGE 1 finished (or disabled) =====");
    }
    return 0;
}

}  // namespace boneprobe

const void* BoneProbeCapturedModel() { return boneprobe::g_model; }
uintptr_t   BoneProbeCapturedEntity() { return boneprobe::g_entity; }

// Called from ovr2xr.cpp the moment the engine has this frame's head, which is
// the ONLY place the frame boundary is visible - the levelling hook runs
// entirely on the other side of it. Re-arms the once-per-frame yaw publish.
volatile LONG g_rpFrame = 0;   // [RENDPOSE]: frame boundaries seen, stamps ring entries / camera samples / submits

void CamHookNoteFrameBoundary()
{
    InterlockedExchange(&g_publishedThisFrame, 0);
    InterlockedIncrement(&g_rpFrame);
}

// ===== [EYEAXIS] - DOES THE STEREO BASELINE FOLLOW THE RENDERED VIEW? (INV-260, 2026-09-27) =====
//
// sub_1802E5530 (engine, render loop, hy17) copies camera a2 into both eye cameras - camhook
// rotates each COPY on the way through - and then places them itself:
//     eyeL.pos = a2.pos + helper[+148] * a2.col0     eyeR.pos = a2.pos + helper[+204] * a2.col0
// so the baseline lies along a2's right axis, not the eye's. Whether camhook ALSO rotates a2 is
// decided at runtime (auto mode levels whatever AutoShouldLevel accepts), so it is measured here
// at the RESULT: B = eyeR.pos - eyeL.pos against the left eye's own right axis.
//
// HEAD ROLL is the discriminator. The view's roll exists only because camhook's keep-head branch
// adds it. A baseline that follows the view rolls with the head (base-view slope ~0); one built on
// an un-rotated a2 stays level (base-view slope ~ -view slope). Yaw cannot separate them today -
// both cameras carry the engine's own look yaw - which is exactly why the decouple would expose it.
//
// Layout from the disassembly, not from camhook's settings: world matrix at +0x40, row-major 3x4,
// col0 = floats {0,4,8}, translation {3,7,11} (hy17: a2[16],[20],[24] / [19],[23],[27]).
// Eye cameras: helper = *(engine+0xA40290) (INV-257), L = *(helper+88), R = *(helper+96).
// Cost while ON: a handful of reads per frame, one log line per second. While OFF: one bool.
namespace {

bool EyeAxisReadEyes(uintptr_t helperSlot, uintptr_t& eyeL, uintptr_t& eyeR,
                     float mL[12], float mR[12], int& why)
{
    __try {
        const uintptr_t helper = *reinterpret_cast<const uintptr_t*>(helperSlot);
        if (!helper) { why = 1; return false; }
        eyeL = *reinterpret_cast<const uintptr_t*>(helper + 88);
        eyeR = *reinterpret_cast<const uintptr_t*>(helper + 96);
        if (!eyeL || !eyeR) { why = 2; return false; }
        memcpy(mL, reinterpret_cast<const void*>(eyeL + 0x40), 48);
        memcpy(mR, reinterpret_cast<const void*>(eyeR + 0x40), 48);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = 3;
        return false;
    }
}

struct EyeAxisAcc {
    double n = 0, sx = 0, sxx = 0;       // x = head roll, deg
    double sv = 0, sxv = 0;              // view roll
    double sb = 0, sxb = 0;              // baseline roll
    double sd = 0, sxd = 0;              // baseline - view roll
    double sLen = 0, sYawAbs = 0;
    double maxYawAbs = 0, maxDAbs = 0;
    double xMin = 1e9, xMax = -1e9;
    long   antiparallel = 0;
    void Add(double x, double v, double b, double yawDeg, double len, bool anti)
    {
        const double d = b - v;
        n += 1; sx += x; sxx += x * x;
        sv += v; sxv += x * v; sb += b; sxb += x * b; sd += d; sxd += x * d;
        sLen += len; sYawAbs += fabs(yawDeg);
        if (fabs(yawDeg) > maxYawAbs) maxYawAbs = fabs(yawDeg);
        if (fabs(d) > maxDAbs) maxDAbs = fabs(d);
        if (x < xMin) xMin = x;
        if (x > xMax) xMax = x;
        if (anti) ++antiparallel;
    }
    double Slope(double s, double sxs) const
    {
        const double den = n * sxx - sx * sx;
        return (fabs(den) > 1e-9) ? (n * sxs - sx * s) / den : 0.0;
    }
};

EyeAxisAcc g_eyeAxisWin, g_eyeAxisCum;
LONG       g_eyeAxisRejTorn = 0, g_eyeAxisRejRead[4] = {};
ULONGLONG  g_eyeAxisWinStart = 0;
bool       g_eyeAxisFirst = true;       // first ACCEPTED sample
bool       g_eyeAxisFirstTorn = true;   // first REJECTED-as-torn sample (separate: must not eat the above)
bool       g_eyeAxisSaidRefusal[4] = {};

// [PROBE: EYEAXIS] gate=diag_eye_axis  site=HEADSET  log-only=yes
void EyeAxisLogWindow(const char* label, const EyeAxisAcc& a)
{
    const double swept = (a.n > 0) ? (a.xMax - a.xMin) : 0.0;
    if (a.n < 2) {
        LOG("camhook: [EYEAXIS] %s n=%.0f - no samples (rejects: torn %ld, no-helper %ld, "
            "no-eye %ld, read-fault %ld)", label, a.n, g_eyeAxisRejTorn, g_eyeAxisRejRead[1],
            g_eyeAxisRejRead[2], g_eyeAxisRejRead[3]);
        return;
    }
    if (swept < 5.0) {
        LOG("camhook: [EYEAXIS] %s n=%.0f head roll swept %.1f deg (< 5) - NO SLOPES | "
            "|B| mean %.4f m | horiz angle B-vs-right mean %.2f max %.2f deg | max |base-view roll| "
            "%.2f deg | antiparallel %ld | torn %ld", label, a.n, swept, a.sLen / a.n,
            a.sYawAbs / a.n, a.maxYawAbs, a.maxDAbs, a.antiparallel, g_eyeAxisRejTorn);
        return;
    }
    LOG("camhook: [EYEAXIS] %s n=%.0f head roll %.1f..%.1f deg | SLOPE vs head roll: view %+.3f  "
        "baseline %+.3f  base-view %+.3f | max |base-view roll| %.2f deg | horiz angle B-vs-right "
        "mean %.2f max %.2f deg | |B| mean %.4f m | antiparallel %ld | torn %ld",
        label, a.n, a.xMin, a.xMax, a.Slope(a.sv, a.sxv), a.Slope(a.sb, a.sxb),
        a.Slope(a.sd, a.sxd), a.maxDAbs, a.sYawAbs / a.n, a.maxYawAbs, a.sLen / a.n,
        a.antiparallel, g_eyeAxisRejTorn);
}

}  // namespace

void CamHookEyeAxisTick()
{
    if (!DiagEyeAxis) {
        return;
    }
    // [PROBE: EYEAXIS] gate=diag_eye_axis  site=HEADSET  log-only=yes
    static uintptr_t s_helperSlot = 0;
    if (!s_helperSlot) {
        const HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
        if (!engine) {
            if (!g_eyeAxisSaidRefusal[0]) {
                g_eyeAxisSaidRefusal[0] = true;
                LOG("camhook: [EYEAXIS] REFUSED - engine_x64_rwdi.dll is not mapped; retrying each frame");
            }
            return;
        }
        s_helperSlot = reinterpret_cast<uintptr_t>(engine) + EngRva(0xA40290);
    }

    uintptr_t eyeL = 0, eyeR = 0;
    float mL[12], mR[12];
    int why = 0;
    if (!EyeAxisReadEyes(s_helperSlot, eyeL, eyeR, mL, mR, why)) {
        ++g_eyeAxisRejRead[why & 3];
        if (!g_eyeAxisSaidRefusal[why & 3]) {
            g_eyeAxisSaidRefusal[why & 3] = true;
            LOG("camhook: [EYEAXIS] REFUSED this frame - %s (counted; logged once per reason)",
                why == 1 ? "COculusHelper qword engine+0xA40290 is NULL"
                : why == 2 ? "eye camera pointer NULL at helper+88 (L) or helper+96 (R)"
                           : "reading the eye cameras faulted");
        }
        return;
    }

    // World up for the matrix is the axis camhook's YawOnly treats as up (cam_up_axis).
    const int up = (CamUpAxis() == 1) ? 2 : 1;
    const int h0 = 0, h1 = (up == 1) ? 2 : 1;

    float r[3] = { mL[0], mL[4], mL[8] };
    const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float B[3] = { mR[3] - mL[3], mR[7] - mL[7], mR[11] - mL[11] };
    const float bl = sqrtf(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);
    if (rl < 0.5f || rl > 1.5f || bl < 0.040f || bl > 0.090f) {
        // |B| outside 40..90 mm: the render thread was mid-way through placing the eyes (one eye
        // from this frame, one from the next), or the eyes are not yet set up. Counted, not used.
        InterlockedIncrement(&g_eyeAxisRejTorn);
        if (g_eyeAxisFirstTorn) {
            g_eyeAxisFirstTorn = false;
            LOG("camhook: [EYEAXIS] FIRST TORN SAMPLE REJECTED - |right|=%.4f |B|=%.4f m (outside 0.040..0.090) "
                "eyeL=%p eyeR=%p RttiMatrixOffset=0x%X - the probe keeps sampling",
                rl, bl, reinterpret_cast<void*>(eyeL), reinterpret_cast<void*>(eyeR),
                static_cast<unsigned>(RttiMatrixOffset()));
        }
        return;
    }
    for (int i = 0; i < 3; ++i) { r[i] /= rl; B[i] /= bl; }
    const bool anti = (r[0] * B[0] + r[1] * B[1] + r[2] * B[2]) < 0.0f;
    if (anti) { B[0] = -B[0]; B[1] = -B[1]; B[2] = -B[2]; }

    const double kDeg = 57.29577951;
    const double viewRoll = asin(fmax(-1.0, fmin(1.0, r[up]))) * kDeg;
    const double baseRoll = asin(fmax(-1.0, fmin(1.0, B[up]))) * kDeg;
    // Horizontal angle between B and the right axis, signed, in the ground plane.
    const double yawDeg = atan2(r[h0] * B[h1] - r[h1] * B[h0], r[h0] * B[h0] + r[h1] * B[h1]) * kDeg;

    // Head roll from the head camhook applies (LastReportedHead, OpenXR Y-up): the elevation of
    // the head's right axis (1,0,0) rotated by q.
    const XrQuaternionf& q = LastReportedHead();
    const double hry = 2.0 * (q.x * q.y + q.z * q.w);
    const double headRoll = asin(fmax(-1.0, fmin(1.0, hry))) * kDeg;

    if (g_eyeAxisFirst) {
        g_eyeAxisFirst = false;
        bool inL = false, inR = false;
        for (LONG i = 0; i < g_yawCandCount && i < kMaxYawCandidates; ++i) {
            inL |= (g_yawCand[i].obj == eyeL);
            inR |= (g_yawCand[i].obj == eyeR);
        }
        LOG("camhook: [EYEAXIS] FIRST SAMPLE eyeL=%p eyeR=%p (in camhook's levelled set: L=%d R=%d, "
            "%ld candidates) RttiMatrixOffset=0x%X (probe reads +0x40) up=%d | |B|=%.4f m "
            "headRoll=%.2f viewRoll=%.2f baseRoll=%.2f horiz=%.2f deg antiparallel=%d",
            reinterpret_cast<void*>(eyeL), reinterpret_cast<void*>(eyeR), inL ? 1 : 0, inR ? 1 : 0,
            static_cast<long>(g_yawCandCount), static_cast<unsigned>(RttiMatrixOffset()), up, bl,
            headRoll, viewRoll, baseRoll, yawDeg, anti ? 1 : 0);
    }

    g_eyeAxisWin.Add(headRoll, viewRoll, baseRoll, yawDeg, bl, anti);
    g_eyeAxisCum.Add(headRoll, viewRoll, baseRoll, yawDeg, bl, anti);

    const ULONGLONG now = GetTickCount64();
    if (g_eyeAxisWinStart == 0) {
        g_eyeAxisWinStart = now;
    }
    if (now - g_eyeAxisWinStart >= 1000) {
        EyeAxisLogWindow("WINDOW    ", g_eyeAxisWin);
        EyeAxisLogWindow("CUMULATIVE", g_eyeAxisCum);
        g_eyeAxisWin = EyeAxisAcc();
        g_eyeAxisWinStart = now;
    }
}

// ===== [VIEWPOS] - DOES THE VIEW TRACK THE HEAD IN TIME, AND DOES THE CAMERA MOVE WITH THE TRACKED HEAD? (2026-09-27) =====
//
// Design D, run 2: the per-window slopes pass (view yaw +1.000) yet Jay: "something still feels wrong whilst turning my
// head". Two candidates a 1 s slope cannot see (INV-265 + addendum):
//  (1) TIMING: under D the view's YAW reaches the render through the engine (+4460, written in the player update, built into
//      the camera by CameraFPPDI), while its PITCH/ROLL are applied by camhook from the reported head. If they arrive from
//      different frames the world swims on combined motion. Measured as a lag: cross-correlate per-frame CHANGES of the
//      real head against the view, for lags 0..8 frames, yaw and pitch separately.
//  (2) POSITION: the camera's world position against the TRACKED head position, the latter mapped into the game with the
//      rig's own XR->game yaw (`rel`, the rotation D5's aim uses: x' = x cos + z sin, z' = -x sin + z cos, y through). Correct
//      = gain ~1, small residual, ~0 heading error. A double-rotated head translation shows as a heading error ~ the head yaw.
//      Also the largest single-frame camera jump the head did NOT cause (a weapon-switch shift would show here).
// Sampled at the frame boundary: s->headPose is still the pose declared for the frame just submitted, and camhook's
// published view is from rendering that frame. Only frames with BOTH sticks centred (|x|,|y| <= 0.2) and the same anchor.
// Defined further down, next to CamHookGetOpenXrToGameYaw, where LevelCamera's anchor block publishes them.
extern volatile float     g_viewYaw;
extern volatile LONG      g_haveViewYaw;
extern volatile float     g_viewPitchApplied;
extern volatile float     g_viewPos[3];
extern volatile uintptr_t g_viewAnchorObj;
bool SafeReadLocal(uintptr_t addr, void* out, size_t bytes);   // defined further down
namespace {

constexpr int kVpMax = 512;
struct VpSample {
    double t;
    float  hy, hp, vy, vp, rel;
    float  T[3], C[3];
    bool   gated;
    uintptr_t anchor;
    // STAB (INV-306 / addendum 2), body frame, metres: L = the lagged copy of HSPINE1 (+16724), O = the stabiliser output
    // added to the head position in C08100 (+16736), P = HSPINE1's body-frame position as C07C70 read it, DERIVED exactly
    // from its own formula O = L - (p - R + S) with R (+17448) and S (+17484) read beside it; w (+16748); dz = DI+4464.
    bool   st;
    float  L[3], O[3], P[3], w, dz;
};
VpSample   g_vp[kVpMax];
int        g_vpN = 0;
ULONGLONG  g_vpWinStart = 0;
bool       g_vpFirst = true;
bool       g_vpSaid[3] = {};
long long  g_vpSticks = 0, g_vpNoAnchor = 0, g_vpNoStick = 0;
long long  g_vpStNoVis = 0, g_vpStReadFail = 0;
bool       g_vpStSaid = false;

float VpWrap(float a)
{
    while (a >  3.14159265f) { a -= 6.28318531f; }
    while (a < -3.14159265f) { a += 6.28318531f; }
    return a;
}

// Per-frame changes of the head (dh) and the view (dv) for one channel, only across consecutive gated samples with the
// same anchor (ok). Two components; the angle and vertical channels leave [1] at 0.
//   kVpYaw / kVpPitch: head vs view angle (rad).
//   kVpPosH / kVpPosV: the tracked head position mapped into the game (x' = x cos + z sin, z' = -x sin + z cos with the
//   CURRENT sample's rel, y through) vs the camera's world position (m) - horizontal (x,z) or vertical (y).
enum { kVpYaw = 0, kVpPitch, kVpPosH, kVpPosV };
float g_vpDh[kVpMax][2], g_vpDv[kVpMax][2];
bool  g_vpOk[kVpMax];

void VpDeltas(int ch)
{
    for (int i = 0; i < g_vpN; ++i) {
        g_vpOk[i] = i > 0 && g_vp[i].gated && g_vp[i - 1].gated && g_vp[i].anchor == g_vp[i - 1].anchor;
        if (!g_vpOk[i]) { continue; }
        const VpSample& a = g_vp[i - 1];
        const VpSample& b = g_vp[i];
        float* dh = g_vpDh[i];
        float* dv = g_vpDv[i];
        dh[1] = dv[1] = 0.0f;
        if (ch == kVpYaw) {
            dh[0] = VpWrap(b.hy - a.hy); dv[0] = VpWrap(b.vy - a.vy);
        } else if (ch == kVpPitch) {
            dh[0] = b.hp - a.hp; dv[0] = b.vp - a.vp;
        } else {
            const float cr = cosf(b.rel), sr = sinf(b.rel);
            const float dT[3] = { b.T[0] - a.T[0], b.T[1] - a.T[1], b.T[2] - a.T[2] };
            if (ch == kVpPosH) {
                dh[0] = dT[0] * cr + dT[2] * sr; dh[1] = -dT[0] * sr + dT[2] * cr;
                dv[0] = b.C[0] - a.C[0];          dv[1] = b.C[2] - a.C[2];
            } else {
                dh[0] = dT[1]; dv[0] = b.C[1] - a.C[1];
            }
        }
    }
}

// Best lag (frames) maximising |normalised correlation| of the per-frame changes VpDeltas(ch) produced.
// Returns false when there is too little motion.
bool VpLag(int ch, int& bestK, float& bestC, float& c0)
{
    VpDeltas(ch);
    bestK = -1; bestC = 0.0f; c0 = 0.0f;
    for (int k = 0; k <= 8; ++k) {
        double sxy = 0, sxx = 0, syy = 0;
        int n = 0;
        for (int i = k; i < g_vpN; ++i) {
            if (!g_vpOk[i] || !g_vpOk[i - k]) { continue; }
            const float* dv = g_vpDv[i];
            const float* dh = g_vpDh[i - k];
            sxy += dv[0] * dh[0] + dv[1] * dh[1];
            sxx += dv[0] * dv[0] + dv[1] * dv[1];
            syy += dh[0] * dh[0] + dh[1] * dh[1];
            ++n;
        }
        if (n < 10 || sxx <= 1e-12 || syy <= 1e-12) { continue; }
        const float c = static_cast<float>(sxy / sqrt(sxx * syy));
        if (k == 0) { c0 = c; }
        if (bestK < 0 || fabsf(c) > fabsf(bestC)) { bestK = k; bestC = c; }
    }
    return bestK >= 0;
}

// Position, one channel (kVpPosH / kVpPosV), HEAD-MOTION FIDELITY step (1), 2026-09-30:
//  (a) how many frames late the camera's position follows the tracked head - VpLag on position changes, attempted when the
//      head's own path in the window is >= 3 mm (no 20 mm gate: breathing / talking scale is the question);
//  (b) how much the camera moves that the head did NOT - at the lag found (lag 0 when none, said in the line): per-frame
//      e_i = view step_i - head step_(i-k). "path" = sum |e| (includes per-frame noise, grows with frame count); "drift rms" =
//      rms of the running sum of e, restarted at every gap in the gated run (the displacement the head did not make).
void VpPosText(int ch, float dtMs, char* out, size_t outSize)
{
    const char* name = (ch == kVpPosH) ? "POS-H" : "POS-V";
    VpDeltas(ch);
    double headPath = 0.0;
    for (int i = 0; i < g_vpN; ++i) {
        if (g_vpOk[i]) { headPath += sqrt(g_vpDh[i][0] * g_vpDh[i][0] + g_vpDh[i][1] * g_vpDh[i][1]); }
    }
    char lagTxt[96];
    int k = 0;
    float cb = 0.0f, c0 = 0.0f;
    if (headPath * 1000.0 < 3.0) {
        strcpy_s(lagTxt, "head path < 3 mm - no lag, lag 0 used");
    } else if (VpLag(ch, k, cb, c0)) {
        _snprintf_s(lagTxt, sizeof(lagTxt), _TRUNCATE, "lag %d fr (%.1f ms) corr %+.3f (lag-0 %+.3f)", k, k * dtMs, cb, c0);
    } else {
        k = 0;
        strcpy_s(lagTxt, "too few consecutive gated frames / no view motion - no lag, lag 0 used");
    }
    double viewPath = 0.0, extraPath = 0.0, driftSq = 0.0;
    float run[2] = { 0.0f, 0.0f };
    int steps = 0;
    bool inRun = false;
    for (int i = 0; i < g_vpN; ++i) {
        if (!g_vpOk[i] || i - k < 0 || !g_vpOk[i - k]) { inRun = false; continue; }
        if (!inRun) { run[0] = run[1] = 0.0f; inRun = true; }
        const float e[2] = { g_vpDv[i][0] - g_vpDh[i - k][0], g_vpDv[i][1] - g_vpDh[i - k][1] };
        viewPath  += sqrt(g_vpDv[i][0] * g_vpDv[i][0] + g_vpDv[i][1] * g_vpDv[i][1]);
        extraPath += sqrt(e[0] * e[0] + e[1] * e[1]);
        run[0] += e[0]; run[1] += e[1];
        driftSq += run[0] * run[0] + run[1] * run[1];
        ++steps;
    }
    const double secs = steps * dtMs / 1000.0;
    _snprintf_s(out, outSize, _TRUNCATE,
                "%s %s | head path %.1f mm, view path %.1f mm, not-from-head path %.1f mm (%.1f mm/s), not-from-head drift rms "
                "%.1f mm, %d steps", name, lagTxt, 1000.0 * headPath, 1000.0 * viewPath, 1000.0 * extraPath,
                secs > 0.0 ? 1000.0 * extraPath / secs : 0.0, steps ? 1000.0 * sqrt(driftSq / steps) : 0.0, steps);
}

// STAB, per window (INV-306 addendum 2, INV-308 attribution, 2026-09-30): which stabiliser term moves with the head.
//  - path (mm) of the head (in C08100's body-frame form: the store's (-x, y, -z)), L, O and p over consecutive gated STAB samples;
//  - p's per-frame change against the head's at lags 0..3: p carrying the head (|corr| high at lag >= 1, p path ~ head path) =
//    HSPINE1 still held the previous C08100 write when C07C70 read it (feedback); |corr| ~ 0 = p is the animation's own;
//  - w and DI+4464 ranges (the dead zone is 0.1*w m at DI+4464 >= 0, 0 at <= -45), max |L - p|.
void VpStabText(char* out, size_t outSize)
{
    int nSt = 0;
    float wMin = 1e9f, wMax = -1e9f, dzMin = 1e9f, dzMax = -1e9f, maxLP = 0.0f;
    for (int i = 0; i < g_vpN; ++i) {
        const VpSample& s = g_vp[i];
        if (!s.st) { continue; }
        ++nSt;
        wMin = fminf(wMin, s.w); wMax = fmaxf(wMax, s.w);
        dzMin = fminf(dzMin, s.dz); dzMax = fmaxf(dzMax, s.dz);
        const float d[3] = { s.L[0] - s.P[0], s.L[1] - s.P[1], s.L[2] - s.P[2] };
        maxLP = fmaxf(maxLP, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    if (nSt == 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "STAB: NO SAMPLES - no-vis %lld (the BFF080 latch is empty: detour not armed or "
                    "slot 342 not called yet), read-fail %lld", g_vpStNoVis, g_vpStReadFail);
        return;
    }
    static float dH[kVpMax][3], dP[kVpMax][3];
    static bool  ok[kVpMax];
    double pathH = 0, pathL = 0, pathO = 0, pathP = 0;
    for (int i = 0; i < g_vpN; ++i) {
        ok[i] = i > 0 && g_vp[i].st && g_vp[i - 1].st && g_vp[i].gated && g_vp[i - 1].gated;
        if (!ok[i]) { continue; }
        const VpSample& a = g_vp[i - 1];
        const VpSample& b = g_vp[i];
        dH[i][0] = -(b.T[0] - a.T[0]); dH[i][1] = b.T[1] - a.T[1]; dH[i][2] = -(b.T[2] - a.T[2]);
        float dL[3], dO[3];
        for (int q = 0; q < 3; ++q) { dP[i][q] = b.P[q] - a.P[q]; dL[q] = b.L[q] - a.L[q]; dO[q] = b.O[q] - a.O[q]; }
        pathH += sqrt(dH[i][0] * dH[i][0] + dH[i][1] * dH[i][1] + dH[i][2] * dH[i][2]);
        pathP += sqrt(dP[i][0] * dP[i][0] + dP[i][1] * dP[i][1] + dP[i][2] * dP[i][2]);
        pathL += sqrt(dL[0] * dL[0] + dL[1] * dL[1] + dL[2] * dL[2]);
        pathO += sqrt(dO[0] * dO[0] + dO[1] * dO[1] + dO[2] * dO[2]);
    }
    float c[4] = { 0, 0, 0, 0 };
    int bestK = -1, nPair = 0;
    for (int k = 0; k <= 3; ++k) {
        double sxy = 0, sxx = 0, syy = 0;
        int n = 0;
        for (int i = k; i < g_vpN; ++i) {
            if (!ok[i] || !ok[i - k]) { continue; }
            for (int q = 0; q < 3; ++q) {
                sxy += dP[i][q] * dH[i - k][q]; sxx += dP[i][q] * dP[i][q]; syy += dH[i - k][q] * dH[i - k][q];
            }
            ++n;
        }
        if (n >= 10 && sxx > 1e-14 && syy > 1e-14) {
            c[k] = static_cast<float>(sxy / sqrt(sxx * syy));
            if (bestK < 0 || fabsf(c[k]) > fabsf(c[bestK])) { bestK = k; }
        }
        if (k == 0) { nPair = n; }
    }
    char corrTxt[128];
    if (bestK < 0) {
        _snprintf_s(corrTxt, sizeof(corrTxt), _TRUNCATE, "p vs head: NO CORR (%d pairs, or no p / head motion)", nPair);
    } else {
        _snprintf_s(corrTxt, sizeof(corrTxt), _TRUNCATE, "p vs head: best lag %d corr %+.2f (lags 0..3: %+.2f %+.2f %+.2f %+.2f)",
                    bestK, c[bestK], c[0], c[1], c[2], c[3]);
    }
    _snprintf_s(out, outSize, _TRUNCATE,
                "STAB n %d (no-vis %lld, read-fail %lld) w %.2f..%.2f DI+4464 %.1f..%.1f | path mm: head %.1f L %.1f O %.1f p %.1f "
                "| %s | max |L-p| %.1f mm", nSt, g_vpStNoVis, g_vpStReadFail, wMin, wMax, dzMin, dzMax, 1000.0 * pathH,
                1000.0 * pathL, 1000.0 * pathO, 1000.0 * pathP, corrTxt, 1000.0f * maxLP);
}

void VpReport()
{
    int gated = 0;
    float hyMin = 1e9f, hyMax = -1e9f, hpMin = 1e9f, hpMax = -1e9f;
    double dtSum = 0;
    int dtN = 0;
    for (int i = 0; i < g_vpN; ++i) {
        if (i > 0) { dtSum += g_vp[i].t - g_vp[i - 1].t; ++dtN; }
        if (!g_vp[i].gated) { continue; }
        ++gated;
        hyMin = fminf(hyMin, g_vp[i].hy); hyMax = fmaxf(hyMax, g_vp[i].hy);
        hpMin = fminf(hpMin, g_vp[i].hp); hpMax = fmaxf(hpMax, g_vp[i].hp);
    }
    const double dtMs = dtN ? 1000.0 * dtSum / dtN : 0.0;
    const float kDeg = 57.2957795f;
    char yawTxt[160] = "yaw: head swept < 5 deg - no lag", pitTxt[160] = "pitch: head swept < 5 deg - no lag";
    int k; float cb, c0;
    if (gated > 0 && (hyMax - hyMin) * kDeg >= 5.0f) {
        if (VpLag(kVpYaw, k, cb, c0)) {
            _snprintf_s(yawTxt, sizeof(yawTxt), _TRUNCATE, "YAW lag %d fr (%.1f ms) corr %+.3f (lag-0 %+.3f)",
                        k, k * dtMs, cb, c0);
        } else {
            strcpy_s(yawTxt, "yaw: too few consecutive gated frames");
        }
    }
    if (gated > 0 && (hpMax - hpMin) * kDeg >= 5.0f) {
        if (VpLag(kVpPitch, k, cb, c0)) {
            _snprintf_s(pitTxt, sizeof(pitTxt), _TRUNCATE, "PITCH lag %d fr (%.1f ms) corr %+.3f (lag-0 %+.3f)",
                        k, k * dtMs, cb, c0);
        } else {
            strcpy_s(pitTxt, "pitch: too few consecutive gated frames");
        }
    }
    char posHTxt[320], posVTxt[320];
    VpPosText(kVpPosH, static_cast<float>(dtMs), posHTxt, sizeof(posHTxt));
    VpPosText(kVpPosV, static_cast<float>(dtMs), posVTxt, sizeof(posVTxt));
    char stabTxt[400];
    VpStabText(stabTxt, sizeof(stabTxt));
    // Position, relative to the first gated sample of the window with the same anchor.
    int ref = -1;
    double see = 0, sgg = 0, scc = 0, scg = 0;
    int pn = 0, anchorChanges = 0;
    float maxJump = 0.0f, endAngErr = 0.0f;
    bool haveAng = false;
    float gPrev[3] = { 0, 0, 0 }, cPrev[3] = { 0, 0, 0 };
    bool havePrev = false;
    for (int i = 0; i < g_vpN; ++i) {
        const VpSample& s = g_vp[i];
        if (!s.gated) { havePrev = false; continue; }
        if (ref < 0 || s.anchor != g_vp[ref].anchor) {
            if (ref >= 0) { ++anchorChanges; }
            ref = i; havePrev = false;
        }
        const VpSample& r = g_vp[ref];
        const float dT[3] = { s.T[0] - r.T[0], s.T[1] - r.T[1], s.T[2] - r.T[2] };
        const float cr = cosf(s.rel), sr = sinf(s.rel);
        const float g[3] = { dT[0] * cr + dT[2] * sr, dT[1], -dT[0] * sr + dT[2] * cr };
        const float c[3] = { s.C[0] - r.C[0], s.C[1] - r.C[1], s.C[2] - r.C[2] };
        const float e[3] = { c[0] - g[0], c[1] - g[1], c[2] - g[2] };
        see += e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        sgg += g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
        scc += c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        scg += c[0] * g[0] + c[1] * g[1] + c[2] * g[2];
        ++pn;
        if (havePrev) {
            const float j[3] = { (c[0] - cPrev[0]) - (g[0] - gPrev[0]), (c[1] - cPrev[1]) - (g[1] - gPrev[1]),
                                 (c[2] - cPrev[2]) - (g[2] - gPrev[2]) };
            maxJump = fmaxf(maxJump, sqrtf(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]));
        }
        for (int q = 0; q < 3; ++q) { gPrev[q] = g[q]; cPrev[q] = c[q]; }
        havePrev = true;
        if (sqrtf(g[0] * g[0] + g[2] * g[2]) > 0.01f && sqrtf(c[0] * c[0] + c[2] * c[2]) > 0.005f) {
            endAngErr = VpWrap(atan2f(c[0], c[2]) - atan2f(g[0], g[2])) * kDeg;
            haveAng = true;
        }
    }
    // [PROBE: VIEWPOS] gate=diag_view_pos  site=HEADSET  log-only=yes
    LOG("camhook: [VIEWPOS] window n %d gated %d (sticks-moving %lld, no-anchor %lld, no-stick-input %lld) frame %.1f ms | head yaw swept %.1f "
        "pitch swept %.1f deg | %s | %s | POS (%d samples): rms head-mapped %.1f mm, camera %.1f mm, residual %.1f mm, "
        "gain %.3f, heading err %s%.1f deg, max 1-frame jump not from the head %.1f mm, anchor changes %d | %s | %s | %s",
        g_vpN, gated, g_vpSticks, g_vpNoAnchor, g_vpNoStick, dtMs, (gated ? (hyMax - hyMin) * kDeg : 0.0f),
        (gated ? (hpMax - hpMin) * kDeg : 0.0f), yawTxt, pitTxt, pn,
        pn ? 1000.0 * sqrt(sgg / pn) : 0.0, pn ? 1000.0 * sqrt(scc / pn) : 0.0, pn ? 1000.0 * sqrt(see / pn) : 0.0,
        sgg > 1e-12 ? scg / sgg : 0.0, haveAng ? "" : "n/a ", haveAng ? endAngErr : 0.0f, 1000.0f * maxJump,
        anchorChanges, posHTxt, posVTxt, stabTxt);
}

}  // namespace

void CamHookViewPosTick()
{
    if (!DiagViewPos) {
        return;
    }
    // [PROBE: VIEWPOS] gate=diag_view_pos  site=HEADSET  log-only=yes
    XrPosef head;
    if (!GetHeadPose(head)) {
        if (!g_vpSaid[0]) {
            g_vpSaid[0] = true;
            LOG("camhook: [VIEWPOS] REFUSED this frame - no head pose located yet (GetHeadPose false); counted by window");
        }
        return;
    }
    float rel = 0.0f;
    if (!g_haveViewYaw || !CamHookGetOpenXrToGameYaw(rel)) {
        ++g_vpNoAnchor;
        if (!g_vpSaid[1]) {
            g_vpSaid[1] = true;
            LOG("camhook: [VIEWPOS] REFUSED this frame - camhook has not published a view yaw / XR->game yaw yet (no yaw "
                "anchor chosen: needs cam_auto, horizon_lock, cam_keep_head and a levelled camera)");
        }
        return;
    }
    const XrQuaternionf& q = head.orientation;
    const float hfx = -2.0f * (q.x * q.z + q.y * q.w);
    const float hfz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    VpSample s;
    LARGE_INTEGER qpc, qpf;
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&qpf);
    s.t = static_cast<double>(qpc.QuadPart) / static_cast<double>(qpf.QuadPart);
    s.hy = atan2f(hfx, hfz);
    s.hp = asinf(fmaxf(-1.0f, fminf(1.0f, sinp)));
    s.vy = g_viewYaw;
    s.vp = g_viewPitchApplied;
    s.rel = rel;
    s.T[0] = head.position.x; s.T[1] = head.position.y; s.T[2] = head.position.z;
    s.C[0] = g_viewPos[0]; s.C[1] = g_viewPos[1]; s.C[2] = g_viewPos[2];
    s.anchor = g_viewAnchorObj;
    // A stick whose action is INACTIVE (controller asleep / off / flat battery) cannot be moving the player, so it counts as
    // centred - counted separately as no-stick-input, never as sticks-moving (run 1, 2026-09-30: both controllers idled off
    // at the start of the still block and the old gate refused every frame of the protocol as "sticks-moving").
    float lx = 0, ly = 0, rx = 0, ry = 0;
    const bool haveL = GetControllerStick(kLeftHand, lx, ly);
    const bool haveR = GetControllerStick(kRightHand, rx, ry);
    if (!haveL || !haveR) {
        ++g_vpNoStick;
        if (!g_vpSaid[2]) {
            g_vpSaid[2] = true;
            LOG("camhook: [VIEWPOS] no stick input (left %s, right %s) - that stick counts as CENTRED; counted per window as "
                "no-stick-input", haveL ? "active" : "INACTIVE", haveR ? "active" : "INACTIVE");
        }
    }
    s.gated = (!haveL || (fabsf(lx) <= 0.2f && fabsf(ly) <= 0.2f)) && (!haveR || (fabsf(rx) <= 0.2f && fabsf(ry) <= 0.2f));
    if (!s.gated) { ++g_vpSticks; }

    // STAB: the head stabiliser through the BFF080-latched PlayerFppVis (boneblend). Read at THIS frame boundary, not at the
    // game's update - a torn read across a game update is possible and counts as data (floats only, SEH-guarded).
    s.st = false;
    s.w = s.dz = 0.0f;
    for (int q = 0; q < 3; ++q) { s.L[q] = s.O[q] = s.P[q] = 0.0f; }
    const uintptr_t vis = BonesAnimPlayerVis();
    if (!vis) {
        ++g_vpStNoVis;
    } else {
        float R[3] = { 0, 0, 0 }, S[3] = { 0, 0, 0 };
        uintptr_t di = 0;
        const bool ok = SafeReadLocal(vis + 16724, s.L, 12) && SafeReadLocal(vis + 16736, s.O, 12) &&
                        SafeReadLocal(vis + 16748, &s.w, 4) && SafeReadLocal(vis + 17448, R, 12) &&
                        SafeReadLocal(vis + 17484, S, 12) && SafeReadLocal(vis + 80, &di, 8) && di &&
                        SafeReadLocal(di + 4464, &s.dz, 4);
        if (!ok) {
            ++g_vpStReadFail;
        } else {
            for (int q = 0; q < 3; ++q) { s.P[q] = s.L[q] + R[q] - S[q] - s.O[q]; }
            s.st = true;
        }
        if (!g_vpStSaid) {
            g_vpStSaid = true;
            // [PROBE: VIEWPOS] gate=diag_view_pos  site=HEADSET  log-only=yes
            LOG("camhook: [VIEWPOS] STAB FIRST READ vis %p %s | L (%.4f %.4f %.4f) O (%.4f %.4f %.4f) p (%.4f %.4f %.4f) w %.3f "
                "DI+4464 %.2f | R (%.4f %.4f %.4f) S (%.4f %.4f %.4f). Body frame, metres; p derived from O = L - (p - R + S).",
                reinterpret_cast<void*>(vis), ok ? "READ" : "*** READ FAILED ***", s.L[0], s.L[1], s.L[2], s.O[0], s.O[1],
                s.O[2], s.P[0], s.P[1], s.P[2], s.w, s.dz, R[0], R[1], R[2], S[0], S[1], S[2]);
        }
    }

    if (g_vpFirst) {
        g_vpFirst = false;
        LOG("camhook: [VIEWPOS] FIRST SAMPLE head yaw %+.1f pitch %+.1f deg pos (%.3f %.3f %.3f) | view yaw %+.1f pitch "
            "(applied) %+.1f deg camera pos (%.3f %.3f %.3f) anchor %p | XR->game yaw %+.1f deg | sticks centred %d | "
            "decouple %d. One line per second.",
            s.hy * 57.2958f, s.hp * 57.2958f, s.T[0], s.T[1], s.T[2], s.vy * 57.2958f, s.vp * 57.2958f,
            s.C[0], s.C[1], s.C[2], reinterpret_cast<void*>(s.anchor), rel * 57.2958f, s.gated ? 1 : 0,
            HeadYawDecoupleActive() ? 1 : 0);
    }
    if (g_vpN < kVpMax) {
        g_vp[g_vpN++] = s;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_vpWinStart == 0) { g_vpWinStart = now; }
    if (now - g_vpWinStart >= 1000) {
        VpReport();
        g_vpN = 0;
        g_vpSticks = 0;
        g_vpNoStick = 0;
        g_vpStNoVis = 0;
        g_vpStReadFail = 0;
        g_vpNoAnchor = 0;
        g_vpWinStart = now;
    }
}

// ===== [RENDPOSE] - SPEC-RENDERED-ORIENTATION-2026-09-30 STAGE 1 (R1 record, R2 pair, R4 instrument). LOG-ONLY. =====
//
// Under design D every rendered view's yaw = the engine's stick-only look + a head yaw WE added into +4460 (camyaw::Hook,
// boneblend.cpp). The spec's fix declares, per submitted frame, the orientation that frame was RENDERED with, so the
// compositor removes INV-266's 2-frame yaw lag. This stage only measures whether that pairing can be made and what it
// would correct; nothing here is declared.
//  R1  camyaw::Hook -> CamHookRendPosePush: {+4460 after the add (deg), the yaw added h (deg), the stripped-yaw quaternion,
//      frame stamp} into an 8-entry ring. Game thread, one writer, seqlock.
//  R2  LevelCamera's yaw-anchor block -> RendPoseCapture: the ENGINE camera's yaw (camhook adds no yaw, D-134), the
//      reported head camhook took pitch/roll from (yaw-free under D), and a snapshot of the ring.
//  R4  submit -> CamHookRendPoseSubmit: the latest capture beside the pose we DECLARE (eye 0, before PitchPose). Once per
//      second the +4460 -> camera-yaw mapping is FITTED - sign s, ring position k (which update's +4460 the camera
//      carries) and offset c - because it is [UNKNOWN] (spec R2). Each frame is then paired BY VALUE against every ring
//      entry with that s and c; the paired entry's stripped yaw gives the rendered XR orientation sy * (pitch/roll used),
//      compared with the declared one: the correction the compositor would make. "trail" > 0 = the image is BEHIND the
//      head in the direction of the turn.
// Cost while ON: one ring write per player update, one 8-entry copy per anchor camera, a few quaternion ops per submit,
// a <= 512 x 8 x 8 fit once a second. While OFF: one bool test at each of the three sites.
namespace {

constexpr int   kRpRing   = 8;
constexpr int   kRpMax    = 512;
constexpr float kRpTolDeg = 0.25f;   // a ring entry within this of the fitted mapping is a match
constexpr float kRpTurnDegS = 30.0f, kRpFastDegS = 100.0f;

struct RpEntry {
    float after;   // +4460 after the add, degrees (unwrapped, as the engine stores it)
    float h;       // the yaw added, degrees
    float sy[4];   // the stripped head-yaw quaternion (OpenXR, Y-up) h came from
    LONG  frame;   // g_rpFrame at the push
};
RpEntry         g_rpRing[kRpRing];
volatile LONG   g_rpRingSeq   = 0;   // odd = writing
volatile LONG64 g_rpRingCount = 0;
bool            g_rpPushSaid  = false;   // game thread only

struct RpCapture {
    float   cy;              // engine camera yaw (rad), g_viewYaw's formula on YawOnly(engine)
    float   pos[3];          // engine camera world position at the capture (cam[3], cam[7], cam[11]) - [AFWCAM]
    float   hq[4];           // LastReportedHead() camhook took pitch/roll from
    float   hp[3];           // LastReportedHeadPos() at the capture (LOCAL space, m) - [AFWCAM] step 2
    RpEntry ring[kRpRing];   // newest first
    int     n;
    bool    mm;              // taken while the main menu's root was being given the head's yaw (ui_panel_main_menu_turn)
    float   mmSy[4];         // ... and the stripped head-yaw quaternion it was given (valid when mm)
    LONG    frame;
    LONG64  seq;             // 1-based capture number ([RENDTAG])
    DWORD   tid;             // thread the capture ran on ([RENDTAG], INV-325 (a))
    LONGLONG qpc;            // QPC at the capture ([RENDTAG], age at the pose recorder)
};
// The last kRpCapHist captures (Stage 2): the image submitted is NOT always the latest capture - the game renders frame N
// from the camera built before the boundary that began it (the echo-age counter, D off: echo = previous located head in
// 591/588/567 of 600), and the next frame's camera can already be captured when the submit runs.
constexpr int   kRpCapHist   = 4;
RpCapture       g_rpCapHist[kRpCapHist];
volatile LONG   g_rpCapSeq   = 0;
volatile LONG64 g_rpCapCount = 0;
volatile LONG64 g_rpCapTorn  = 0;
bool            g_rpCapSaid  = false;   // capture thread only

struct RpSample {
    RpCapture cap;
    float     dq[4];        // declared eye-0 orientation, before PitchPose
    float     eyeYawDiff;   // |yaw(eye 0) - yaw(eye 1)|, deg (canted displays would show here)
    double    t;
    LONG      frame;        // g_rpFrame at the submit
    int       newCaps;      // captures since the previous submit
    int       echoPos;      // which capture (0 = latest) has hq == the game's RenderPose echo; -1 = none (cap = latest)
    int       echoN;        // how many captures matched
    bool      declared;     // Stage 2 declared this frame's rendered orientation
};
RpSample  g_rpS[kRpMax];   // submit thread only
int       g_rpN = 0;
LONG64    g_rpLastCapCount = 0;
ULONGLONG g_rpWinStart = 0;
long long g_rpSubmits = 0, g_rpRefused = 0, g_rpInvalid = 0, g_rpNoCap = 0, g_rpReadTorn = 0, g_rpRingEmpty = 0;
bool      g_rpSaid[4] = {};   // refused, no capture, ring empty, first sample

// ===== Stage 2 - DECLARE (declare_rendered_orientation), spec R3. =====
// Pairing (Stage 2b, INV-326): the submit's frameIndex equals its OWN pose record's (rd3d11 recorder detour, [RENDTAG]),
// and that record names the anchor capture made on its thread just before - the camera the image was drawn from. Stage 2
// paired on the game's RenderPose echo instead, which picked a different capture ~99% of the time (D-165, INV-326). Then
// the capture's ring entry at position 1 (INV-323: k = 1 in 120/120 windows), verified by value with INV-323's mapping
// (camera yaw = -(+4460) - 90.00 deg, residual <= kRdTolDeg). Anything else -> today's pose for that frame, counted by named
// reason. A wrong declaration looks like judder; the fallback is the current behaviour.
constexpr float kRdEchoTol = 1e-5f;   // per-component quaternion tolerance for "the same head"
constexpr float kRdTolDeg  = 0.10f;   // k = 1 value check against INV-323's mapping
long long g_rdCalls = 0, g_rdDeclared = 0, g_rdRefused = 0, g_rdNotReady = 0, g_rdNoEcho = 0, g_rdMulti = 0,
          g_rdShortRing = 0, g_rdK1Fail = 0, g_rdNaN = 0;
double    g_rdSumAbs = 0.0;
float     g_rdMaxAbs = 0.0f, g_rdMaxK1Res = 0.0f;
long long g_rdPos[kRpCapHist] = {};
long long g_rdAltK[kRpRing] = {};   // declared frames whose camera carried ring position j != 1 (found by value)
// For the VR Comfort tab's status line (CamHookRendDeclStatus): when the last frame was declared, and the last reason a
// frame was not. Written by the submit thread, read by the overlay - plain stores, a stale read only delays the text.
volatile ULONGLONG g_rdLastOkMs = 0;
const char* volatile g_rdLastWhy = "no frame submitted with it on yet";
ULONGLONG g_rdWinStart = 0;
bool      g_rdSaidFirst = false, g_rdPrevOn = false, g_rdPrevKnown = false, g_rdSaidEcho = false;
// THE BUGGY's DECLARATION (2026-10-28 session 8, run B2). In a vehicle the camera's yaw = the look (+4460) PLUS the buggy camera's
// own smoothed car heading / velocity / drift (CarBuggyCamera FPP update +34A5D0, ida bq2), so the k = 1 VALUE check above fails
// on most frames while driving ([RENDPOSE] declared 0-110 of ~130 / s, fit offset -86.1 .. -91.6 deg) and passes exactly when
// parked (k = 1, residual 0.000-0.002 deg, every frame declared - so the head yaw IS in +4460 in the buggy). A refused frame is
// declared with today's pose (the turn judder Stage 2b removed on foot, INV-326) and its AFW rebuild is not gated in (GateNone):
// run B3 rebuilt 48-163 of ~650 buggy frames per 5 s. While the local player is in a vehicle, a frame the value check refuses is
// declared from ring position k = 1 - the entry the parked frames match - instead of today's pose. NOT established: that k = 1
// is the camera's entry while driving (the car's term hides it from the value check).
// In a vehicle = [[[gd + 0x1C16358] + 1920] + 3008] + 16 (INV-570 route 1 / INV-572 - throwable.cpp's read; the rva is the EGL
// table's on the Epic build). COST: four guarded loads per submit, and only while the declaration is on.
long long g_rdVehSubs = 0, g_rdVehValue = 0, g_rdVehK1 = 0, g_rdVehUnread = 0;
double    g_rdVehResSum = 0.0;
float     g_rdVehResMax = 0.0f;
ULONGLONG g_rdVehNext = 0;
bool      g_rdVehSaidFirst = false;
// THE BUGGY's DECLARATION v2 (2026-10-29, docs/superpowers/specs/2026-10-29-vehicle-declaration-design.md; Jay: "Yes, build the
// declaration fix first"). Run B10 (the RENDPOSE 1 s fit, diag_render_pose=1): in the buggy the camera carried ring k = 0 in 73 of 82 s
// (PARKED: every second, residual <= 0.05 deg) while v1 above declared k = 1 - one head update behind on ~90 % of frames (Jay: the
// world doubles on a head turn, "visible whilst parked"). B2 parked matched k = 1 -> the carried entry is not fixed. So every ring
// entry is matched by value against the car's own term, TRACKED frame to frame (camera yaw = -(+4460[k]) - 90 deg + that term;
// parked it is 0): the entry within kRdVehTolDeg of the predicted term is declared and the term follows it; none -> RE-ACQUIRE from
// k = 0 (the entry carried in 73 / 82 buggy s and 54 / 60 on-foot s of B10). Submit thread only. COST: in a vehicle, <= kRpRing
// entries x one wrap per submit.
constexpr float kRdVehTolDeg  = 0.25f;   // = kRpTolDeg, the RENDPOSE probe's own pairing tolerance
constexpr float kRdVehStepDeg = 1.0f;    // the prediction's largest per-frame change of the car's term
constexpr float kRdVehSameDeg = 0.05f;   // entries this close are the same pose (head still): the newest is kept
float     g_rdVehOff = 0.0f, g_rdVehOffPrev = 0.0f;   // the car's term (deg) at the last two declared vehicle frames
bool      g_rdVehHave = false;
long long g_rdVehMatch = 0, g_rdVehReacq = 0, g_rdVehAt[4] = {};
double    g_rdVehMResSum = 0.0;
float     g_rdVehMResMax = 0.0f, g_rdVehOffMin = 0.0f, g_rdVehOffMax = 0.0f;
bool      g_rdVehOffSeen = false;
// THE LEFTOVER JUDDER (2026-10-29, Jay: "Confirm the cause for the leftover judder"; run B11: "some weird judder if I was turning in
// one direction & looking the opposite"). LOG-ONLY. Static (gamedll 97046bba, ida/bq2): the buggy camera's heading is the car's
// forward through a spring (0.5 / 0.5, a1+232) blended with the travel direction (0 below 5 m/s .. 0.5 at 15 m/s) - not rigid to the
// car - so the tracked term above is the game turning the view by itself. Per vehicle frame: the term's change dOff against the
// declared head yaw's change dHead (ring entry .after), both deg / frame; counted when both exceed kRdVehMoveDeg.
constexpr float kRdVehMoveDeg = 0.10f;
float     g_rdVehLastAfter = 0.0f, g_rdVehDOffMax = 0.0f;
long long g_rdVehDOffBig = 0, g_rdVehOpp = 0, g_rdVehSame = 0, g_rdVehTermOnly = 0;
// THE FALLBACK FRAMES, ONE BY ONE (2026-10-29, Jay: "Do it" - the full pass of B10-B12 found no per-frame field; STATE 10-29 (1)).
// LOG-ONLY, gate diag_render_pose, in a vehicle. Each re-acquired frame: every ring entry's miss against the prediction (a near miss
// = the tolerance, a far one = the camera's own jump), the jump it made; and the NEXT frame: matched or not, its entry, its term against
// the term before the fallback (back within ~0.3 deg = the fallback was a wrong pick, out and back) and against the fallback's.
// kRdVehLogCap lines per session (each fallback + its next frame), then one cap line. COST: <= ~10 lines / s while it logs.
constexpr int kRdVehLogCap = 300;
int       g_rdVehLogN = 0;
bool      g_rdVehAfterPending = false, g_rdVehSaidCap = false;
float     g_rdVehOffBefore = 0.0f, g_rdVehOffAtFb = 0.0f;
// THE HELD ENTRY (2026-11-05 (2), Jay: "Yes"; vehpick_logic.h, STATE 11-05 (2)). v2's per-frame match (ties to the newest, term tracked from the
// entry chosen) was right only while the camera carried k = 0: on k = 1 the head's own step became a false car term (BG5: 41.6 % of parked and
// 66.9 % of driving head-turning frames off; [CARLAG] proved the car still). Now the entry declared is the held one; the match below only keeps
// the counters and the FALLBACK lines: "DECLARED BY VALUE" = the held entry was within tolerance of the prediction, "RE-ACQUIRED" = it was not (the
// term moved; the frame is still declared from the held entry). Submit thread only, as the rest of this block.
// [LOADBEARING: veh_hold_pick] g_vehPick - without it a vehicle frame is declared from the wrong head update on head turns (judder / doubling, INV-649).
vehpick::State g_vehPick;
long g_vhpLines = 0;
bool g_vhpSaid = false;
// The ring entry to declare a vehicle frame from. Updates the tracked term from the entry it returns.
int RdVehPick(const RpCapture& c)
{
    float pred = 0.0f;
    if (g_rdVehHave) {
        float d = g_rdVehOff - g_rdVehOffPrev;
        d = d > kRdVehStepDeg ? kRdVehStepDeg : (d < -kRdVehStepDeg ? -kRdVehStepDeg : d);
        pred = g_rdVehOff + d;
    }
    int best = -1;
    float br = 1e9f;
    float rj[kRpRing];
    for (int j = 0; j < kRpRing; ++j) { rj[j] = -1.0f; }
    for (int j = 0; j < c.n && j < kRpRing; ++j) {
        const float r = fabsf(VpWrap(c.cy + c.ring[j].after / 57.2957795f + 1.5707963f - pred / 57.2957795f)) * 57.2957795f;
        rj[j] = r;
        if (r < br - kRdVehSameDeg) { br = r; best = j; }   // ascending j: a newer entry within kRdVehSameDeg is kept (the log's "nearest")
    }
    float offs[kRpRing] = {};
    const int nOff = c.n < kRpRing ? c.n : kRpRing;
    for (int j = 0; j < nOff; ++j) { offs[j] = VpWrap(c.cy + c.ring[j].after / 57.2957795f + 1.5707963f) * 57.2957795f; }
    if (!g_rdVehHave) { vehpick::Reset(g_vehPick); }
    const int heldBefore = g_vehPick.k;
    const int use = vehpick::Step(g_vehPick, offs, nOff, c.ring[0].h, c.ring[nOff > 3 ? 3 : (nOff > 0 ? nOff - 1 : 0)].h);
    if (!g_vhpSaid || (use != heldBefore && g_vhpLines < 200)) {
        ++g_vhpLines;
        // [PROBE: VEHPICK] gate=none (in a vehicle; the first frame, then each change of the held entry, capped 200)  site=HEADSET  log-only=yes
        LOG("camhook: [VEHPICK] %s entry k%d -> k%d | score held %.5f, new %.5f | head yaw k0 minus k3 %+.2f deg",
            g_vhpSaid ? "HELD" : "FIRST (logged unconditionally) held", heldBefore, use, g_vehPick.sc[heldBefore < kRpRing ? heldBefore : 0],
            g_vehPick.sc[use], nOff > 3 ? c.ring[0].h - c.ring[3].h : 0.0f);
        g_vhpSaid = true;
    }
    const float ur = use < nOff ? rj[use] : 1e9f;
    const bool fb = !(ur <= kRdVehTolDeg);
    if (!fb) {
        ++g_rdVehMatch;
        ++g_rdVehAt[use < 3 ? use : 3];
        g_rdVehMResSum += ur;
        if (ur > g_rdVehMResMax) { g_rdVehMResMax = ur; }
    } else {
        ++g_rdVehReacq;
    }
    const float off = offs[use];
    if (DiagRenderPose && g_rdVehHave) {   // THE FALLBACK FRAMES, ONE BY ONE (above)
        auto wd = [](float a) { return VpWrap(a / 57.2957795f) * 57.2957795f; };
        if (g_rdVehAfterPending) {
            g_rdVehAfterPending = false;
            if (g_rdVehLogN < kRdVehLogCap) {
                ++g_rdVehLogN;
                // [PROBE: RENDDECL] gate=diag_render_pose (in a vehicle; each fallback frame + the next, capped)  site=HEADSET  log-only=yes
                LOG("camhook: [RENDDECL] VEHICLE AFTER: %s k%d (miss %.3f deg) | term %.2f deg = %+.2f vs before the fallback (%.2f), "
                    "%+.2f vs the fallback's (%.2f)", fb ? "ANOTHER FALLBACK, nearest" : "matched", fb ? best : use, br, off,
                    wd(off - g_rdVehOffBefore), g_rdVehOffBefore, wd(off - g_rdVehOffAtFb), g_rdVehOffAtFb);
            }
        }
        if (fb) {
            if (g_rdVehLogN < kRdVehLogCap) {
                ++g_rdVehLogN;
                const float a0 = c.ring[0].after;
                // [PROBE: RENDDECL] gate=diag_render_pose (in a vehicle; each fallback frame + the next, capped)  site=HEADSET  log-only=yes
                LOG("camhook: [RENDDECL] VEHICLE FALLBACK: predicted term %.2f deg (last %.2f, step %+.2f) | miss per entry k0..k5 %.3f "
                    "%.3f %.3f %.3f %.3f %.3f deg (n %d), nearest k%d %.3f | declared k%d (held) -> term %.2f = jump %+.2f deg | head yaw k0 minus "
                    "k1..k3 %+.2f %+.2f %+.2f deg", pred, g_rdVehOff, pred - g_rdVehOff, rj[0], rj[1], rj[2], rj[3], rj[4], rj[5], c.n,
                    best, br, use, off, wd(off - g_rdVehOff), c.n > 1 ? a0 - c.ring[1].after : 0.0f, c.n > 2 ? a0 - c.ring[2].after : 0.0f,
                    c.n > 3 ? a0 - c.ring[3].after : 0.0f);
                g_rdVehAfterPending = true;
                g_rdVehOffBefore = g_rdVehOff;
                g_rdVehOffAtFb = off;
            } else if (!g_rdVehSaidCap) {
                g_rdVehSaidCap = true;
                // [PROBE: RENDDECL] gate=diag_render_pose (in a vehicle; each fallback frame + the next, capped)  site=HEADSET  log-only=yes
                LOG("camhook: [RENDDECL] VEHICLE FALLBACK lines: cap %d reached - the 5 s counts continue", kRdVehLogCap);
            }
        }
    }
    if (g_rdVehHave) {   // the leftover-judder counters (above): the game's own turn of the view vs the head's
        const float dOff = VpWrap((off - g_rdVehOff) / 57.2957795f) * 57.2957795f;
        const float dHead = c.ring[use].after - g_rdVehLastAfter;
        const float aO = fabsf(dOff), aH = fabsf(dHead);
        if (aO > g_rdVehDOffMax) { g_rdVehDOffMax = aO; }
        if (aO > 0.25f) { ++g_rdVehDOffBig; }
        if (aO > kRdVehMoveDeg && aH > kRdVehMoveDeg) {
            if ((dOff > 0.0f) != (dHead > 0.0f)) { ++g_rdVehOpp; } else { ++g_rdVehSame; }
        } else if (aO > kRdVehMoveDeg) {
            ++g_rdVehTermOnly;
        }
    }
    g_rdVehLastAfter = c.ring[use].after;
    g_rdVehOffPrev = g_rdVehHave ? g_rdVehOff : off;
    g_rdVehOff = off;
    g_rdVehHave = true;
    if (!g_rdVehOffSeen || off < g_rdVehOffMin) { g_rdVehOffMin = off; }
    if (!g_rdVehOffSeen || off > g_rdVehOffMax) { g_rdVehOffMax = off; }
    g_rdVehOffSeen = true;
    return use;
}
uintptr_t VehGlobalRva()   // separate from the __try below: a function-local static cannot sit in a function using __try
{
    static const uintptr_t s = DLVR_VEH_GD_RVA(0x1C16358);
    return s;
}
uintptr_t VehGameDll()
{
    static uintptr_t s = 0;
    if (!s) { s = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll")); }
    return s;
}
// 1 = the local player is in a vehicle, 0 = not, -1 = not read (no game dll / HUD / player, or a fault).
int RdLocalPlayerInVehicle()
{
    const uintptr_t gd = VehGameDll();
    const uintptr_t rva = VehGlobalRva();
    if (!gd) { return -1; }
    __try {
        const uintptr_t hud = *reinterpret_cast<const volatile uintptr_t*>(gd + rva);
        if (!hud) { return -1; }
        const uintptr_t pl = *reinterpret_cast<const volatile uintptr_t*>(hud + 1920);
        if (!pl) { return -1; }
        const uintptr_t vc = *reinterpret_cast<const volatile uintptr_t*>(pl + 3008);
        if (!vc) { return 0; }
        return *reinterpret_cast<const volatile unsigned char*>(vc + 16) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// BUGGY STEADY COCKPIT (A): the car's term at the vehicle root, by RdVehPick's rule on the root's OWN arriving yaw and ONE ring
// snapshot - prediction = last + last step clamped to kRdVehStepDeg, the entry within kRdVehTolDeg (newest within kRdVehSameDeg);
// with none, the entry NEAREST the prediction when its miss is <= kVcNearDeg (B13 nearest miss: median 0.366, p90 0.986 deg), else
// k0. Unlike the declaration this result is CANCELLED in the picture, so a wrong pick is drawn: the nearest is the better guess.
// cyRad = the root's yaw (YawOnly, the same convention as RpCapture::cy). Root-copy thread only (one per frame); the counters are
// read and zeroed by CamHookCutsceneTick. COST: one 8-entry ring copy + 8 wraps per root copy (~1 per frame, in a vehicle only).
constexpr float kVcNearDeg  = 1.0f;
constexpr ULONGLONG kVcGapMs = 250;   // no root copy for this long = a new stretch: the term is re-acquired
float     g_vcOff = 0.0f, g_vcOffPrev = 0.0f;
bool      g_vcHave = false;
ULONGLONG g_vcLastMs = 0;
volatile LONG g_vcCopies = 0, g_vcValue = 0, g_vcNearest = 0, g_vcK0 = 0, g_vcNoRing = 0, g_vcReacq = 0;
volatile LONG g_vcReset = 1;           // CamHookCutsceneTick sets it after reading min / max
float     g_vcMin = 0.0f, g_vcMax = 0.0f, g_vcLast = 0.0f, g_vcStepMax = 0.0f;
bool RpRingSnapshot(RpEntry out[kRpRing], int& n);   // below
// VEHREC, THE ROOT (2026-11-05 (2), Jay: "Yes"): what VehCockpitTerm saw and picked, one record per root copy, so its fix can be replayed the way
// RdVehPick's was. LOG-ONLY, gate diag_render_pose. The root runs on a job thread (no lock, file or allocation): a record goes into a 512-slot ring,
// the slot's seq last; VehRecNote (the submit thread) drains it to dlvr_vehroot.bin. COST on: one QPC + ~30 float copies per root copy.
#pragma pack(push, 1)
struct VehRootRec {
    long long qpc;
    float     cy;               // the root's yaw going into the term (rad)
    float     pred;             // the prediction (deg)
    float     off;              // the term cancelled (deg)
    int       n;
    int       use;              // the entry picked
    int       kind;             // 0 by value, 1 nearest, 2 k0
    float     after[kRpRing];
    float     h[kRpRing];
    long      frame[kRpRing];
};
#pragma pack(pop)
static_assert(sizeof(VehRootRec) == 128, "VEHREC root record size: the replay tool reads it");
constexpr long kVrtSlots = 512;
VehRootRec    g_vrtRec[kVrtSlots];
volatile LONG g_vrtSeq[kVrtSlots] = {};   // the number the slot holds, written last
volatile LONG g_vrtHead = 0;              // records claimed
void VehRootNote(float cyRad, float pred, float off, int n, int use, int kind, const RpEntry* ring)
{
    const LONG me = InterlockedIncrement(&g_vrtHead);
    VehRootRec& r = g_vrtRec[(me - 1) % kVrtSlots];
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.qpc = q.QuadPart; r.cy = cyRad; r.pred = pred; r.off = off; r.n = n; r.use = use; r.kind = kind;
    for (int j = 0; j < kRpRing; ++j) {
        r.after[j] = j < n ? ring[j].after : 0.0f;
        r.h[j] = j < n ? ring[j].h : 0.0f;
        r.frame[j] = j < n ? ring[j].frame : 0;
    }
    InterlockedExchange(&g_vrtSeq[(me - 1) % kVrtSlots], me);
}

// The root carries k = 0 (INV-679), not the eye copies' usual k = 1.
vehpick::State MakeRootPick() { vehpick::State s; s.k = 0; return s; }
vehpick::State g_vcPick = MakeRootPick();
bool VehCockpitTerm(float cyRad, float* termDeg)
{
    RpEntry ring[kRpRing];
    int n = 0;
    if (!RpRingSnapshot(ring, n) || n <= 0) {
        InterlockedIncrement(&g_vcNoRing);
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_vcHave && now - g_vcLastMs > kVcGapMs) {
        g_vcHave = false;
        InterlockedIncrement(&g_vcReacq);
    }
    g_vcLastMs = now;
    float pred = 0.0f;
    if (g_vcHave) {
        float d = g_vcOff - g_vcOffPrev;
        d = d > kRdVehStepDeg ? kRdVehStepDeg : (d < -kRdVehStepDeg ? -kRdVehStepDeg : d);
        pred = g_vcOff + d;
    }
    // THE HELD ENTRY AT THE ROOT (2026-11-05 (2), Jay: "Do it"; vehpick_logic.h, INV-679). The per-frame match strayed onto k1-k7 on 35 % of head-still driving
    // copies (BG6: the root carries k0, 3,137 of 3,140 parked): while the car turns the entries differ by its own turn, so a wrong entry read as a term and the
    // cancel rotated the picture and snapped back (6.95 deg in one copy). Replay: 2,376 entry changes -> 0, picture wobble over 0.5 deg 613 -> 11 frames.
    // The counters keep their names: "by value" = the held entry within tolerance of the prediction, "nearest" = within kVcNearDeg, "k0" = neither - the
    // entry declared is the held one in all three. Same one job thread at a time as the g_vc* it sits beside.
    // [LOADBEARING: veh_root_hold] g_vcPick - without it the steady cockpit's cancel jumps while the car turns.
    float offs[kRpRing] = {};
    const int nOff = n < kRpRing ? n : kRpRing;
    for (int j = 0; j < nOff; ++j) { offs[j] = VpWrap(cyRad + ring[j].after / 57.2957795f + 1.5707963f) * 57.2957795f; }
    if (!g_vcHave) { vehpick::Reset(g_vcPick); }
    const int use = vehpick::Step(g_vcPick, offs, nOff, ring[0].h, ring[nOff > 3 ? 3 : nOff - 1].h);
    const float heldMiss = fabsf(VpWrap((offs[use] - pred) / 57.2957795f)) * 57.2957795f;
    int vrKind = 2;
    if (heldMiss <= kRdVehTolDeg) {
        vrKind = 0;
        InterlockedIncrement(&g_vcValue);
    } else if (g_vcHave && heldMiss <= kVcNearDeg) {
        vrKind = 1;
        InterlockedIncrement(&g_vcNearest);
    } else {
        InterlockedIncrement(&g_vcK0);
    }
    const float off = offs[use];
    if (DiagRenderPose) { VehRootNote(cyRad, pred, off, n, use, vrKind, ring); }   // VEHREC: off -> one bool test
    if (InterlockedExchange(&g_vcReset, 0)) {
        g_vcMin = g_vcMax = off;
        g_vcStepMax = 0.0f;
    }
    if (off < g_vcMin) { g_vcMin = off; }
    if (off > g_vcMax) { g_vcMax = off; }
    if (g_vcHave) {
        const float st = fabsf(VpWrap((off - g_vcOff) / 57.2957795f)) * 57.2957795f;
        if (st > g_vcStepMax) { g_vcStepMax = st; }
    }
    g_vcOffPrev = g_vcHave ? g_vcOff : off;
    g_vcOff = off;
    g_vcHave = true;
    g_vcLast = off;
    InterlockedIncrement(&g_vcCopies);
    *termDeg = off;
    return true;
}

// [PROBE: VEHREC] gate=diag_render_pose, in a vehicle, a tagged capture  site=HEADSET  log-only=yes
// ===== VEHREC - THE BUGGY's DECLARATION, FRAME BY FRAME, FOR OFFLINE REPLAY (2026-11-05, Jay: "I want you to be 100% certain for this fix"
// -> "Yes"; STATE 11-05 (2)). LOG-ONLY. RdVehPick is proven wrong when the camera carries k != 0 (BG1-3: 449 of 450 fallbacks), but three
// replacement rules were only testable on a SYNTHETIC head. One binary record per vehicle frame - what the submit already holds, no game
// read, nothing the game can see - into its OWN file dlvr_vehrec.bin beside the log (truncated per launch), so every rule replays on the
// real head + car. Layout below; tools read it by the header's record size. Submit thread only.
// COST: off - one bool test. On, in a vehicle: ~90 float copies + one buffered fwrite of 176 bytes per submit (~22 KB / s); the CRT buffer
// (256 KB) reaches the OS about every 12 s; fopen once, on the first vehicle frame; fflush on leaving the vehicle. 80 MB cap (~1 hour).
#pragma pack(push, 1)
struct VehRec {
    long long qpc;              // the capture's QPC
    long long capSeq;           // the capture's number
    long      capFrame;         // g_rpFrame at the capture
    float     cy;               // the anchor camera's engine yaw (rad)
    float     pos[3];           // the anchor camera's world position - parked vs driving
    float     hp[3];            // the reported head position
    float     hq[4];            // the head quaternion camhook took pitch / roll from
    int       n;                // ring entries valid
    int       use;              // the entry RdVehPick declared
    float     rootTerm;         // VehCockpitTerm's last term at the root (deg)
    unsigned  anchorLow;        // the yaw anchor's address, low 32 bits (a change = a re-decide)
    float     after[kRpRing];   // ring +4460 after the add (deg), newest first
    float     h[kRpRing];       // ring yaw added (deg)
    long      frame[kRpRing];   // ring g_rpFrame
};
#pragma pack(pop)
static_assert(sizeof(VehRec) == 176, "VEHREC record size: the replay tool reads it");
FILE*     g_vrFile = nullptr;
bool      g_vrRefused = false, g_vrCapped = false, g_vrDirty = false;
long long g_vrN = 0, g_vrRootN = 0, g_vrRootLost = 0;
FILE*     g_vrRootFile = nullptr;
LONG      g_vrTail = 0;
constexpr long long kVrCapRecords = 80ll * 1024 * 1024 / static_cast<long long>(sizeof(VehRec));

bool VehRecOpen()
{
    HMODULE self = nullptr;
    char path[MAX_PATH] = {};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&VehRecOpen), &self);
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = n ? strrchr(path, '\\') : nullptr;
    if (slash) { strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "dlvr_vehrec.bin"); }
    FILE* f = nullptr;
    if (!slash || fopen_s(&f, path, "wb") != 0 || !f) {
        g_vrRefused = true;
        // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
        LOG("camhook: [VEHREC] REFUSED - could not open '%s' for writing; nothing recorded this launch. diag_render_pose=1", path);
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 256 * 1024);
    LARGE_INTEGER qpf = {};
    QueryPerformanceFrequency(&qpf);
    const unsigned recSize = static_cast<unsigned>(sizeof(VehRec));
    fwrite("DLVRVEH1", 1, 8, f);
    fwrite(&recSize, sizeof(recSize), 1, f);
    fwrite(&qpf.QuadPart, sizeof(qpf.QuadPart), 1, f);
    g_vrFile = f;
    // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
    LOG("camhook: [VEHREC] RECORDING to '%s' (logged unconditionally) - %u bytes per vehicle frame. diag_render_pose=1", path, recSize);
    // the root's own file, beside it
    strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "dlvr_vehroot.bin");
    FILE* fr = nullptr;
    if (fopen_s(&fr, path, "wb") != 0 || !fr) {
        // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
        LOG("camhook: [VEHREC] ROOT REFUSED - could not open '%s'; the root is not recorded this launch. diag_render_pose=1", path);
        return true;
    }
    setvbuf(fr, nullptr, _IOFBF, 256 * 1024);
    const unsigned rootSize = static_cast<unsigned>(sizeof(VehRootRec));
    fwrite("DLVRVRT1", 1, 8, fr);
    fwrite(&rootSize, sizeof(rootSize), 1, fr);
    fwrite(&qpf.QuadPart, sizeof(qpf.QuadPart), 1, fr);
    g_vrRootFile = fr;
    g_vrTail = g_vrtHead;    // from here on: what the ring holds from before the vehicle is not this recording's
    // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
    LOG("camhook: [VEHREC] ROOT RECORDING to '%s' (logged unconditionally) - %u bytes per root copy. diag_render_pose=1", path, rootSize);
    return true;
}

// The root's ring to its file. Submit thread.
void VehRootDrain()
{
    if (!g_vrRootFile) { return; }
    const LONG head = g_vrtHead;
    if (head - g_vrTail > kVrtSlots) {
        g_vrRootLost += head - g_vrTail - kVrtSlots;
        g_vrTail = head - kVrtSlots;
    }
    while (g_vrTail < head) {
        const long slot = g_vrTail % kVrtSlots;
        if (g_vrtSeq[slot] != g_vrTail + 1) { break; }   // being written: next submit
        fwrite(&g_vrtRec[slot], sizeof(VehRootRec), 1, g_vrRootFile);
        ++g_vrTail;
        ++g_vrRootN;
    }
}

void VehRecNote(const RpCapture& c, int use)
{
    if (g_vrRefused || g_vrCapped) { return; }
    if (!g_vrFile && !VehRecOpen()) { return; }
    VehRec r = {};
    r.qpc = c.qpc; r.capSeq = c.seq; r.capFrame = c.frame; r.cy = c.cy;
    for (int i = 0; i < 3; ++i) { r.pos[i] = c.pos[i]; r.hp[i] = c.hp[i]; }
    for (int i = 0; i < 4; ++i) { r.hq[i] = c.hq[i]; }
    r.n = c.n; r.use = use; r.rootTerm = g_vcLast; r.anchorLow = static_cast<unsigned>(g_yawAnchor);
    for (int j = 0; j < c.n && j < kRpRing; ++j) { r.after[j] = c.ring[j].after; r.h[j] = c.ring[j].h; r.frame[j] = c.ring[j].frame; }
    fwrite(&r, sizeof(r), 1, g_vrFile);
    VehRootDrain();
    g_vrDirty = true;
    if (++g_vrN == 1) {
        // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
        LOG("camhook: [VEHREC] FIRST RECORD (logged unconditionally) - cy %.4f rad, ring entries %d, declared entry %d, anchor low %08X. "
            "diag_render_pose=1", r.cy, r.n, r.use, r.anchorLow);
    }
    if (g_vrN >= kVrCapRecords) {
        g_vrCapped = true;
        fflush(g_vrFile);
        // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
        LOG("camhook: [VEHREC] CAP reached - %lld frames, recording stopped. diag_render_pose=1", g_vrN);
    }
}

// Leaving the vehicle: the buffer to disk, and the count. COST: one bool test on every on-foot submit.
void VehRecLeft()
{
    if (!g_vrDirty || !g_vrFile) { return; }
    g_vrDirty = false;
    VehRootDrain();
    fflush(g_vrFile);
    if (g_vrRootFile) { fflush(g_vrRootFile); }
    // [PROBE: VEHREC] gate=diag_render_pose, in a vehicle  site=HEADSET  log-only=yes
    LOG("camhook: [VEHREC] flushed on leaving the vehicle - %lld frames so far, %lld root copies, %lld root copies lost. diag_render_pose=1",
        g_vrN, g_vrRootN, g_vrRootLost);
}

// ===== [RENDTAG] - INV-325 (a)/(b), LOG-ONLY: CAN THE POSE RECORDER TAG THE SUBMITTED FRAME WITH ITS CAPTURE? =====
// rd3d11's opcode-2 recorder (sub_18003B9B0, slot 164) is detoured PASS-THROUGH (boneblend.cpp rendtag::Hook, args unchanged)
// and calls CamHookRendTagRecord(frameIndex) first, on the thread that records the pose (the main thread, INV-325). It notes
// the newest anchor capture - its number, its thread, its age - and queues {frameIndex, capture number}. Each submit pops one
// entry. Per second: does the recorder run on the capture's thread, how old is the capture, how many captures per record,
// does each submit pop the record carrying ITS OWN frameIndex (one record per submit, in order), is the tagged capture still
// in the history, and does it AGREE with the echo pairing Stage 2 used (INV-324 predicts ~half disagree while turning).
struct RtRec {
    long long frameIndex;
    LONG64    capSeq;     // 0 = no capture existed
    bool      sameThread;
    float     ageUs;
    int       skipEye;    // afwalt::g_skipEyeNow at the record: the eye this frame did NOT render (-1 = both rendered)
    LONG64    afwFrame;   // afwalt::g_frame at the record - [AFWCAM] keys the image's GPU view-projection by it
};
int g_afwSubmitSkip = -2;   // submit thread: the submit's OWN record's skipEye; -2 = no own record this submit
constexpr int   kRtFifo = 16;
RtRec           g_rtFifo[kRtFifo];        // written by the recorder thread only
volatile LONG64 g_rtPushed = 0;           // entries published (recorder thread)
LONG64          g_rtConsumed = 0;         // submit thread only
LONG64          g_rtLastCount = 0;        // recorder thread only
// recorder-side CUMULATIVE counters (one writer); the submit thread logs per-window deltas
volatile LONG64 g_rtRecCalls = 0, g_rtNoCap = 0, g_rtSame = 0, g_rtOther = 0, g_rtSince[4] = {}, g_rtAgeSumUs = 0;
volatile LONG   g_rtAgeMaxUs = 0;         // reset by the submit thread each window (a racy max, labelled as such)
volatile LONG   g_rtRecTid = 0, g_rtCapTidSeen = 0;
bool            g_rtRecSaid = false;      // recorder thread
// submit-side window counters
long long g_rtSubs = 0, g_rtPend[4] = {}, g_rtEmpty = 0, g_rtOverrun = 0, g_rtInOrder = 0, g_rtOutOfOrder = 0,
          g_rtNotInHist = 0, g_rtNoCapRec = 0, g_rtAgreeTurn = 0, g_rtDisTurn = 0, g_rtAgreeStill = 0, g_rtDisStill = 0,
          g_rtEchoNone = 0, g_rtK1Fail = 0;
long long g_rtTagPos[kRpCapHist] = {};
// v2 (2026-10-01): is the submit's own record pending at all? Every pending record is compared by frameIndex.
//   match pos 0/1/2/3+ = the FIRST pending record (0 = oldest) whose frameIndex == the submit's; none; multi = 2+ matched.
//   head diff = submit frameIndex - the OLDEST pending record's, -3..+3 (else 'other').
// The record used for the TAG columns is now the first match (records before it are discarded, counted), else the oldest.
long long g_rtMatchPos[4] = {}, g_rtMatchNone = 0, g_rtMatchMulti = 0, g_rtHeadDiff[8] = {}, g_rtSkipped = 0;
int       g_rtRawLines = 0;
constexpr int kRtRawMax = 20;
LONG64    g_rtPrevRec[7] = {};            // recorder cumulative counters at the previous window
float     g_rtCorr[kRpMax];
int       g_rtCorrN = 0;
bool      g_rtSaidOrder = false, g_rtSaidFirst = false;
float     g_rtPrevYaw = 0.0f;
double    g_rtPrevT = 0.0;
bool      g_rtHavePrev = false;

float RpPct(float* v, int n, float p);   // defined below

void RtReport()
{
    const LONG64 cur[7] = { g_rtRecCalls, g_rtNoCap, g_rtSame, g_rtOther, g_rtAgeSumUs, g_rtSince[0], g_rtSince[1] };
    const LONG64 since2 = g_rtSince[2], since3 = g_rtSince[3];
    static LONG64 prev2 = 0, prev3 = 0;
    const LONG64 calls = cur[0] - g_rtPrevRec[0];
    const LONG64 withCap = (cur[2] - g_rtPrevRec[2]) + (cur[3] - g_rtPrevRec[3]);
    const float corrMed = RpPct(g_rtCorr, g_rtCorrN, 0.5f), corrP90 = RpPct(g_rtCorr, g_rtCorrN, 0.9f);
    // [PROBE: RENDTAG] gate=diag_render_pose  site=HEADSET  log-only=yes
    LOG("camhook: [RENDTAG] window RECORDER calls %lld (thread %ld; the newest capture's thread %ld) | newest capture on the "
        "recorder's thread %lld, OTHER thread %lld, none %lld | captures since the previous record 0/1/2/3+ %lld %lld %lld %lld | "
        "capture age at record mean %.0f us, max %ld us | SUBMITS %lld: pending records 0/1/2/3+ %lld %lld %lld %lld, none to "
        "pop %lld, overrun %lld | popped record's frameIndex == the submit's %lld, != %lld | tagged capture pos 0/1/2/3 %lld %lld "
        "%lld %lld, not in history %lld, record had no capture %lld | TAG vs ECHO capture: turning agree %lld disagree %lld; "
        "still agree %lld disagree %lld; echo matched none %lld | tag k=1 check fail %lld | TAG correction while turning n %d "
        "median %.2f p90 %.2f deg | v2: submit frameIndex FIRST matches pending pos 0/1/2/3+ %lld %lld %lld %lld, none %lld, "
        "2+ matches %lld | submit minus OLDEST pending frameIndex -3..+3 %lld %lld %lld %lld %lld %lld %lld, other %lld | "
        "records skipped to reach the match %lld",
        calls, static_cast<long>(g_rtRecTid), static_cast<long>(g_rtCapTidSeen), cur[2] - g_rtPrevRec[2],
        cur[3] - g_rtPrevRec[3], cur[1] - g_rtPrevRec[1], cur[5] - g_rtPrevRec[5], cur[6] - g_rtPrevRec[6], since2 - prev2,
        since3 - prev3, withCap ? static_cast<double>(cur[4] - g_rtPrevRec[4]) / withCap : 0.0,
        static_cast<long>(g_rtAgeMaxUs), g_rtSubs, g_rtPend[0], g_rtPend[1], g_rtPend[2], g_rtPend[3], g_rtEmpty, g_rtOverrun,
        g_rtInOrder, g_rtOutOfOrder, g_rtTagPos[0], g_rtTagPos[1], g_rtTagPos[2], g_rtTagPos[3], g_rtNotInHist, g_rtNoCapRec,
        g_rtAgreeTurn, g_rtDisTurn, g_rtAgreeStill, g_rtDisStill, g_rtEchoNone, g_rtK1Fail, g_rtCorrN, corrMed, corrP90,
        g_rtMatchPos[0], g_rtMatchPos[1], g_rtMatchPos[2], g_rtMatchPos[3], g_rtMatchNone, g_rtMatchMulti, g_rtHeadDiff[0],
        g_rtHeadDiff[1], g_rtHeadDiff[2], g_rtHeadDiff[3], g_rtHeadDiff[4], g_rtHeadDiff[5], g_rtHeadDiff[6], g_rtHeadDiff[7],
        g_rtSkipped);
    for (int i = 0; i < 4; ++i) { g_rtMatchPos[i] = 0; }
    for (int i = 0; i < 8; ++i) { g_rtHeadDiff[i] = 0; }
    g_rtMatchNone = g_rtMatchMulti = g_rtSkipped = 0;
    for (int i = 0; i < 7; ++i) { g_rtPrevRec[i] = cur[i]; }
    prev2 = since2;
    prev3 = since3;
    InterlockedExchange(&g_rtAgeMaxUs, 0);
    g_rtSubs = g_rtEmpty = g_rtOverrun = g_rtInOrder = g_rtOutOfOrder = g_rtNotInHist = g_rtNoCapRec = 0;
    g_rtAgreeTurn = g_rtDisTurn = g_rtAgreeStill = g_rtDisStill = g_rtEchoNone = g_rtK1Fail = 0;
    for (int i = 0; i < 4; ++i) { g_rtPend[i] = 0; }
    for (int i = 0; i < kRpCapHist; ++i) { g_rtTagPos[i] = 0; }
    g_rtCorrN = 0;
}

bool RpRingSnapshot(RpEntry out[kRpRing], int& n)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_rpRingSeq;
        if (s0 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        const LONG64 c = g_rpRingCount;
        RpEntry tmp[kRpRing];
        memcpy(tmp, g_rpRing, sizeof(tmp));
        MemoryBarrier();
        if (g_rpRingSeq != s0) { continue; }
        n = c < kRpRing ? static_cast<int>(c) : kRpRing;
        for (int k = 0; k < n; ++k) { out[k] = tmp[static_cast<int>((c - 1 - k) % kRpRing)]; }
        return true;
    }
    return false;
}

// The capture history, newest first. n = how many exist (<= kRpCapHist).
bool RpReadCaptures(RpCapture out[kRpCapHist], int& n, LONG64& count)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_rpCapSeq;
        if (s0 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        const LONG64 c = g_rpCapCount;
        RpCapture tmp[kRpCapHist];   // per call: the submit thread AND the pose recorder ([RENDTAG], main thread) read
        memcpy(tmp, g_rpCapHist, sizeof(tmp));
        MemoryBarrier();
        if (g_rpCapSeq != s0) { continue; }
        n = c < kRpCapHist ? static_cast<int>(c) : kRpCapHist;
        for (int i = 0; i < n; ++i) { out[i] = tmp[static_cast<int>((c - 1 - i) % kRpCapHist)]; }
        count = c;
        return true;
    }
    return false;
}

// Two unit quaternions equal to within tol per component, either sign (q and -q are one rotation).
bool RpQuatSame(const float a[4], const float b[4], float tol)
{
    const float sgn = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (!(fabsf(a[i] - sgn * b[i]) <= tol)) { return false; }
    }
    return true;
}

// OpenXR, Y-up, forward -Z: the same yaw/pitch extraction [VIEWPOS] uses for the head.
float RpYaw(const float q[4])
{
    return atan2f(-2.0f * (q[0] * q[2] + q[1] * q[3]), -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])));
}
float RpPitch(const float q[4])
{
    return asinf(fmaxf(-1.0f, fminf(1.0f, 2.0f * (q[3] * q[0] - q[1] * q[2]))));
}

// p in [0,1] of the first n values of v (sorts v in place). 0 when n == 0.
float RpPct(float* v, int n, float p)
{
    if (n <= 0) { return 0.0f; }
    std::sort(v, v + n);
    int i = static_cast<int>(p * (n - 1) + 0.5f);
    if (i > n - 1) { i = n - 1; }
    return v[i];
}

// R2: called from LevelCamera for the yaw anchor only, every time it is levelled. q = the ENGINE camera's rotation,
// hq = the reported head camhook applied, pos = the engine camera's world position ([AFWCAM]), hp = the reported head
// position standing at the capture ([AFWCAM] step 2).
void RendPoseCapture(const float q[4], const float hq[4], const float pos[3], const float hp[3])
{
    RpCapture c;
    if (!RpRingSnapshot(c.ring, c.n)) {
        InterlockedIncrement64(&g_rpCapTorn);
        return;
    }
    float cyq[4];
    YawOnly(q, CamUpAxis(), cyq);
    c.cy = (CamUpAxis() == 1)
        ? atan2f(2.0f * (cyq[3] * cyq[2] + cyq[0] * cyq[1]), 1.0f - 2.0f * (cyq[1] * cyq[1] + cyq[2] * cyq[2]))
        : atan2f(2.0f * (cyq[3] * cyq[1] + cyq[0] * cyq[2]), 1.0f - 2.0f * (cyq[0] * cyq[0] + cyq[1] * cyq[1]));
    for (int i = 0; i < 4; ++i) { c.hq[i] = hq[i]; }
    for (int i = 0; i < 3; ++i) { c.pos[i] = pos[i]; c.hp[i] = hp[i]; }
    {
        // Main-menu head turn: the root is levelled before the anchor (the anchor is a copy made FROM it), so the yaw
        // standing here is the one this camera carries. 100 ms = "the root is still being given one" (it gets ~120/s).
        const ULONGLONG ms = g_mmTurnSyMs;
        c.mm = TurnRoot() != 0 && ms != 0 && GetTickCount64() - ms < 100;   // the menu's root or a cutscene's (INV-620 (c))
        for (int i = 0; i < 4; ++i) { c.mmSy[i] = g_mmTurnSy[i]; }
    }
    c.frame = g_rpFrame;
    c.tid = GetCurrentThreadId();
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        c.qpc = q.QuadPart;
    }
    c.seq = g_rpCapCount + 1;   // one writer (the anchor's levelling); the count it becomes below
    InterlockedIncrement(&g_rpCapSeq);   // odd = writing
    MemoryBarrier();
    memcpy(&g_rpCapHist[static_cast<int>(g_rpCapCount % kRpCapHist)], &c, sizeof(c));
    g_rpCapCount = g_rpCapCount + 1;
    MemoryBarrier();
    InterlockedIncrement(&g_rpCapSeq);   // even = stable
    if (!g_rpCapSaid) {
        g_rpCapSaid = true;
        // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
        LOG("camhook: [RENDPOSE] FIRST CAPTURE (logged unconditionally) engine camera yaw %+.2f deg | ring entries %d%s | "
            "newest: +4460 %.2f deg, head yaw added %+.2f deg | frame %ld",
            c.cy * 57.2958f, c.n, c.n ? "" : " - EMPTY: camyaw::Hook has not pushed (design D not adding yaw?)",
            c.n ? c.ring[0].after : 0.0f, c.n ? c.ring[0].h : 0.0f, static_cast<long>(c.frame));
    }
}

void RpReport()
{
    const float kDeg = 57.2957795f;
    // --- the +4460 -> camera-yaw fit: sign s, ring position k, offset c ---
    int bestS = 0, bestK = -1, bestN = 0, secS = 0, secK = -1;
    float bestC = 0.0f, bestMed = 1e9f, secMed = 1e9f;
    static float res[kRpMax];
    for (int si = 0; si < 2; ++si) {
        const int s = si == 0 ? 1 : -1;
        for (int k = 0; k < 4; ++k) {
            double ss = 0.0, cs = 0.0;
            int n = 0;
            for (int i = 0; i < g_rpN; ++i) {
                const RpCapture& c = g_rpS[i].cap;
                if (c.n <= k) { continue; }
                const float d = VpWrap(c.cy - s * c.ring[k].after / kDeg);
                ss += sin(d); cs += cos(d); ++n;
            }
            if (n < 10) { continue; }
            const float cOff = static_cast<float>(atan2(ss, cs));
            int m = 0;
            for (int i = 0; i < g_rpN; ++i) {
                const RpCapture& c = g_rpS[i].cap;
                if (c.n <= k) { continue; }
                res[m++] = fabsf(VpWrap(c.cy - s * c.ring[k].after / kDeg - cOff)) * kDeg;
            }
            const float med = RpPct(res, m, 0.5f);
            if (med < bestMed) {
                secMed = bestMed; secS = bestS; secK = bestK;
                bestMed = med; bestS = s; bestK = k; bestC = cOff; bestN = n;
            } else if (med < secMed) {
                secMed = med; secS = s; secK = k;
            }
        }
    }
    char fitTxt[200];
    if (bestK < 0) {
        _snprintf_s(fitTxt, sizeof(fitTxt), _TRUNCATE, "FIT: NO VERDICT - fewer than 10 submits with a ring entry");
    } else {
        _snprintf_s(fitTxt, sizeof(fitTxt), _TRUNCATE,
                    "FIT camera yaw = %+d * +4460[k=%d newest-back] %+.2f deg: residual median %.3f deg (n %d) | runner-up "
                    "s %+d k %d residual %.3f", bestS, bestK, bestC * kDeg, bestMed, bestN, secS, secK,
                    secMed < 1e8f ? secMed : -1.0f);
    }
    // --- pairing by value + the correction the compositor would make ---
    static float aAll[kRpMax], aTurn[kRpMax], aFast[kRpMax], trail[kRpMax], rate[kRpMax], pit[kRpMax], mres[kRpMax];
    int nAll = 0, nTurn = 0, nFast = 0, nTrail = 0, nRate = 0, nPit = 0, nRes = 0;
    int found = 0, notFound = 0, ambiguous = 0, ringless = 0;
    int kHist[kRpRing] = {}, entryAge[5] = {}, capAge[3] = {}, perSubmit[4] = {};
    int echoPos[kRpCapHist] = {}, echoNone = 0, echoMulti = 0, nDeclared = 0;
    float eyeDiffMax = 0.0f, prevPsiD = 0.0f;
    double prevT = 0.0;
    bool havePrev = false;
    for (int i = 0; i < g_rpN; ++i) {
        const RpSample& S = g_rpS[i];
        const RpCapture& c = S.cap;
        eyeDiffMax = fmaxf(eyeDiffMax, S.eyeYawDiff);
        if (S.echoPos < 0) { ++echoNone; } else { ++echoPos[S.echoPos]; }
        if (S.echoN > 1) { ++echoMulti; }
        if (S.declared) { ++nDeclared; }
        ++perSubmit[S.newCaps < 3 ? (S.newCaps < 0 ? 0 : S.newCaps) : 3];
        const LONG ca = S.frame - c.frame;
        ++capAge[ca <= 0 ? 0 : (ca == 1 ? 1 : 2)];
        const float psiD = RpYaw(S.dq);
        float r = 0.0f;
        if (havePrev && S.t > prevT) { r = VpWrap(psiD - prevPsiD) * kDeg / static_cast<float>(S.t - prevT); }
        prevPsiD = psiD; prevT = S.t; havePrev = true;
        if (bestK < 0 || c.n == 0) { ++ringless; continue; }
        int bj = -1;
        float br = 1e9f;
        for (int j = 0; j < c.n; ++j) {
            const float rr = fabsf(VpWrap(c.cy - bestS * c.ring[j].after / kDeg - bestC)) * kDeg;
            if (rr < br) { br = rr; bj = j; }
        }
        if (bj < 0 || br > kRpTolDeg) { ++notFound; continue; }
        ++found;
        mres[nRes++] = br;
        for (int j = 0; j < c.n; ++j) {
            if (j == bj) { continue; }
            const float rr = fabsf(VpWrap(c.cy - bestS * c.ring[j].after / kDeg - bestC)) * kDeg;
            if (rr <= kRpTolDeg && fabsf(c.ring[j].h - c.ring[bj].h) > 0.05f) { ++ambiguous; break; }
        }
        const LONG ea = S.frame - c.ring[bj].frame;
        ++entryAge[ea <= 0 ? 0 : (ea >= 4 ? 4 : ea)];
        // Stage 2: the correction is quoted only for frames whose capture the echo identified (Stage 1 used the latest
        // capture, which is the NEXT frame's camera on ~1/3 of submits).
        if (S.echoPos < 0) { continue; }
        // rendered XR orientation = the paired stripped yaw * the pitch/roll camhook applied (spec R2: sy (x) pr_used)
        float hy[4], hinv[4], pr[4], qr[4];
        YawOnly(c.hq, 0, hy);
        hinv[0] = -hy[0]; hinv[1] = -hy[1]; hinv[2] = -hy[2]; hinv[3] = hy[3];
        QuatMul(hinv, c.hq, pr);
        QuatMul(c.ring[bj].sy, pr, qr);
        const float corrY = VpWrap(psiD - RpYaw(qr)) * kDeg;   // declared - rendered
        const float corrP = (RpPitch(S.dq) - RpPitch(qr)) * kDeg;
        aAll[nAll++] = fabsf(corrY);
        pit[nPit++] = fabsf(corrP);
        if (fabsf(r) >= kRpTurnDegS) {
            ++kHist[bj];
            aTurn[nTurn++] = fabsf(corrY);
            trail[nTrail++] = r > 0.0f ? corrY : -corrY;
            rate[nRate++] = fabsf(r);
            if (fabsf(r) >= kRpFastDegS) { aFast[nFast++] = fabsf(corrY); }
        }
    }
    const int paired = found + notFound;
    const float maxAll = nAll ? *std::max_element(aAll, aAll + nAll) : 0.0f;
    const float maxTurn = nTurn ? *std::max_element(aTurn, aTurn + nTurn) : 0.0f;
    const float maxFast = nFast ? *std::max_element(aFast, aFast + nFast) : 0.0f;
    const float maxPit = nPit ? *std::max_element(pit, pit + nPit) : 0.0f;
    const float maxRes = nRes ? *std::max_element(mres, mres + nRes) : 0.0f;
    // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
    LOG("camhook: [RENDPOSE] window submits %lld, samples %d (refused %lld, poses-invalid %lld, no-capture %lld, ring-empty "
        "%lld, capture read torn %lld, ring snapshot torn %lld) | %s | PAIRS found %d of %d (%.1f%%), not found %d, "
        "ambiguous %d, no-fit/ringless %d | match residual median %.3f max %.3f deg | matched ring pos while turning "
        "(0=newest) %d %d %d %d %d %d %d %d | entry age fr 0/1/2/3/4+ %d %d %d %d %d | capture age fr 0/1/2+ %d %d %d | "
        "captures per submit 0/1/2/3+ %d %d %d %d | PAIRED (tag, INV-326) capture pos 0/1/2/3 %d %d %d %d, unpaired %d, echo "
        "multi %d | declared %d | CORRECTION (tag-paired frames only) yaw all n %d median %.2f max %.2f deg; TURNING (>= %.0f "
        "deg/s) n %d median %.2f p90 %.2f max %.2f, trail median %+.2f deg, rate median %.0f deg/s; FAST (>= %.0f deg/s) n "
        "%d median %.2f p90 %.2f max %.2f | pitch median %.2f max %.2f deg | eye0-eye1 yaw max %.3f deg",
        g_rpSubmits, g_rpN, g_rpRefused, g_rpInvalid, g_rpNoCap, g_rpRingEmpty, g_rpReadTorn,
        static_cast<long long>(g_rpCapTorn), fitTxt, found, paired, paired ? 100.0 * found / paired : 0.0, notFound,
        ambiguous, ringless, RpPct(mres, nRes, 0.5f), maxRes, kHist[0], kHist[1], kHist[2], kHist[3], kHist[4], kHist[5],
        kHist[6], kHist[7], entryAge[0], entryAge[1], entryAge[2], entryAge[3], entryAge[4], capAge[0], capAge[1],
        capAge[2], perSubmit[0], perSubmit[1], perSubmit[2], perSubmit[3], echoPos[0], echoPos[1], echoPos[2],
        echoPos[3], echoNone, echoMulti, nDeclared, nAll, RpPct(aAll, nAll, 0.5f), maxAll,
        kRpTurnDegS, nTurn, RpPct(aTurn, nTurn, 0.5f), RpPct(aTurn, nTurn, 0.9f), maxTurn, RpPct(trail, nTrail, 0.5f),
        RpPct(rate, nRate, 0.5f), kRpFastDegS, nFast, RpPct(aFast, nFast, 0.5f), RpPct(aFast, nFast, 0.9f), maxFast,
        RpPct(pit, nPit, 0.5f), maxPit, eyeDiffMax);
}

void RpWindowTick()
{
    const ULONGLONG now = GetTickCount64();
    if (g_rpWinStart == 0) { g_rpWinStart = now; }
    if (now - g_rpWinStart < 1000) { return; }
    RpReport();
    RtReport();
    g_rpN = 0;
    g_rpSubmits = g_rpRefused = g_rpInvalid = g_rpNoCap = g_rpReadTorn = g_rpRingEmpty = 0;
    InterlockedExchange64(&g_rpCapTorn, 0);
    g_rpWinStart = now;
}

// [RENDDECL] - Stage 2's own 5 s line, while declare_rendered_orientation is ON (independent of diag_render_pose).
void RdTick()
{
    const ULONGLONG now = GetTickCount64();
    if (g_rdWinStart == 0) { g_rdWinStart = now; }
    if (now - g_rdWinStart < 5000) { return; }
    // [PROBE: RENDDECL] gate=declare_rendered_orientation  site=HEADSET  log-only=no
    LOG_PERIODIC("camhook: [RENDDECL] 5 s: calls %lld | DECLARED %lld (%.1f%%) | fell back to today's pose: refused %lld, not-ready "
        "%lld (poses invalid / no capture / torn / ring empty), no tagged capture (own record missing or capture out of "
        "history) %lld, ring < 2 entries %lld, k=1 "
        "value check > %.2f deg %lld (worst residual %.3f deg), NaN %lld | echo multi-match %lld | declared from TAGGED "
        "capture pos 0/1/2/3 %lld %lld %lld %lld | declared with a VALUE-MATCHED ring entry instead of k=1, pos 0/2/3/4+ %lld "
        "%lld %lld %lld | |current - declared| yaw mean %.2f max %.2f deg",
        g_rdCalls, g_rdDeclared, g_rdCalls ? 100.0 * g_rdDeclared / g_rdCalls : 0.0, g_rdRefused, g_rdNotReady,
        g_rdNoEcho, g_rdShortRing, kRdTolDeg, g_rdK1Fail, g_rdMaxK1Res, g_rdNaN, g_rdMulti, g_rdPos[0], g_rdPos[1],
        g_rdPos[2], g_rdPos[3], g_rdAltK[0], g_rdAltK[2], g_rdAltK[3],
        g_rdAltK[4] + g_rdAltK[5] + g_rdAltK[6] + g_rdAltK[7], g_rdDeclared ? g_rdSumAbs / g_rdDeclared : 0.0, g_rdMaxAbs);
    for (int i = 0; i < kRpRing; ++i) { g_rdAltK[i] = 0; }
    g_rdCalls = g_rdDeclared = g_rdRefused = g_rdNotReady = g_rdNoEcho = g_rdMulti = g_rdShortRing = g_rdK1Fail = 0;
    g_rdNaN = 0;
    g_rdSumAbs = 0.0;
    g_rdMaxAbs = g_rdMaxK1Res = 0.0f;
    for (int i = 0; i < kRpCapHist; ++i) { g_rdPos[i] = 0; }
    g_rdWinStart = now;
}

}  // namespace

// ===== afwcam - the matrices afw_rebuild's per-image gate needs (AFW step 4b). =====
// The bootstrap pushes each GBuffer-pass view-projection (dlvr_AfwPushVp) into a 16-slot ring; at every submit the number
// of pushes since the previous submit is noted, and the gate (below) takes the newest push when exactly one landed - the
// tonemap draw and ovr_SubmitFrame are both issued by rd3d11's command executor (INV-325), so that push is this image's.
// The LOG-ONLY [AFWCAM] checks built on the same data (diag_afw_cam, INV-383/384: BASIS, CAMERA, HEADPOS, EYEGEOM,
// TURNSTEP) were REMOVED 2026-10-07, audit section 4 group 1.
namespace afwcam {
constexpr int kRing = 16;
struct Slot { volatile LONG64 tag; float vp[16]; DWORD tid; };   // tag = afwalt::g_frame at the push, written LAST (-1 = writing)
Slot          g_ring[kRing];
volatile LONG64 g_pushes = 0;
bool          g_pushSaid = false;       // bootstrap render thread only
// submit-order key (submit thread only)
LONG64    g_pushAtSubmit = 0, g_since = -1;   // g_pushes at the previous submit; pushes since it (-1 = no previous submit)
bool      g_haveSubmit = false;

float Wrap180(float d) { while (d > 180.0f) { d -= 360.0f; } while (d <= -180.0f) { d += 360.0f; } return d; }

// Every submit (declared or not): how many pushes landed since the previous submit.
void NoteSubmit()
{
    const LONG64 cur = g_pushes;
    g_since = g_haveSubmit ? cur - g_pushAtSubmit : -1;
    g_pushAtSubmit = cur;
    g_haveSubmit = true;
}

// Rows r_i = m[4i..4i+3] (row-major, clip_i = r_i . (p,1); row 2 = (0,0,0,near), U-8). The camera position zeroes rows
// 0, 1 and 3; the forward direction is row 3's own (w = depth along it). OFF-CENTRE (trace docs/traces/20261124_1700_afw_offcentre_stage1.md):
// it was row0 x row1, which leans by the frustum's off-centre (~13.6 deg at symmetric_fov 0 - every image refused, INV-612); centred, the two
// differ only by the jitter (~0.006 deg against the gate's 0.5).
bool GpuCam(const float m[16], float pos[3], float fwd[3])
{
    const float* a0 = m; const float* a1 = m + 4; const float* a3 = m + 12;
    auto cross = [](const float* u, const float* v, float* o) {
        o[0] = u[1] * v[2] - u[2] * v[1]; o[1] = u[2] * v[0] - u[0] * v[2]; o[2] = u[0] * v[1] - u[1] * v[0]; };
    float c13[3], c30[3], c01[3];
    cross(a1, a3, c13); cross(a3, a0, c30); cross(a0, a1, c01);
    const float det = a0[0] * c13[0] + a0[1] * c13[1] + a0[2] * c13[2];
    if (!(fabsf(det) > 1e-12f)) { return false; }
    for (int i = 0; i < 3; ++i) { pos[i] = -(a0[3] * c13[i] + a1[3] * c30[i] + a3[3] * c01[i]) / det; }
    float f[3] = { a3[0], a3[1], a3[2] };
    const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(len > 1e-12f)) { return false; }
    for (int i = 0; i < 3; ++i) { fwd[i] = f[i] / len; }
    return true;
}

// ===== THE REBUILD's PER-IMAGE GATE (afw_rebuild, AFW step 4b). =====
// INV-384's "the GPU camera is the captured camera" holds in play but NOT for every image (INV-386: 474 of 485 images
// failed it with the inventory open / the headset off). So it is tested on every image the rebuild wants to use, and
// the held eye must be the image right before this one, of the other eye, having passed the same test.
// 2026-10-18 (Jay: "Gate checks position + eye", docs/BETA-FINDINGS-2026-10-18.md §3 candidate A): the yaw test alone passed a
// STALE snapshot that kept the heading (INV-271 - both eyes handed one old matrix; seen in ordinary play on two testers'
// laptops). Two more refusals, both from afwgate_logic.h:
//   kGateStale   this image's matrix is the PREVIOUS image's, bit for bit (two eyes are never one matrix legitimately)
//   kGateEyePos  the GPU eye camera is not half the eye separation (+-10 mm) from the captured camera (INV-384: 0.0318 m for
//                a 63.6 mm IPD on 23,936 images, moving included); no usable separation = refused (fails closed)
// [LOADBEARING: AFW-GATE-STALE] the ONLY thing that keeps a stale-but-same-heading camera out of the AFW rebuild.
enum { kGateOk = 0, kGateNotPaired, kGateNoPush, kGateTorn, kGateCamera, kGateNoHeld, kGateStale, kGateEyePos, kGateCount };
int       g_gate = kGateNotPaired;
float     g_gateRes = 0.0f;
long long g_gatePrevFi = -10;
int       g_gatePrevEye = -1;
bool      g_gatePrevOk = false;
float     g_prevVp[16] = {};          // the previous gated image's GPU matrix (submit thread)
bool      g_havePrevVp = false;
volatile float g_eyeSep = 0.0f;       // this submit's eye separation (m), CamHookAfwNoteEyeSep
float     g_gatePosErr = 0.0f;        // |distance - sep/2| of the last image that reached the position test (m)

void GateNone() { g_gate = kGateNotPaired; g_gatePrevOk = false; }

void GateImage(long long frameIndex, int skip, const RpCapture& cap)
{
    int code = kGateOk;
    const int eye = (skip == 0 || skip == 1) ? 1 - skip : -1;
    if (eye < 0) {
        code = kGateNotPaired;
    } else if (g_since != 1) {
        code = kGateNoPush;
    } else {
        float vp[16];
        Slot& s = g_ring[static_cast<int>((g_pushAtSubmit - 1) % kRing)];
        const LONG64 t0 = s.tag;
        memcpy(vp, s.vp, sizeof(vp));
        MemoryBarrier();
        float gp[3], gf[3];
        if (t0 < 0 || s.tag != t0 || g_pushes != g_pushAtSubmit) {
            code = kGateTorn;
        } else {
            const bool stale = g_havePrevVp && ::dlvr::afwgate::SameMatrix(vp, g_prevVp);
            memcpy(g_prevVp, vp, sizeof(g_prevVp));
            g_havePrevVp = true;
            if (stale) {
                code = kGateStale;
            } else if (!GpuCam(vp, gp, gf)) {
                code = kGateCamera;
            } else {
                // INV-384: gpu heading - captured camera yaw = 180.00 deg in play; 0.5 deg is INV-384's own counting limit.
                g_gateRes = fabsf(Wrap180(atan2f(gf[0], gf[2]) * 57.2957795f - cap.cy * 57.2957795f - 180.0f));
                if (!(g_gateRes <= 0.5f)) {
                    code = kGateCamera;
                } else {
                    const float sep = g_eyeSep;
                    const float dx = gp[0] - cap.pos[0], dy = gp[1] - cap.pos[1], dz = gp[2] - cap.pos[2];
                    g_gatePosErr = fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - 0.5f * sep);
                    if (!::dlvr::afwgate::EyeOffsetOk(gp, cap.pos, sep)) { code = kGateEyePos; }
                }
            }
        }
    }
    const bool imgOk = code == kGateOk;
    if (imgOk && !(g_gatePrevOk && frameIndex == g_gatePrevFi + 1 && g_gatePrevEye == 1 - eye)) { code = kGateNoHeld; }
    g_gatePrevOk = imgOk; g_gatePrevFi = frameIndex; g_gatePrevEye = eye;
    g_gate = code;
}

}  // namespace afwcam

void CamHookAfwNoteEyeSep(float sepM)
{
    afwcam::g_eyeSep = sepM;
}

int CamHookAfwRebuildGate(float* residualDeg)
{
    if (residualDeg) { *residualDeg = afwcam::g_gateRes; }
    return afwcam::g_gate;
}

const char* CamHookAfwRebuildGateName(int code)
{
    static const char* const k[afwcam::kGateCount] = {
        "ok", "not paired / not declared", "not exactly one matrix since the previous submit", "matrix torn",
        "GPU camera != captured camera (> 0.5 deg)", "the previous image is not the other eye's, right before",
        "STALE camera (the previous image's matrix, bit for bit)",
        "GPU eye camera not half the eye separation (+-10 mm) from the captured camera" };
    return (code >= 0 && code < afwcam::kGateCount) ? k[code] : "?";
}

// Called by the bootstrap (dlvr_AfwPushVp) at each GBuffer-pass view-projection snapshot, on the render thread.
void CamHookAfwPushVp(const float vp[16])
{
    if (vp) { InterlockedExchange(&afwalt::g_netVpEver, 1); }   // [AFWNET]: before the return below, so a hold sees one arrive
    if (!AfwRebuild || !vp) { return; }   // the rebuild's gate is the only reader
    const LONG64 n = afwcam::g_pushes;
    afwcam::Slot& s = afwcam::g_ring[static_cast<int>(n % afwcam::kRing)];
    InterlockedExchange64(&s.tag, -1);
    memcpy(s.vp, vp, sizeof(s.vp));
    s.tid = GetCurrentThreadId();
    MemoryBarrier();
    InterlockedExchange64(&s.tag, afwalt::g_frame);
    afwcam::g_pushes = n + 1;
    if (!afwcam::g_pushSaid) {
        afwcam::g_pushSaid = true;
        // [PROBE: AFWCAM] gate=afw_rebuild (the gate's matrix sink - first push only)  site=HEADSET  log-only=yes
        LOG("[AFWCAM] FIRST GPU view-projection pushed by the bootstrap (logged unconditionally): afwFrame %lld, thread %lu, "
            "row 2 (%.5f %.5f %.5f %.5f)", static_cast<long long>(afwalt::g_frame), GetCurrentThreadId(), vp[8], vp[9],
            vp[10], vp[11]);
    }
}

bool CamHookRendPoseWanted()
{
    return DiagRenderPose || DeclareRenderedOrientation;
}

// AFW (afw_held_eye_pose): the eye the image just passed to CamHookRendPoseSubmit did NOT render this frame, from its own
// capture-paired RENDTAG record. 0/1 = that eye is HELD (shows its previous image); -1 = both eyes rendered; -2 = unknown
// (no own record / not paired this submit). Valid only right after CamHookRendPoseSubmit, on the submit thread.
int CamHookAfwSubmitSkipEye()
{
    return g_afwSubmitSkip;
}

int CamHookRendDeclStatus(const char** why)
{
    if (!DeclareRenderedOrientation) {
        if (why) { *why = "off"; }
        return 0;
    }
    const ULONGLONG last = g_rdLastOkMs;
    if (last != 0 && GetTickCount64() - last < 1000) {
        if (why) { *why = nullptr; }
        return 1;
    }
    if (why) { *why = g_rdLastWhy; }
    return -1;
}

void CamHookRendPosePush(float afterDeg, float hDeg, const float sy[4])
{
    if (!CamHookRendPoseWanted()) {
        return;
    }
    InterlockedIncrement(&g_rpRingSeq);   // odd = writing
    MemoryBarrier();
    const LONG64 c = g_rpRingCount;
    RpEntry& e = g_rpRing[static_cast<int>(c % kRpRing)];
    e.after = afterDeg;
    e.h = hDeg;
    for (int i = 0; i < 4; ++i) { e.sy[i] = sy[i]; }
    e.frame = g_rpFrame;
    g_rpRingCount = c + 1;
    MemoryBarrier();
    InterlockedIncrement(&g_rpRingSeq);   // even = stable
    if (!g_rpPushSaid) {
        g_rpPushSaid = true;
        // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
        LOG("camhook: [RENDPOSE] FIRST RING PUSH (logged unconditionally) +4460 after the add %.2f deg, head yaw added "
            "%+.2f deg, stripped yaw q (%.4f %.4f %.4f %.4f), frame %ld",
            afterDeg, hDeg, sy[0], sy[1], sy[2], sy[3], static_cast<long>(e.frame));
    }
}

void CamHookRendTagRecord(long long frameIndex)
{
    if (!CamHookRendPoseWanted()) {   // Stage 2b: the declaration pairs through these records too (INV-326)
        return;
    }
    static LARGE_INTEGER qpf = {};
    if (qpf.QuadPart == 0) { QueryPerformanceFrequency(&qpf); }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const DWORD me = GetCurrentThreadId();
    InterlockedExchange(&g_rtRecTid, static_cast<LONG>(me));
    RpCapture caps[kRpCapHist];
    int n = 0;
    LONG64 count = 0;
    RtRec r = { frameIndex, 0, false, 0.0f, static_cast<int>(afwalt::g_skipEyeNow), afwalt::g_frame };
    if (!RpReadCaptures(caps, n, count) || n == 0) {
        InterlockedIncrement64(&g_rtNoCap);
    } else {
        const LONG64 since = count - g_rtLastCount;
        g_rtLastCount = count;
        InterlockedIncrement64(&g_rtSince[since < 0 ? 0 : (since > 3 ? 3 : since)]);
        r.capSeq = caps[0].seq;
        r.sameThread = caps[0].tid == me;
        InterlockedExchange(&g_rtCapTidSeen, static_cast<LONG>(caps[0].tid));
        InterlockedIncrement64(r.sameThread ? &g_rtSame : &g_rtOther);
        const double us = qpf.QuadPart > 0
            ? 1e6 * static_cast<double>(now.QuadPart - caps[0].qpc) / static_cast<double>(qpf.QuadPart) : 0.0;
        r.ageUs = static_cast<float>(us);
        InterlockedExchangeAdd64(&g_rtAgeSumUs, static_cast<LONG64>(us));
        if (static_cast<LONG>(us) > g_rtAgeMaxUs) { InterlockedExchange(&g_rtAgeMaxUs, static_cast<LONG>(us)); }
    }
    const LONG64 p = g_rtPushed;
    g_rtFifo[static_cast<int>(p % kRtFifo)] = r;
    MemoryBarrier();
    g_rtPushed = p + 1;
    InterlockedIncrement64(&g_rtRecCalls);
    if (!g_rtRecSaid) {
        g_rtRecSaid = true;
        // [PROBE: RENDTAG] gate=diag_render_pose  site=HEADSET  log-only=yes
        LOG("camhook: [RENDTAG] FIRST RECORD (logged unconditionally) pose recorder on thread %lu, frameIndex %lld | newest "
            "capture #%lld on thread %lu (%s), %.0f us old, %lld capture(s) so far",
            static_cast<unsigned long>(me), frameIndex, r.capSeq, n ? static_cast<unsigned long>(caps[0].tid) : 0ul,
            n ? (r.sameThread ? "SAME thread" : "a DIFFERENT thread") : "no capture yet", r.ageUs, count);
    }
}

bool CamHookRendPoseSubmit(bool posesValid, const float eye0[4], const float eye1[4], const float echo[4], bool haveEcho,
                           bool useRenderPose, float outRendered[4], long long frameIndex)
{
    const bool diag = DiagRenderPose;
    const bool decl = DeclareRenderedOrientation;
    g_afwSubmitSkip = -2;   // set below ONLY from this submit's own, capture-paired record
    if (!g_rdPrevKnown || decl != g_rdPrevOn) {
        if (g_rdPrevKnown || decl) {
            // [PROBE: RENDDECL] gate=declare_rendered_orientation  site=HEADSET  log-only=no
            LOG("camhook: [RENDDECL] declare_rendered_orientation SWITCHED %s (live) - %s", decl ? "ON" : "OFF",
                decl ? "each frame is declared with the orientation it was RENDERED with when every pairing check passes, "
                       "otherwise with today's located pose (counted by reason, one summary line per 5 s)"
                     : "every frame is declared with the current located pose, as before Stage 2");
        }
        g_rdPrevOn = decl;
        g_rdPrevKnown = true;
    }
    if (!diag && !decl) {
        return false;
    }
    const int inVeh = RdLocalPlayerInVehicle();   // THE BUGGY's DECLARATION (above g_rdVehSubs): read once per submit
    if (inVeh != 1) { g_rdVehHave = false; VehRecLeft(); }     // v2: the car's term is re-acquired on the next vehicle frame
    enum { kOk, kRefused, kInvalid, kTorn, kNoCap, kRingEmpty, kNoEcho, kShortRing, kK1, kNaN };
    int code = kOk;
    const char* refuse = !HeadYawDecoupleActive()
        ? "head_yaw_decouple is not active (latched at startup) - the ring is fed only when design D adds the head yaw "
          "into +4460, and with D off the view's yaw has no single head sample to pair with (spec section 3)"
        : (useRenderPose ? "use_render_pose=1 - the declared pose is then the game's echo, not the located view"
                         : nullptr);
    static RpCapture caps[kRpCapHist];   // submit thread only
    int nCaps = 0, newCaps = 0, echoPos = -1, echoN = 0;
    LONG64 count = 0;
    float qr[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float k1res = 0.0f;
    int   altK  = -1;   // ring position used instead of k = 1 (value-matched), -1 = k = 1 was used
    if (refuse) {
        code = kRefused;
    } else if (!posesValid) {
        code = kInvalid;
    } else if (!RpReadCaptures(caps, nCaps, count)) {
        code = kTorn;
    } else if (count == 0) {
        code = kNoCap;
    } else {
        newCaps = static_cast<int>(count - g_rpLastCapCount);
        g_rpLastCapCount = count;
        if (caps[0].n == 0) {
            code = kRingEmpty;
        }
        if (haveEcho) {
            for (int i = 0; i < nCaps; ++i) {
                if (RpQuatSame(echo, caps[i].hq, kRdEchoTol)) {
                    if (echoPos < 0) { echoPos = i; }
                    ++echoN;
                }
            }
        }
        if (!g_rdSaidEcho && caps[0].n > 0) {
            g_rdSaidEcho = true;
            // [PROBE: RENDDECL] gate=declare_rendered_orientation,diag_render_pose  site=HEADSET  log-only=yes
            LOG("camhook: [RENDDECL] FIRST ECHO CHECK (logged unconditionally) game RenderPose echo %s (%.6f %.6f %.6f %.6f) | "
                "capture hq newest-first: 0 (%.6f %.6f %.6f %.6f) 1 (%.6f %.6f %.6f %.6f) of %d | matched pos %d (%d match(es), "
                "tolerance %g per component)", haveEcho ? "" : "ABSENT", echo[0], echo[1], echo[2], echo[3], caps[0].hq[0],
                caps[0].hq[1], caps[0].hq[2], caps[0].hq[3], nCaps > 1 ? caps[1].hq[0] : 0.0f, nCaps > 1 ? caps[1].hq[1] : 0.0f,
                nCaps > 1 ? caps[1].hq[2] : 0.0f, nCaps > 1 ? caps[1].hq[3] : 0.0f, nCaps, echoPos, echoN, kRdEchoTol);
        }
    }

    // [RENDTAG] - the submit's OWN pose record (INV-326: submit frameIndex == the oldest pending record's, 10,709/10,709) names
    // the capture the image was drawn from. Popped on EVERY submit while wanted, so the queue stays aligned whatever `code` is.
    int tagPos = -1;
    LONG64 tagAfwFrame = -1;   // [AFWCAM]: the own record's afwalt::g_frame
    {
        ++g_rtSubs;
        const LONG64 pushed = g_rtPushed;
        MemoryBarrier();
        LONG64 pend = pushed - g_rtConsumed;
        ++g_rtPend[pend < 0 ? 0 : (pend > 3 ? 3 : pend)];
        if (pend > kRtFifo) {
            ++g_rtOverrun;
            g_rtConsumed = pushed - kRtFifo;
            pend = kRtFifo;
        }
        LARGE_INTEGER tq, tf;
        QueryPerformanceCounter(&tq);
        QueryPerformanceFrequency(&tf);
        const double tNow = static_cast<double>(tq.QuadPart) / static_cast<double>(tf.QuadPart);
        const float yawNow = RpYaw(eye0);
        float rate = 0.0f;
        if (g_rtHavePrev && tNow > g_rtPrevT) {
            rate = fabsf(VpWrap(yawNow - g_rtPrevYaw)) * 57.2957795f / static_cast<float>(tNow - g_rtPrevT);
        }
        g_rtPrevYaw = yawNow; g_rtPrevT = tNow; g_rtHavePrev = true;
        if (pend <= 0) {
            ++g_rtEmpty;
        } else {
            // compare the submit's frameIndex with EVERY pending record, oldest first.
            int firstMatch = -1, nMatch = 0;
            long long pendIdx[kRtFifo];
            bool pendCap[kRtFifo];
            for (int i = 0; i < pend; ++i) {
                const RtRec& q = g_rtFifo[static_cast<int>((g_rtConsumed + i) % kRtFifo)];
                pendIdx[i] = q.frameIndex;
                pendCap[i] = q.capSeq != 0;
                if (q.frameIndex == frameIndex) {
                    if (firstMatch < 0) { firstMatch = i; }
                    ++nMatch;
                }
            }
            const long long hd = frameIndex - pendIdx[0];
            ++g_rtHeadDiff[(hd >= -3 && hd <= 3) ? static_cast<int>(hd + 3) : 7];
            if (firstMatch < 0) { ++g_rtMatchNone; } else { ++g_rtMatchPos[firstMatch > 3 ? 3 : firstMatch]; }
            if (nMatch > 1) { ++g_rtMatchMulti; }
            if (diag && g_rtRawLines < kRtRawMax && pendCap[0]) {
                ++g_rtRawLines;
                char lst[160];
                int off = 0;
                for (int i = 0; i < pend && i < 8 && off < static_cast<int>(sizeof(lst)) - 24; ++i) {
                    off += _snprintf_s(lst + off, sizeof(lst) - off, _TRUNCATE, "%s%lld", i ? " " : "", pendIdx[i]);
                }
                // [PROBE: RENDTAG] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDTAG] RAW #%d (first %d in-world submits, logged unconditionally) submit frameIndex %lld | "
                    "pending records oldest-first: %s | first match pos %d of %d match(es)", g_rtRawLines, kRtRawMax,
                    frameIndex, lst, firstMatch, nMatch);
            }
            if (firstMatch > 0) {   // discard the records before the submit's own (counted)
                g_rtSkipped += firstMatch;
                g_rtConsumed += firstMatch;
            }
            const RtRec r = g_rtFifo[static_cast<int>(g_rtConsumed % kRtFifo)];
            ++g_rtConsumed;
            const bool own = r.frameIndex == frameIndex;
            if (own) {
                ++g_rtInOrder;
            } else {
                ++g_rtOutOfOrder;
                if (diag && !g_rtSaidOrder) {
                    g_rtSaidOrder = true;
                    // [PROBE: RENDTAG] gate=diag_render_pose  site=HEADSET  log-only=yes
                    LOG("camhook: [RENDTAG] FIRST ORDER MISMATCH (logged unconditionally) submit frameIndex %lld popped a "
                        "record for frameIndex %lld (%lld record(s) were pending) - one record per submit, in order, is "
                        "NOT holding", frameIndex, r.frameIndex, pend);
                }
            }
            if (r.capSeq == 0) {
                ++g_rtNoCapRec;
            } else if (own) {   // only the submit's OWN record identifies its image; anything else is not paired
                for (int i = 0; i < nCaps; ++i) {
                    if (caps[i].seq == r.capSeq) { tagPos = i; break; }
                }
                if (tagPos < 0) { ++g_rtNotInHist; } else { ++g_rtTagPos[tagPos]; }
                if (tagPos >= 0) { g_afwSubmitSkip = r.skipEye; tagAfwFrame = r.afwFrame; }   // AFW: the eye THIS image did not render
            }
            if (tagPos >= 0) {
                const bool turning = rate >= kRpTurnDegS;
                if (echoPos < 0) {
                    ++g_rtEchoNone;
                } else if (echoPos == tagPos) {
                    ++(turning ? g_rtAgreeTurn : g_rtAgreeStill);
                } else {
                    ++(turning ? g_rtDisTurn : g_rtDisStill);
                }
                const RpCapture& c = caps[tagPos];
                if (c.n >= 2) {
                    const float res = fabsf(VpWrap(c.cy + c.ring[1].after / 57.2957795f + 1.5707963f)) * 57.2957795f;
                    if (!(res <= kRdTolDeg)) {
                        ++g_rtK1Fail;
                    } else if (turning && g_rtCorrN < kRpMax) {
                        float hy[4], hinv[4], pr[4], q[4];
                        YawOnly(c.hq, 0, hy);
                        hinv[0] = -hy[0]; hinv[1] = -hy[1]; hinv[2] = -hy[2]; hinv[3] = hy[3];
                        QuatMul(hinv, c.hq, pr);
                        QuatMul(c.ring[1].sy, pr, q);
                        g_rtCorr[g_rtCorrN++] = fabsf(VpWrap(yawNow - RpYaw(q))) * 57.2957795f;
                    }
                } else {
                    ++g_rtK1Fail;
                }
            }
            if (diag && !g_rtSaidFirst) {
                g_rtSaidFirst = true;
                // [PROBE: RENDTAG] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDTAG] FIRST POP (logged unconditionally) submit frameIndex %lld, record frameIndex %lld, "
                    "tagged capture #%lld at history pos %d, echo pos %d, %lld record(s) were pending",
                    frameIndex, r.frameIndex, r.capSeq, tagPos, echoPos, pend);
            }
        }
    }

    // MAIN-MENU HEAD TURN (ui_panel_main_menu_turn v4, INV-537 add. 8). While the root is being given the head's yaw the
    // camera's heading sweeps with the head, and the player-look ring holds only what the game left behind (or nothing):
    // matching it by value declared 2-8 frames per 5 s with the WRONG yaw (run 3). So here the ring is never consulted. The
    // frame is declared with the yaw its own tagged capture says the root was given, times the pitch/roll camhook applied -
    // the in-game formula below with the root's yaw in place of the ring entry's. No tagged capture, or one taken without
    // the yaw -> today's located pose, counted.
    // A cutscene (INV-620 (c)) uses this same branch with its own root; its counts go to [CINYAW] instead of [UIPIN].
    const uintptr_t turnRoot = TurnRoot();
    const bool cinDecl = TurnRootIsCutscene(turnRoot);
    if (turnRoot != 0 && (code == kOk || code == kRingEmpty)) {
        if (tagPos < 0) {
            code = kNoEcho;
            InterlockedIncrement(cinDecl ? &g_cinDeclNoTag : &g_mmDeclNoTag);
        } else if (!caps[tagPos].mm) {
            code = kNoCap;
            InterlockedIncrement(cinDecl ? &g_cinDeclNoYaw : &g_mmDeclNoYaw);
        } else {
            const RpCapture& c = caps[tagPos];
            float hy[4], hinv[4], pr[4];
            YawOnly(c.hq, 0, hy);
            hinv[0] = -hy[0]; hinv[1] = -hy[1]; hinv[2] = -hy[2]; hinv[3] = hy[3];
            QuatMul(hinv, c.hq, pr);
            QuatMul(c.mmSy, pr, qr);
            const float len = sqrtf(qr[0] * qr[0] + qr[1] * qr[1] + qr[2] * qr[2] + qr[3] * qr[3]);
            if (!(len > 0.99f && len < 1.01f)) {
                code = kNaN;
            } else {
                for (int i = 0; i < 4; ++i) { qr[i] /= len; }
                code = kOk;
                if (cinDecl) {
                    InterlockedIncrement(&g_cinDeclOk);
                } else {
                    InterlockedIncrement(&g_mmDeclOk);
                    const float d = fabsf(VpWrap(RpYaw(eye0) - RpYaw(qr))) * 57.2957795f;
                    if (d > g_mmDeclMaxDeg) { g_mmDeclMaxDeg = d; }
                }
            }
        }
    } else
    // Stage 2b selection: the TAGGED capture (INV-326), never the echo (D-165: the echo picked the wrong capture ~99%).
    if (code == kOk) {
        if (tagPos < 0) {
            code = kNoEcho;   // counted as "no tagged capture" in [RENDDECL]
        } else if (caps[tagPos].n < 2) {
            code = kShortRing;
        } else {
            const RpCapture& c = caps[tagPos];
            // INV-323: camera yaw = -(+4460) - 90.00 deg  =>  cy + after + 90 deg == 0. The camera normally carries ring
            // position 1 (k = 1); on the frames it does not (the 90 Hz run: 13 in ~70 s, residual 0.21-4.33 deg - Jay's
            // momentary doubling), the entry it DOES carry is found by value among the ring, so the frame is still declared.
            // Only when no entry matches does the frame fall back to today's pose.
            k1res = fabsf(VpWrap(c.cy + c.ring[1].after / 57.2957795f + 1.5707963f)) * 57.2957795f;
            int use = 1;
            if (inVeh == 1) {
                // THE BUGGY's DECLARATION (above g_rdVehSubs). v1's k = 1 value check is still counted (the 5 s line compares);
                // v2 (RdVehPick, 2026-10-29) chooses the entry by value against the tracked car term.
                ++g_rdVehSubs;
                if (k1res <= kRdTolDeg) {
                    ++g_rdVehValue;
                } else {
                    ++g_rdVehK1;
                    g_rdVehResSum += k1res;
                    if (k1res > g_rdVehResMax) { g_rdVehResMax = k1res; }
                }
                use = RdVehPick(c);
                if (DiagRenderPose) { VehRecNote(c, use); }   // VEHREC: off -> one bool test
                if (use != 1) { altK = use; }
            } else if (!(k1res <= kRdTolDeg)) {
                use = -1;
                float best = 1e9f;
                for (int j = 0; j < c.n; ++j) {
                    const float rj = fabsf(VpWrap(c.cy + c.ring[j].after / 57.2957795f + 1.5707963f)) * 57.2957795f;
                    if (rj <= kRdTolDeg && rj < best) { best = rj; use = j; }
                }
                if (use >= 0) { altK = use; }
            }
            if (use < 0) {
                code = kK1;
            } else {
                const RpEntry& e = c.ring[use];
                float hy[4], hinv[4], pr[4];
                YawOnly(c.hq, 0, hy);
                hinv[0] = -hy[0]; hinv[1] = -hy[1]; hinv[2] = -hy[2]; hinv[3] = hy[3];
                QuatMul(hinv, c.hq, pr);
                QuatMul(e.sy, pr, qr);
                const float len = sqrtf(qr[0] * qr[0] + qr[1] * qr[1] + qr[2] * qr[2] + qr[3] * qr[3]);
                if (!(len > 0.99f && len < 1.01f)) {
                    code = kNaN;
                } else {
                    for (int i = 0; i < 4; ++i) { qr[i] /= len; }
                }
            }
        }
    }
    const bool ok = code == kOk;
    if (inVeh < 0) { ++g_rdVehUnread; }
    if (inVeh == 1 || (g_rdVehSaidFirst && g_rdVehSubs > 0)) {   // THE BUGGY's DECLARATION: first in-vehicle submit, then 5 s
        const ULONGLONG nowV = GetTickCount64();
        if (!g_rdVehSaidFirst || nowV >= g_rdVehNext) {
            // [PROBE: RENDDECL] gate=declare_rendered_orientation (in a vehicle; FIRST line unconditional)  site=HEADSET  log-only=yes
            LOG("camhook: [RENDDECL] VEHICLE %s: submits in a vehicle reaching the check %lld | v1's k = 1 value check (counted "
                "only) passed %lld, failed %lld (its residual mean %.2f max %.2f deg) | v2 DECLARED BY VALUE %lld (at k = 0 / 1 / 2 / 3+: "
                "%lld %lld %lld %lld; residual mean %.3f max %.3f deg), RE-ACQUIRED from k = 0 %lld | car term %.2f .. %.2f deg | "
                "JUDDER: the term's own change per frame max %.2f deg, frames over 0.25 deg %lld; with the head also moving "
                "OPPOSITE %lld / same way %lld, the term moving alone %lld | "
                "in-vehicle read failed %lld | this submit: in a vehicle %d, code %d (0 = declared)",
                g_rdVehSaidFirst ? "5 s" : "FIRST (logged unconditionally)", g_rdVehSubs, g_rdVehValue, g_rdVehK1,
                g_rdVehK1 ? g_rdVehResSum / static_cast<double>(g_rdVehK1) : 0.0, g_rdVehResMax, g_rdVehMatch, g_rdVehAt[0],
                g_rdVehAt[1], g_rdVehAt[2], g_rdVehAt[3], g_rdVehMatch ? g_rdVehMResSum / static_cast<double>(g_rdVehMatch) : 0.0,
                g_rdVehMResMax, g_rdVehReacq, g_rdVehOffSeen ? g_rdVehOffMin : 0.0f, g_rdVehOffSeen ? g_rdVehOffMax : 0.0f,
                g_rdVehDOffMax, g_rdVehDOffBig, g_rdVehOpp, g_rdVehSame, g_rdVehTermOnly, g_rdVehUnread, inVeh, code);
            g_rdVehSaidFirst = true;
            g_rdVehNext = nowV + 5000;
            g_rdVehSubs = g_rdVehValue = g_rdVehK1 = g_rdVehUnread = 0;
            g_rdVehResSum = 0.0;
            g_rdVehResMax = 0.0f;
            g_rdVehMatch = g_rdVehReacq = 0;
            g_rdVehAt[0] = g_rdVehAt[1] = g_rdVehAt[2] = g_rdVehAt[3] = 0;
            g_rdVehMResSum = 0.0;
            g_rdVehMResMax = 0.0f;
            g_rdVehOffSeen = false;
            g_rdVehDOffMax = 0.0f;
            g_rdVehDOffBig = g_rdVehOpp = g_rdVehSame = g_rdVehTermOnly = 0;
        }
    }
    if (AfwRebuild) { afwcam::NoteSubmit(); }   // every submit, so the gate's "pushes since the previous submit" is per submit
    if (AfwRebuild) {   // step 4b: this image's gate (read by ovr_SubmitFrame through CamHookAfwRebuildGate)
        if (ok && AfwAltEye && tagAfwFrame >= 0) { afwcam::GateImage(frameIndex, g_afwSubmitSkip, caps[tagPos]); }
        else { afwcam::GateNone(); }
    }

    // ---- the declaration (Stage 2) ----
    if (decl) {
        ++g_rdCalls;
        if (echoN > 1) { ++g_rdMulti; }
        switch (code) {
        case kOk:
            ++g_rdDeclared;
            ++g_rdPos[tagPos];
            if (altK >= 0) { ++g_rdAltK[altK < kRpRing ? altK : kRpRing - 1]; }
            g_rdLastOkMs = GetTickCount64();
            break;
        case kRefused:
            ++g_rdRefused;
            g_rdLastWhy = HeadYawDecoupleActive()
                ? "refused: use_render_pose is 1 - set it to 0"
                : "refused: needs head_yaw_decouple = 1 (restart the game after changing it)";
            break;
        case kNoEcho:
            ++g_rdNoEcho;
            g_rdLastWhy = "frames are not being paired - the pose-recorder hook is not live (see the pose-recorder line in the log)";
            break;
        case kShortRing:
            ++g_rdShortRing;
            g_rdLastWhy = "waiting for head-yaw records (the player update has not run yet)";
            break;
        case kK1:
            ++g_rdK1Fail;
            if (k1res > g_rdMaxK1Res) { g_rdMaxK1Res = k1res; }
            g_rdLastWhy = "the camera matched no head-yaw record this frame";
            break;
        case kNaN:
            ++g_rdNaN;
            g_rdLastWhy = "an invalid orientation was rejected";
            break;
        default:
            ++g_rdNotReady;
            g_rdLastWhy = "not ready: no headset pose or no camera capture yet (in a menu or loading?)";
            break;
        }
        if (code == kRefused) {
            static bool said = false;
            if (!said) {
                said = true;
                // [PROBE: RENDDECL] gate=declare_rendered_orientation  site=HEADSET  log-only=no
                LOG("camhook: [RENDDECL] REFUSED - %s. Every frame keeps today's located pose.", refuse);
            }
        }
        if (ok) {
            const float d = fabsf(VpWrap(RpYaw(eye0) - RpYaw(qr))) * 57.2957795f;
            g_rdSumAbs += d;
            if (d > g_rdMaxAbs) { g_rdMaxAbs = d; }
            for (int i = 0; i < 4; ++i) { outRendered[i] = qr[i]; }
            if (!g_rdSaidFirst) {
                g_rdSaidFirst = true;
                // [PROBE: RENDDECL] gate=declare_rendered_orientation  site=HEADSET  log-only=no
                LOG("camhook: [RENDDECL] FIRST DECLARATION (logged unconditionally) tagged capture pos %d (frameIndex %lld), k=1 "
                    "residual %.4f deg | current yaw %+.2f pitch %+.2f -> declared yaw %+.2f pitch %+.2f deg", tagPos, frameIndex,
                    k1res,
                    RpYaw(eye0) * 57.2958f, RpPitch(eye0) * 57.2958f, RpYaw(qr) * 57.2958f, RpPitch(qr) * 57.2958f);
            }
        }
        RdTick();
    }

    // ---- the Stage 1 instrument (diag_render_pose) ----
    if (diag) {
        ++g_rpSubmits;
        switch (code) {
        case kRefused:
            ++g_rpRefused;
            if (!g_rpSaid[0]) {
                g_rpSaid[0] = true;
                // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDPOSE] REFUSED - %s. NO VERDICT; counted per window as 'refused'.", refuse);
            }
            break;
        case kInvalid: ++g_rpInvalid; break;
        case kTorn:    ++g_rpReadTorn; break;
        case kNoCap:
            ++g_rpNoCap;
            if (!g_rpSaid[1]) {
                g_rpSaid[1] = true;
                // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDPOSE] no camera capture yet - needs a chosen yaw anchor (cam_auto, horizon_lock, "
                    "cam_keep_head and a levelled camera); counted per window as 'no-capture'");
            }
            break;
        case kRingEmpty:
            ++g_rpRingEmpty;
            if (!g_rpSaid[2]) {
                g_rpSaid[2] = true;
                // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDPOSE] ring EMPTY at capture - camyaw::Hook has not pushed: the camyaw detour is not "
                    "live (see the [CAMYAW] install line) or the local player update has not run; counted as 'ring-empty'");
            }
            break;
        default: {   // a sample: the TAGGED capture (INV-326) when there is one, else the latest (flagged as unpaired)
            const RpCapture& cap = caps[tagPos >= 0 ? tagPos : 0];
            LARGE_INTEGER qpc, qpf;
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&qpf);
            if (g_rpN < kRpMax) {
                RpSample& S = g_rpS[g_rpN++];
                S.cap = cap;
                for (int i = 0; i < 4; ++i) { S.dq[i] = eye0[i]; }
                S.eyeYawDiff = fabsf(VpWrap(RpYaw(eye0) - RpYaw(eye1))) * 57.2958f;
                S.t = static_cast<double>(qpc.QuadPart) / static_cast<double>(qpf.QuadPart);
                S.frame = g_rpFrame;
                S.newCaps = newCaps;
                S.echoPos = tagPos;   // Stage 2b: the PAIRED capture's position (tag), -1 = unpaired
                S.echoN = echoN;
                S.declared = decl && ok;
            }
            if (!g_rpSaid[3]) {
                g_rpSaid[3] = true;
                // [PROBE: RENDPOSE] gate=diag_render_pose  site=HEADSET  log-only=yes
                LOG("camhook: [RENDPOSE] FIRST SAMPLE (logged unconditionally) declared yaw %+.2f pitch %+.2f deg | engine "
                    "camera yaw %+.2f deg | ring %d entries, newest +4460 %.2f deg h %+.2f deg | capture frame %ld, submit "
                    "frame %ld | echo capture pos %d | decouple %d, use_render_pose %d. One [RENDPOSE] window line per second.",
                    RpYaw(eye0) * 57.2958f, RpPitch(eye0) * 57.2958f, cap.cy * 57.2958f, cap.n, cap.ring[0].after,
                    cap.ring[0].h, static_cast<long>(cap.frame), static_cast<long>(g_rpFrame), echoPos,
                    HeadYawDecoupleActive() ? 1 : 0, useRenderPose ? 1 : 0);
            }
            break;
        }
        }
        RpWindowTick();
    }
    return decl && ok;
}

// ---- every camera matrix camhook writes, newest first (for the main-menu latch; see the write site) ----
void CamHookWantWrittenViews(bool on) { g_wvWanted = on; }

long long CamHookWrittenViewCount() { return g_wvCount; }

void CamHookSetMainMenuTurnRoot(uintptr_t cameraObject)
{
    // The page switch catch's two engine addresses, once (submit thread). Set BEFORE the root, so the copy hook never sees a
    // non-zero root with unresolved addresses.
    if (cameraObject && !g_mmEngView) {
        if (const HMODULE e = GetModuleHandleA("engine_x64_rwdi.dll")) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(e);
            g_mmEngCull = b + EngRva(0xAAABF0);
            MemoryBarrier();
            g_mmEngView = b + EngRva(0xAADDA8);
        }
    }
    g_mmTurnRoot = cameraObject;
}

void CamHookMainMenuViewSwitchStats(long* caught, long* refused, uintptr_t* cullAt, uintptr_t* viewAt)
{
    *caught  = InterlockedExchange(&g_mmViewCaught, 0);
    *refused = InterlockedExchange(&g_mmViewRefused, 0);
    *cullAt  = g_mmEngCull;
    *viewAt  = g_mmEngView;
}

// ---- INV-620 (c): cutscene head turn ----
bool CamHookCutsceneActive() { return g_cinActive != 0; }   // camhook.h (plan 2026-11-20 Task 5)

void CamHookSetCutsceneTurn(bool active, float y0RawRad)
{
    LONG bits = 0;
    memcpy(&bits, &y0RawRad, sizeof(bits));
    InterlockedExchange(&g_cinY0Bits, bits);
    if (!active) { InterlockedExchange(&g_cinAnimPitchOk, 0); }   // ADDENDUM 2: the next cutscene latches its own pitch
    InterlockedExchange(&g_cinActive, active ? 1 : 0);
}

void CamHookCutsceneTick()
{
    // Submit thread, once per submit. The root = the cached render camera of the scene view the engine last set up (engine+AADDA8,
    // view+0x90398) - the same object uipin.cpp names for the main menu (INV-537 add. 3 / D-210). COST: off - one load; on - two
    // guarded loads, plus one LOG per second while a cutscene runs (count-capped).
    static constexpr uintptr_t kSceneView = 0xAADDA8;
    static constexpr size_t    kCachedCam = 0x90398;
    static ULONGLONG s_next = 0;
    static long s_lines = 0, s_noRoot = 0, s_secs = 0;
    static bool s_was = false;
    const bool on = g_cinActive != 0;
    uintptr_t root = 0;
    if (on) {
        static uintptr_t s_eng = 0;
        if (!s_eng) { s_eng = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll")); }
        __try {
            const uintptr_t view = s_eng ? *reinterpret_cast<const uintptr_t*>(s_eng + DLVR_CIN_EN_RVA(kSceneView)) : 0;
            root = view ? *reinterpret_cast<const uintptr_t*>(view + kCachedCam) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { root = 0; }
        if (!root) { ++s_noRoot; }
    }
    g_cinRoot = root;
    const ULONGLONG now = GetTickCount64();
    // BUGGY STEADY COCKPIT (A, see kVehCockpit): the same root while the local player is in a vehicle and neither a cutscene nor the
    // main menu owns it. COST: RdLocalPlayerInVehicle's four guarded loads + two more for the root, per submit; one LOG per 5 s in a vehicle.
    {
        static ULONGLONG s_vNext = 0;
        static bool s_vWas = false;
        static long s_vLines = 0, s_vNoRoot = 0;
        uintptr_t vroot = 0;
        const bool inVehNow = RdLocalPlayerInVehicle() == 1;   // the buggy lean needs it every submit, kVehCockpit or not
        InterlockedExchange(&g_localInVeh, inVehNow ? 1 : 0);
        if (!on && !g_mmTurnRoot && inVehNow) {
            static uintptr_t s_engV = 0;
            if (!s_engV) { s_engV = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine_x64_rwdi.dll")); }
            __try {
                const uintptr_t view = s_engV ? *reinterpret_cast<const uintptr_t*>(s_engV + DLVR_CIN_EN_RVA(kSceneView)) : 0;
                vroot = view ? *reinterpret_cast<const uintptr_t*>(view + kCachedCam) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { vroot = 0; }
            if (!vroot) { ++s_vNoRoot; }
        }
        g_vehRoot = kVehCockpit ? vroot : 0;   // the steady cockpit's users exactly as before (kVehCockpit off = 0)
        g_leanRoot = vroot;
        {   // the buggy lean's lines (submit thread): ON / OFF with the run totals, and the first applied copy
            static bool s_lWas = false;
            static LONG s_lSaidFirst = 0;
            const bool lon = vroot != 0;
            if (lon != s_lWas) {
                s_lWas = lon;
                // [PROBE: VEHLEAN] gate=none (always on)  site=HEADSET  log-only=yes
                LOG("camhook: [VEHLEAN] %s - root %p | root copies offset %lld, no fresh offset %lld (run totals) - the head's offset is "
                    "added to the vehicle view and the arm anchor while in a vehicle", lon ? "ON" : "OFF", reinterpret_cast<void*>(vroot),
                    static_cast<long long>(g_vlApplied), static_cast<long long>(g_vlStale));
            }
            if (lon && !s_lSaidFirst && g_vlApplied > 0) {
                s_lSaidFirst = 1;
                float d[3] = {};
                const bool fresh = VehLeanRead(d);
                // [PROBE: VEHLEAN] gate=none (always on)  site=HEADSET  log-only=yes
                LOG("camhook: [VEHLEAN] FIRST OFFSET APPLIED (logged unconditionally) - offset now (%+.3f %+.3f %+.3f) m world%s, root "
                    "copies offset %lld", d[0], d[1], d[2], fresh ? "" : " (not fresh at this read)", static_cast<long long>(g_vlApplied));
            }
        }
        const bool von = g_vehRoot != 0;
        if (von != s_vWas) {
            s_vWas = von;
            s_vNext = now + 5000;
            // [PROBE: VEHCOCKPIT] gate=none (always on)  site=HEADSET  log-only=yes
            LOG("camhook: [VEHCOCKPIT] %s - root %p (the scene view's cached render camera; the buggy camera's own term is cancelled "
                "there)", von ? "STEADY COCKPIT ON" : "STEADY COCKPIT OFF", reinterpret_cast<void*>(vroot));
        }
        if (von && now >= s_vNext && s_vLines < 2000) {
            s_vNext = now + 5000;
            ++s_vLines;
            const LONG cp = InterlockedExchange(&g_vcCopies, 0);
            const LONG va = InterlockedExchange(&g_vcValue, 0);
            const LONG ne = InterlockedExchange(&g_vcNearest, 0);
            const LONG k0 = InterlockedExchange(&g_vcK0, 0);
            const LONG nr = InterlockedExchange(&g_vcNoRing, 0);
            const LONG rq = InterlockedExchange(&g_vcReacq, 0);
            const LONG fo = InterlockedExchange(&g_vcForced, 0);
            const float mn = g_vcMin, mx = g_vcMax, la = g_vcLast, sm = g_vcStepMax;
            InterlockedExchange(&g_vcReset, 1);
            // [PROBE: VEHCOCKPIT] gate=none (always on)  site=HEADSET  log-only=yes
            LOG("camhook: [VEHCOCKPIT] 5 s #%ld | root %p | root copies cancelled %ld (levelled only as the root %ld) | term found by value "
                "%ld, nearest %ld, k0 %ld | no ring %ld, re-acquired %ld | term cancelled %.2f .. %.2f deg (last %.2f), largest frame step "
                "%.2f deg | no root %ld", s_vLines, reinterpret_cast<void*>(vroot), cp, fo, va, ne, k0, nr, rq, cp ? mn : 0.0f,
                cp ? mx : 0.0f, la, sm, s_vNoRoot);
            s_vNoRoot = 0;
        }
    }
    if (on != s_was) {
        float y0 = 0.0f;
        const LONG bits = g_cinY0Bits;
        memcpy(&y0, &bits, sizeof(y0));
        // [PROBE: CINYAW] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("camhook: [CINYAW] %s - root %p, start head yaw (raw OpenXR) %+.2f deg", on ? "CUTSCENE TURN ON" : "CUTSCENE TURN OFF",
            reinterpret_cast<void*>(root), y0 * 57.2957795f);
        s_was = on;
        s_next = now + 1000;
        s_secs = 0;
    }
    if (on && now >= s_next && s_lines < 600) {
        s_next = now + 1000;
        ++s_lines;
        ++s_secs;
        const LONG adds = InterlockedExchange(&g_cinAdds, 0);
        const LONG ok = InterlockedExchange(&g_cinDeclOk, 0);
        const LONG noTag = InterlockedExchange(&g_cinDeclNoTag, 0);
        const LONG noYaw = InterlockedExchange(&g_cinDeclNoYaw, 0);
        const LONG rel = InterlockedExchange(&g_cinRelFix, 0);
        const LONG pc = InterlockedExchange(&g_cinPitchCopies, 0);
        const LONG sr = InterlockedExchange(&g_cinSplitRefused, 0);
        // [PROBE: CINYAW] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("camhook: [CINYAW] 1 s #%ld | root %p | root copies given the head turn %ld (last %+.2f deg) | submits declared with the head "
            "yaw %ld, no tagged capture %ld, tagged capture without the turn %ld | mapping fixed on the root anchor %ld | no root %ld | "
            "cutscene pitch %+.2f deg (%s) kept on %ld copies, split refused (axes) %ld",
            s_secs, reinterpret_cast<void*>(root), adds, g_cinAddedDeg, ok, noTag, noYaw, rel, s_noRoot,
            g_cinAnimPitch * 57.2957795f, g_cinAnimPitchOk ? "latched" : "NOT latched", pc, sr);
        s_noRoot = 0;
    }
}

long CamHookMainMenuTurnForced() { return InterlockedExchange(&g_mmTurnForced, 0); }

void CamHookMainMenuDeclStats(long* declared, long* noTag, long* noYaw, float* maxDeg)
{
    *declared = InterlockedExchange(&g_mmDeclOk, 0);
    *noTag    = InterlockedExchange(&g_mmDeclNoTag, 0);
    *noYaw    = InterlockedExchange(&g_mmDeclNoYaw, 0);
    *maxDeg   = g_mmDeclMaxDeg;
    g_mmDeclMaxDeg = 0.0f;
}

void CamHookMainMenuTurnStats(long* hits, float* addedDeg, float* engineYawDeg)
{
    *hits = InterlockedExchange(&g_mmTurnHits, 0);
    *addedDeg = g_mmTurnAdded * 57.2957795f;
    *engineYawDeg = g_mmTurnEngineYaw * 57.2957795f;
}

// The newest matrix camhook wrote whose position is within `tolM` of `pos`. False = none in the ring (nothing written
// since it was switched on, or none near that position). *ageMsOut = how old the one returned is.
bool CamHookGetWrittenViewNear(const float pos[3], float tolM, float out12[12], unsigned* ageMsOut, uintptr_t excludeObj)
{
    const ULONGLONG now = GetTickCount64();
    const LONG head = g_wvHead;
    for (int back = 0; back < kWvRing; ++back) {
        const WrittenView& w = g_wv[static_cast<unsigned>(head - back) & (kWvRing - 1)];
        const LONG s0 = w.seq;
        if (!s0 || (s0 & 1)) { continue; }
        float m[12];
        for (int i = 0; i < 12; ++i) { m[i] = w.m[i]; }
        const ULONGLONG t = w.ms;
        const uintptr_t o = w.obj;
        MemoryBarrier();
        if (w.seq != s0) { continue; }
        if (excludeObj && o == excludeObj) { continue; }
        const float dx = m[3] - pos[0], dy = m[7] - pos[1], dz = m[11] - pos[2];
        if (!(dx * dx + dy * dy + dz * dz <= tolM * tolM)) { continue; }
        for (int i = 0; i < 12; ++i) { out12[i] = m[i]; }
        if (ageMsOut) { *ageMsOut = static_cast<unsigned>(now > t ? now - t : 0); }
        return true;
    }
    return false;
}

bool CamHookGetRenderedViewMatrix(float out12[12], unsigned* ageMsOut)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_rvSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        float m[12];
        for (int i = 0; i < 12; ++i) { m[i] = g_rvMtx[i]; }
        const ULONGLONG t = g_rvMs;
        MemoryBarrier();
        if (g_rvSeq != s0) { continue; }
        if (!t) { return false; }
        const ULONGLONG now = GetTickCount64();
        const unsigned age = static_cast<unsigned>(now > t ? now - t : 0);
        if (ageMsOut) { *ageMsOut = age; }
        if (age > 150) { return false; }
        for (int i = 0; i < 12; ++i) { out12[i] = m[i]; }
        return true;
    }
    return false;
}

bool CamHookGetOpenXrToGameYaw(float& outRadians)
{
    if (!g_haveOpenxrToGameYaw) {
        return false;
    }
    outRadians = g_openxrToGameYaw;
    return true;
}

bool CamHookGetRawYawPair(float& y0, long long& t0, float& y1, long long& t1, long long& count)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_ryxSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        const float a0 = g_ryxY[0], a1 = g_ryxY[1];
        const LONG64 b0 = g_ryxT[0], b1 = g_ryxT[1], c = g_ryxCount;
        MemoryBarrier();
        if (g_ryxSeq != s0) { continue; }
        if (c < 2) { return false; }
        y0 = a0; t0 = b0; y1 = a1; t1 = b1; count = c;
        return true;
    }
    return false;
}

// SPEC-HEAD-YAW-DECOUPLE: the one yaw-only extraction, exported (see camhook.h).
void CamYawOnly(const float q[4], int upAxis, float out[4])
{
    YawOnly(q, upAxis, out);
}

// The RENDERED view's yaw, published by LevelCamera from the yaw anchor's FINAL rotation (after
// look-back, the decouple's C2 yaw and head pitch/roll) - i.e. what the player sees, not the
// engine's camera. Read by [HEADTURN] (spec section 5): with the decouple ON its slope against
// real HMD yaw must be ~+1; today (OFF) it is the engine's look yaw, ~0.83 (INV-256).
volatile float g_viewYaw     = 0.0f;
volatile LONG  g_haveViewYaw = 0;
volatile float     g_viewPitchApplied = 0.0f;   // [VIEWPOS]
volatile float     g_viewPos[3]       = { 0.0f, 0.0f, 0.0f };
volatile uintptr_t g_viewAnchorObj    = 0;

bool CamHookGetViewYaw(float& outRadians)
{
    if (!g_haveViewYaw) {
        return false;
    }
    outRadians = g_viewYaw;
    return true;
}

// Starts the Stage-0 skeleton dump worker once per process. Read-only and a
// cheap no-op while DiagBoneDump is off, so safe to start unconditionally.
// RIGGING STAGE 4 tick. Driven from dlvr_FilterScePad, NOT LevelCamera.
// LevelCamera runs inside the RENDER phase - after the engine has already
// computed this frame's camera - and `eyecamera` is element 8 of the very
// skeleton we mutate, so writing there perturbs a camera the frame was already
// drawn with. That is the leading explanation for the body detaching and the
// camera drifting in a circle. The scePad poll runs on a game thread during
// the UPDATE phase, which is where the engine itself moves these bones.
void RigStage4Tick()
{
    if (DiagRigStage4) {
        boneprobe::Stage4Pose(0);
    }
}

void StartBoneDumpOnce()
{
    if (InterlockedCompareExchange(&boneprobe::g_started, 1, 0) == 0) {
        CreateThread(nullptr, 0, boneprobe::Thread, nullptr, 0, nullptr);
    }
}

// >>> RE-ACQUIRE THE PLAYER'S MODEL AFTER A DEATH. INV-81. <<<
//
// THE BUG THIS EXISTS FOR, MEASURED 2026-09-17: the capture is ONE-SHOT. Jay
// died at 13:00:54 and the rig never solved again - 73 consecutive samples,
// 2 m 26 s, `IK solves` stuck on 209762 - while every arming line kept
// printing `[SHOULDER] latched`, `hooks - owner=OK ... tail=LIVE` and
// `WRITE TEST LIVE`. Death destroys the player's model object; SolveArmIk's
// five accessor reads then fail and it returns at boneblend.cpp:4631, and the
// probe said so in its own words: `[FROZEN] elem 13 - accessor read FAILED. A
// failed READ, not a frozen bone.`
//
// Returns true only if a fresh capture was actually started, so the caller can
// say something exactly once rather than every tick.
bool BoneProbeRecapture()
{
    // RATE LIMIT FIRST, because the caller is the per-tick solve path. Without
    // this, a permanent failure re-arms a hardware breakpoint across ~170
    // threads at tick rate - the cost rule in CLAUDE.md, and the reason this
    // check is not last.
    constexpr ULONGLONG kCooldownMs = 5000;
    static volatile LONG64 s_nextAllowedMs = 0;
    const LONGLONG now = static_cast<LONGLONG>(GetTickCount64());
    const LONGLONG next = s_nextAllowedMs;
    if (now < next) {
        return false;
    }
    if (InterlockedCompareExchange64(&s_nextAllowedMs, now + kCooldownMs, next) != next) {
        return false;   // another thread got here first
    }

    // Only when a capture has COMPLETED and the thread has actually exited.
    // g_done alone is not enough - see g_threadAlive.
    if (boneprobe::g_threadAlive) {
        return false;
    }
    if (InterlockedCompareExchange(&boneprobe::g_done, 0, 1) != 1) {
        return false;   // nothing captured yet; the first capture is still trying
    }

    boneprobe::g_model  = nullptr;
    boneprobe::g_entity = 0;
    LOG("boneprobe: RECAPTURE - the captured model stopped answering its accessors, which is "
        "what a player death does to it (INV-81). Dropping it and re-arming the capture. "
        "The rig does NOT solve until a new model is captured, so expect a gap: watch for "
        "`boneprobe: DONE` and then `IK solves` climbing again. If this line repeats every "
        "5 s the re-capture is not succeeding and the run is not measuring a rigged game.");

    // Let StartBoneDumpOnce create a thread again. Safe because g_threadAlive
    // is clear: the previous one has returned.
    InterlockedExchange(&boneprobe::g_started, 0);
    StartBoneDumpOnce();
    return true;
}

// rig_player_live_guard (U-49 / INV-365): forget the captured model and entity AT ONCE, without the cooldown and
// without re-arming anything. Every consumer already treats a null model as "nothing captured" (it is the state
// between RECAPTURE and `boneprobe: DONE`). The caller then asks BoneProbeRecapture() until it starts - that keeps
// the rate limit and the capture thread's own rules. Two plain stores; safe from any thread.
void BoneProbeDropModel()
{
    boneprobe::g_model  = nullptr;
    boneprobe::g_entity = 0;
}

// ---- Texture-streaming budget (2026-09-05; rewritten 2026-10-16 for F1, INV-580) ----
//
// HISTORY. Written 2026-09-05 believing r_device.scr's StreamMemPC(600) was the live budget and that no config or command-line
// route existed. Both were wrong (INV-580, static, OUR engine e9fbfc2c): the live budget is the engine's own clamp in
// sub_18023FDE0 - (adapter MB - reserves) / 2, held to [1050, 2000] MB at texture quality 5 - so a 16 GB card gets 2000; and
// CGame::Initialize parses -texStreamMemMB <n>. The old fixed 2048 was therefore a no-op, and it also overwrote any
// -texStreamMemMB about 2 s later (perf_run1: engine "set to 2000MB" 10:07:13.837, ours "set to 2048MB" 10:07:15.764).
//
// WHAT THE ENGINE ALREADY HAS. rd3d11's stream-in (sub_180045B80) recomputes the budget per stream-in as streaming-used + live
// free VRAM - but only while case 5 has never been given an explicit value (byte_18007BCF5) AND the vendor flag dword_18007BC44
// is 1, and the free-VRAM figure comes from NVAPI (nvapi_QueryInterface in sub_1800157E0 / sub_180014FE0; the AMD init path
// sets the flag to 2). NVIDIA-only, and switched off by the engine's own 2000 MB call. Dying Light The Beast ships the same
// stream-in path (TryToStreamIn) - comparison only.
//
// WHAT THIS DOES (stream_budget_mb):
//       0             leave the engine's own value alone (nothing sent)
//      -1 (compiled)  AUTO (shipped since 2026-11-13; 2026-09-05 .. 2026-11-13 the compiled value was a fixed 2048):
//                     every 5 s, budget = streaming used + (DXGI local budget - this process's usage) - the reserve
//                     (stream_budget_reserve_mb; -1 (compiled) = kAutoReservePct of the DXGI local budget, else that many MB),
//                     never below the engine's own value, at most kAutoCeilMB; re-sent only when it moves by kAutoStepMB or more.
//                     DXGI QueryVideoMemoryInfo is vendor-neutral (D-232: nothing NVIDIA-only).
//     > 0             that many MB
// Every value is sent with SRendererAPI slot 143 case 5 AND stored on the renderer with CRenderer slot 20 - the store
// -texStreamMemMB uses - so the engine's own video-settings apply (+23EF50) re-sends OUR value instead of its 2000 default.
// Both slots are checked against their expected RVAs before the first call; a mismatch refuses by name and sends nothing.
// Runs on the scePad poll (ApplyStreamingBudgetOnce is called every poll). Per-poll cost: one GetTickCount64 and a few compares;
// with diag_stream_budget on, also one 8-byte read of the used counter.

// Defined further down this file; used here to read the budget globals back.
bool SafeReadLocal(uintptr_t addr, void* out, size_t bytes);

typedef void (*StreamingCmdFn)(void* rendererApiThis, int cmd, long long value);
typedef void (*RendererStoreIntFn)(void* rendererThis, int value);

bool SafeCallStreamingCmd(void* api, int cmd, long long value)
{
    bool ok = false;
    __try {
        void** vtable = *reinterpret_cast<void***>(api);
        auto fn = reinterpret_cast<StreamingCmdFn>(vtable[0x478 / 8]);
        fn(api, cmd, value);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

namespace streambudget {

constexpr uintptr_t kEngineApiRva      = 0xA40218;   // SRendererAPI*
constexpr uintptr_t kEngineRendererRva = 0xA40220;   // CRenderer*
constexpr uintptr_t kRendererSlot20Rva = 0x739B60;   // CRenderer slot 20: *(this + 384) = value (what -texStreamMemMB uses)
constexpr uintptr_t kSlot143Rva        = 0x42ED0;    // rd3d11 SRendererAPI slot 143 (rstreaming.cpp dispatcher)
constexpr uintptr_t kBudgetRva         = 0x784B8;    // rd3d11 qword: budget in BYTES (case 5 stores MB << 20)
constexpr uintptr_t kUsedRva           = 0x7BD18;    // rd3d11 qword: bytes of streamed mip data resident (+= stream-in, -= evict)
constexpr uintptr_t kDeviceRva         = 0x7BC70;    // rd3d11 ppDevice (ID3D11Device*, written by D3D11CreateDevice)
constexpr int kAutoCeilMB = 16384;
constexpr int kAutoStepMB = 128;
// stream_budget_reserve_mb = -1: the reserve is this share of the card's DXGI local budget (Jay 2026-11-13: "20% of the card"). Scaled
// from Jay's tuned 3072 MB on a ~15.3 GB budget (INV-581: 15,280 MB) = ~3,000 MB there, ~1,600 MB on an 8 GB card. What DLSS / AFW /
// the VD encoder actually take on a smaller card is NOT measured.
constexpr long long kAutoReservePct = 20;
constexpr ULONGLONG kAutoEveryMs  = 5000;
constexpr ULONGLONG kProbeEveryMs = 30000;

int  g_sentMB = INT_MIN;          // last value sent to the engine, MB; INT_MIN = nothing sent
int  g_sentSetting = INT_MIN;     // the stream_budget_mb value that produced g_sentMB
int  g_engineMB = -1;             // the engine's own budget as first read, MB (AUTO never goes below it)
bool g_refused = false;           // a slot did not match its expected RVA - nothing is ever sent
bool g_renderer384 = true;        // CRenderer slot 20 matched (else only case 5 is sent)
ULONGLONG g_nextAuto = 0;
ULONGLONG g_nextProbe = 0;
bool g_probeSaidFirst = false;
unsigned long long g_lastUsed = 0;
bool g_haveLastUsed = false;
long g_changes = 0;
unsigned long long g_inBytes = 0, g_outBytes = 0;

bool ReadQword(HMODULE mod, uintptr_t rva, unsigned long long* out)
{
    return mod && SafeReadLocal(reinterpret_cast<uintptr_t>(mod) + rva, out, sizeof(*out));
}

// DXGI local-segment budget and this process's usage, bytes. Raw COM pointers only (the caller holds no RAII objects, so the
// SEH guard in QueryVramGuarded is legal). The device pointer is read from rd3d11 and NOT AddRef'd by us; QueryInterface does.
bool QueryVramRaw(ID3D11Device* dev, unsigned long long* budget, unsigned long long* usage)
{
    IDXGIDevice* dx = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) || !dx) {
        return false;
    }
    IDXGIAdapter* ad = nullptr;
    const HRESULT hr = dx->GetAdapter(&ad);
    dx->Release();
    if (FAILED(hr) || !ad) {
        return false;
    }
    IDXGIAdapter3* a3 = nullptr;
    const HRESULT hq = ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3));
    ad->Release();
    if (FAILED(hq) || !a3) {
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const HRESULT hm = a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    a3->Release();
    if (FAILED(hm)) {
        return false;
    }
    *budget = info.Budget;
    *usage = info.CurrentUsage;
    return true;
}

bool QueryVramGuarded(HMODULE rd3d11, unsigned long long* budget, unsigned long long* usage)
{
    ID3D11Device* dev = nullptr;
    if (!rd3d11 || !SafeReadLocal(reinterpret_cast<uintptr_t>(rd3d11) + kDeviceRva, &dev, sizeof(dev)) || !dev) {
        return false;
    }
    __try {
        return QueryVramRaw(dev, budget, usage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeRendererStore(void* renderer, int value)
{
    __try {
        void** vtable = *reinterpret_cast<void***>(renderer);
        reinterpret_cast<RendererStoreIntFn>(vtable[20])(renderer, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Checks both slots once against their expected bodies. false = refuse forever (another build of the engine / rd3d11).
bool Verify(HMODULE engine, HMODULE rd3d11, void* api, void* renderer)
{
    static int state = 0;   // 0 unchecked, 1 ok, 2 refused
    if (state) {
        return state == 1;
    }
    void* s143 = nullptr;
    void* s20 = nullptr;
    __try {
        s143 = (*reinterpret_cast<void***>(api))[143];
        if (renderer) {
            s20 = (*reinterpret_cast<void***>(renderer))[20];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const uintptr_t want143 = reinterpret_cast<uintptr_t>(rd3d11) + kSlot143Rva;
    const uintptr_t want20 = reinterpret_cast<uintptr_t>(engine) + EngRva(kRendererSlot20Rva);
    if (reinterpret_cast<uintptr_t>(s143) != want143) {
        LOG("streambudget: REFUSED - SRendererAPI slot 143 is %p, expected rd3d11+0x%llX (%p). Another rd3d11 build: no "
            "budget is ever sent (stream_budget_mb ignored).", s143, static_cast<unsigned long long>(kSlot143Rva),
            reinterpret_cast<void*>(want143));
        state = 2;
        g_refused = true;
        return false;
    }
    g_renderer384 = renderer && reinterpret_cast<uintptr_t>(s20) == want20;
    if (!g_renderer384) {
        LOG("streambudget: CRenderer slot 20 is %p, expected engine+0x%llX (%p) - case 5 only; a video-settings apply will "
            "put the engine's own default back.", s20, static_cast<unsigned long long>(kRendererSlot20Rva),
            reinterpret_cast<void*>(want20));
    }
    state = 1;
    return true;
}

// Sends `mb` (case 5 + the renderer store). Logs the engine's budget global either side - the proof the call landed.
void Send(HMODULE rd3d11, void* api, void* renderer, int mb, const char* why)
{
    unsigned long long before = 0, after = 0;
    ReadQword(rd3d11, kBudgetRva, &before);
    const bool stored = g_renderer384 && SafeRendererStore(renderer, mb);
    const bool ok = SafeCallStreamingCmd(api, 5, mb);
    ReadQword(rd3d11, kBudgetRva, &after);
    g_sentMB = mb;
    LOG("streambudget: %s -> %d MB (SRendererAPI slot 143 case 5 %s; CRenderer+384 %s). rd3d11+0x784b8 before %llu MB, "
        "after %llu MB%s", why, mb, ok ? "called" : "FAULTED", stored ? "stored" : "NOT stored",
        before >> 20, after >> 20,
        !ok ? "  -> NOT applied" : ((after >> 20) == static_cast<unsigned long long>(mb) ? "  -> CONFIRMED" :
                                    "  -> value did NOT change, the call did not land"));
}

int AutoMB(HMODULE rd3d11, unsigned long long* vb, unsigned long long* vu, unsigned long long* su, long long* reserveMB)
{
    *vb = *vu = *su = 0;
    *reserveMB = 0;
    if (!QueryVramGuarded(rd3d11, vb, vu)) {
        return -1;
    }
    ReadQword(rd3d11, kUsedRva, su);
    const long long freeMB = (static_cast<long long>(*vb) - static_cast<long long>(*vu)) >> 20;
    *reserveMB = (StreamBudgetReserveMB >= 0) ? static_cast<long long>(StreamBudgetReserveMB)
                                              : static_cast<long long>(*vb >> 20) * kAutoReservePct / 100;
    long long mb = static_cast<long long>(*su >> 20) + freeMB - *reserveMB;
    if (mb < g_engineMB) {
        mb = g_engineMB;
    }
    if (mb > kAutoCeilMB) {
        mb = kAutoCeilMB;
    }
    return static_cast<int>(mb);
}

// Returns true when it printed its 30 s line (the timer's summary rides the same tick).
bool Probe(HMODULE rd3d11, ULONGLONG now)
{
    if (!DiagStreamBudget) {
        return false;   // the caller checks too; kept here so the gate sits beside the emission
    }
    unsigned long long used = 0, budget = 0;
    if (!ReadQword(rd3d11, kUsedRva, &used) || !ReadQword(rd3d11, kBudgetRva, &budget)) {
        if (!g_probeSaidFirst) {
            g_probeSaidFirst = true;
            // [PROBE: STREAMBUDGET] gate=diag_stream_budget  site=FLAT+HEADSET  log-only=yes
            LOG("[STREAMBUDGET] NOT READ - rd3d11+0x7BD18 / +0x784B8 unreadable. NO VERDICT. diag_stream_budget=1");
        }
        return false;
    }
    if (g_haveLastUsed && used != g_lastUsed) {
        ++g_changes;
        if (used > g_lastUsed) {
            g_inBytes += used - g_lastUsed;
        } else {
            g_outBytes += g_lastUsed - used;
        }
    }
    g_lastUsed = used;
    g_haveLastUsed = true;
    if (g_probeSaidFirst && now < g_nextProbe) {
        return false;
    }
    g_nextProbe = now + kProbeEveryMs;
    unsigned long long vb = 0, vu = 0;
    const bool dxgi = QueryVramGuarded(GetModuleHandleA("rd3d11_x64_rwdi.dll"), &vb, &vu);
    char dx[160];
    if (dxgi) {
        _snprintf_s(dx, sizeof(dx), _TRUNCATE, "DXGI local budget %llu MB, this process using %llu MB, free %lld MB",
                    vb >> 20, vu >> 20, (static_cast<long long>(vb) - static_cast<long long>(vu)) >> 20);
    } else {
        _snprintf_s(dx, sizeof(dx), _TRUNCATE, "DXGI query FAILED (no adapter / no IDXGIAdapter3)");
    }
    // [PROBE: STREAMBUDGET] gate=diag_stream_budget  site=FLAT+HEADSET  log-only=yes
    LOG("[STREAMBUDGET] %s | engine streaming budget %llu MB, used %llu MB (%.1f%%), headroom %lld MB | used changed %ld "
        "time(s) since the last line: +%llu MB in, -%llu MB out | %s | stream_budget_mb=%d (last sent %s%d), reserve %d MB "
        "| diag_stream_budget=1",
        g_probeSaidFirst ? "30 s" : "FIRST (logged unconditionally)", budget >> 20, used >> 20,
        budget ? 100.0 * static_cast<double>(used) / static_cast<double>(budget) : 0.0,
        (static_cast<long long>(budget) - static_cast<long long>(used)) >> 20, g_changes, g_inBytes >> 20,
        g_outBytes >> 20, dx, StreamBudgetMB, g_sentMB == INT_MIN ? "nothing, " : "", g_sentMB == INT_MIN ? 0 : g_sentMB,
        StreamBudgetReserveMB);
    g_probeSaidFirst = true;
    g_changes = 0;
    g_inBytes = g_outBytes = 0;
    return true;
}

// ---- The render thread's streaming WORK, timed (2026-10-16, Jay: "Add the timer around the creation call"; INV-581 limit) ----
// The used counter above moves when a stream-in is REQUESTED; the expensive part - creating the new texture with every mip as
// initial data - happens later on the render thread and moves nothing. So time it directly with two pass-through detours on OUR
// rd3d11 (3804f0b1, bytes read from the deployed file 2026-10-16):
//   +43320 sub_180043320  the per-frame streaming update (SRendererAPI slot 62 "Streaming Update", from CRenderer::EndFrame),
//                         under its own critical section stru_1800793E8 - so at most one runs at a time; callers: +3AEA0 only
//   +42C30 sub_180042C30  ONE stream-in: CreateTexture (vtable +328) with every mip, old texture released; callers: +43320 (up to
//                         10 per update) and +45FC0 (from +44260 - another path, thread not established, counted separately)
// Stolen bytes are whole instructions with no RIP-relative operand (15: mov [rsp+10h],rdx / mov [rsp+8],rcx / push rbp/rbx/rsi/r12;
// 20: mov rax,rsp / mov [rax+10h],edx / mov [rax+8],rcx / push rbp / push r15 / lea rbp,[rax-0B8h]); InstallSingleGameDetour
// byte-verifies them and commits with every other thread suspended. Installed only while diag_stream_budget is on, once.
// Per-call cost: two QueryPerformanceCounter + a few interlocked adds (update: once a frame; creation: at most 10 a frame).
// NOTHING is logged on the render thread: slow updates go to a small ring the scePad poll prints.
namespace timer {

constexpr uintptr_t kUpdateRva = 0x43320;
constexpr uintptr_t kApplyRva  = 0x42C30;
const unsigned char kUpdateBytes[15] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x41, 0x54};
const unsigned char kApplyBytes[20] = {0x48, 0x8B, 0xC4, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08,
                                       0x55, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x48, 0xFF, 0xFF, 0xFF};
constexpr LONG64 kSlowUs = 4000;   // an update at or above this is listed individually
constexpr int kRing = 64;

typedef void(__fastcall* UpdateFn)(__int64, unsigned int*);
typedef __int64(__fastcall* ApplyFn)(void*, unsigned int, __int64);
UpdateFn g_trueUpdate = nullptr;
ApplyFn g_trueApply = nullptr;
volatile LONG g_state = 0;          // 0 not tried, 1 live, -1 refused
double g_usPerTick = 0.0;

volatile LONG   g_updTid = 0;       // thread inside the update right now (0 = none)
volatile LONG64 g_curApplies = 0, g_curApplyUs = 0;   // creations inside the CURRENT update
volatile LONG64 g_workUs = 0;       // cumulative streaming work, any thread (update + outside creations) - read by the stall line
// window counters (reset by Report)
volatile LONG64 g_wUpd = 0, g_wUpdUs = 0, g_wUpdMaxUs = 0, g_wSlow = 0;
volatile LONG64 g_wApplyIn = 0, g_wApplyInUs = 0, g_wApplyMaxUs = 0, g_wApplyOut = 0, g_wApplyOutUs = 0;
struct Slow { LONG64 us; LONG64 applies; LONG64 applyUs; FILETIME when; DWORD tid; };
Slow g_ring[kRing];
volatile LONG g_ringHead = 0;       // written by the render thread
LONG g_ringTail = 0;                // read by the poll
volatile LONG g_ringLost = 0;

void MaxInto(volatile LONG64* slot, LONG64 v)
{
    LONG64 cur = *slot;
    while (v > cur) {
        const LONG64 seen = InterlockedCompareExchange64(slot, v, cur);
        if (seen == cur) {
            break;
        }
        cur = seen;
    }
}

LONG64 NowTicks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void __fastcall HookUpdate(__int64 a1, unsigned int* a2)
{
    const LONG64 t0 = NowTicks();
    InterlockedExchange64(&g_curApplies, 0);
    InterlockedExchange64(&g_curApplyUs, 0);
    InterlockedExchange(&g_updTid, static_cast<LONG>(GetCurrentThreadId()));
    g_trueUpdate(a1, a2);
    InterlockedExchange(&g_updTid, 0);
    const LONG64 us = static_cast<LONG64>(static_cast<double>(NowTicks() - t0) * g_usPerTick);
    InterlockedAdd64(&g_workUs, us);
    InterlockedIncrement64(&g_wUpd);
    InterlockedAdd64(&g_wUpdUs, us);
    MaxInto(&g_wUpdMaxUs, us);
    if (us >= kSlowUs) {
        InterlockedIncrement64(&g_wSlow);
        const LONG h = g_ringHead;
        if (h - g_ringTail < kRing) {
            Slow& s = g_ring[h % kRing];
            s.us = us;
            s.applies = g_curApplies;
            s.applyUs = g_curApplyUs;
            s.tid = GetCurrentThreadId();
            GetSystemTimePreciseAsFileTime(&s.when);
            InterlockedExchange(&g_ringHead, h + 1);
        } else {
            InterlockedIncrement(&g_ringLost);
        }
    }
}

__int64 __fastcall HookApply(void* a1, unsigned int a2, __int64 a3)
{
    const LONG64 t0 = NowTicks();
    const __int64 r = g_trueApply(a1, a2, a3);
    const LONG64 us = static_cast<LONG64>(static_cast<double>(NowTicks() - t0) * g_usPerTick);
    MaxInto(&g_wApplyMaxUs, us);
    if (g_updTid != 0 && static_cast<DWORD>(g_updTid) == GetCurrentThreadId()) {
        InterlockedIncrement64(&g_curApplies);
        InterlockedAdd64(&g_curApplyUs, us);
        InterlockedIncrement64(&g_wApplyIn);
        InterlockedAdd64(&g_wApplyInUs, us);
    } else {
        InterlockedAdd64(&g_workUs, us);   // outside the update: not already inside an update's time
        InterlockedIncrement64(&g_wApplyOut);
        InterlockedAdd64(&g_wApplyOutUs, us);
    }
    return r;
}

void InstallOnce(HMODULE rd3d11)
{
    if (g_state != 0 || !rd3d11) {
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_usPerTick = f.QuadPart > 0 ? 1e6 / static_cast<double>(f.QuadPart) : 0.0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(rd3d11);
    int a1 = 0, a2 = 0;
    const bool u = InstallSingleGameDetour(base + kUpdateRva, kUpdateBytes, sizeof(kUpdateBytes),
                                           reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_trueUpdate),
                                           "rd3d11 streaming update +43320", &a1);
    const bool c = u && InstallSingleGameDetour(base + kApplyRva, kApplyBytes, sizeof(kApplyBytes),
                                                reinterpret_cast<void*>(&HookApply), reinterpret_cast<void**>(&g_trueApply),
                                                "rd3d11 stream-in creation +42C30", &a2);
    InterlockedExchange(&g_state, (u && c) ? 1 : -1);
    // [PROBE: STREAMAPPLY] gate=diag_stream_budget  site=FLAT+HEADSET  log-only=yes
    LOG("[STREAMAPPLY] %s (logged unconditionally) - streaming update rd3d11+0x43320 %s (%d attempt(s)), stream-in creation "
        "rd3d11+0x42C30 %s (%d attempt(s)). Updates >= %lld ms are listed one by one; 30 s summaries ride the STREAMBUDGET line. "
        "diag_stream_budget=1", (u && c) ? "LIVE" : "REFUSED - NO VERDICT for the timer", u ? "LIVE" : "NOT installed (prologue "
        "mismatch or commit failed - see the boneblend line above)", a1, c ? "LIVE" : (u ? "NOT installed" : "not tried"), a2,
        static_cast<long long>(kSlowUs / 1000));
}

// Prints any slow updates the render thread queued. Poll thread only.
void Drain()
{
    while (g_ringTail != g_ringHead) {
        const Slow s = g_ring[g_ringTail % kRing];
        ++g_ringTail;
        SYSTEMTIME st{};
        FILETIME lt{};
        FileTimeToLocalFileTime(&s.when, &lt);
        FileTimeToSystemTime(&lt, &st);
        // [PROBE: STREAMAPPLY] gate=diag_stream_budget  site=FLAT+HEADSET  log-only=yes
        LOG("[STREAMAPPLY] SLOW streaming update at %02u:%02u:%02u.%03u on tid %lu: %.2f ms, of which %lld stream-in "
            "creation(s) took %.2f ms", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s.tid, s.us / 1000.0,
            static_cast<long long>(s.applies), s.applyUs / 1000.0);
    }
}

void Report()
{
    if (g_state != 1) {
        return;
    }
    const LONG64 upd = InterlockedExchange64(&g_wUpd, 0), updUs = InterlockedExchange64(&g_wUpdUs, 0);
    const LONG64 updMax = InterlockedExchange64(&g_wUpdMaxUs, 0), slow = InterlockedExchange64(&g_wSlow, 0);
    const LONG64 ain = InterlockedExchange64(&g_wApplyIn, 0), ainUs = InterlockedExchange64(&g_wApplyInUs, 0);
    const LONG64 amax = InterlockedExchange64(&g_wApplyMaxUs, 0);
    const LONG64 aout = InterlockedExchange64(&g_wApplyOut, 0), aoutUs = InterlockedExchange64(&g_wApplyOutUs, 0);
    const LONG lost = InterlockedExchange(&g_ringLost, 0);
    // [PROBE: STREAMAPPLY] gate=diag_stream_budget  site=FLAT+HEADSET  log-only=yes
    LOG("[STREAMAPPLY] 30 s | streaming update: %lld call(s), %.1f ms in all, mean %.3f ms, max %.2f ms, %lld at >= %lld ms%s | "
        "stream-in creations inside the update %lld (%.1f ms), outside it %lld (%.1f ms), slowest single %.2f ms | "
        "diag_stream_budget=1", static_cast<long long>(upd), updUs / 1000.0, upd ? updUs / 1000.0 / upd : 0.0,
        updMax / 1000.0, static_cast<long long>(slow), static_cast<long long>(kSlowUs / 1000),
        lost ? " (some slow ones NOT listed: ring full)" : "", static_cast<long long>(ain), ainUs / 1000.0,
        static_cast<long long>(aout), aoutUs / 1000.0, amax / 1000.0);
}

}  // namespace timer

}  // namespace streambudget

unsigned long long StreamWorkMicros()
{
    return static_cast<unsigned long long>(streambudget::timer::g_workUs);
}

bool StreamTimerLive()
{
    return streambudget::timer::g_state == 1;
}

// ---- S1: SHADOW CASCADE STAGGER (2026-10-16, Jay "S1" / "Yes"; INV-584 / INV-585) ----------------------------------------
// DL1 re-renders all 4 sun cascades every frame (sub_1802B9680 loops dword_180AAD7EC cascades through sub_1802B7070, no skip);
// they cost 0.34-0.68 ms each at 4096^2 (INV-584). Dying Light The Beast renders a per-frame RANGE (i_hsm_begin / i_hsm_end).
// This staggers the far cascades: with shadow_cascade_stagger N >= 2, cascade i >= shadow_stagger_first is rendered only when
// (frame + i) % N == 0, so with N = 2 and first = 2 exactly one of cascades 2 / 3 renders per frame and 0 / 1 render every frame.
// WHY A SKIP IS SAFE (static, OUR engine e9fbfc2c): sub_1802B7070 is the ONLY writer of the cascade's HSM matrix (HSM slot 9
// +7B8B80, stored at hsm + ((i + 2) << 6)); the shaders read it back through EvalSH_HSM_XFORM_ES / _WS -> CRenderer+32 -> HSM slot 10
// (+7B8C70), so a skipped cascade keeps texture AND matrix from the same frame. Its clear (hsm slot 4, and hsm50.ppfx's Clear) is
// skipped with it - in NON-atlas mode each cascade has its own target; in ATLAS mode (hsm+24) index 0 clears the whole atlas, so atlas
// mode is REFUSED. NOT read: a CPU reader of the light manager's second per-frame copy (sub_1802B6680 -> +0x200 + 64 i).
// Detour: engine +2B7070, sole caller +2B9680, prologue 20 B whole instructions with no RIP-relative operand (bytes read from the
// deployed DLL 2026-10-16). It runs on whichever thread runs the cascade loop - one loop, one thread at a time (the loop is
// sequential), so plain statics. Per call: a few compares; NOTHING logged in the detour - events are flagged and the feature's own
// thread (shadowstagger::Thread -> Report, started only at >= 2) prints them. (Was the scePad poll's ShadowStaggerTick until
// 2026-10-16 - removed so that nothing runs at 0.)
// ⚠ CORRECTED 2026-10-16 after shadowstagger_run1 (one eye's distant shadows dark + an out-of-commit crash at engine +761F48; D-234):
// (1) NOT the only matrix writer - sub_1802B6680 overwrites all 4 cascade matrices with a placeholder EVERY frame before the loop
//     (+2B69D0..+2B6A33, INV-586), so a skip writes back the 64 bytes this hook saved after the cascade's last real render.
// (2) A skip left the cascade's per-frame ShadowMap_%d queue (*a7) un-executed; CRenderer slot 49 (+7621A0) only reuses queues whose
//     byte +432 is set and otherwise mallocs a new 8 MiB one (INV-587) - so a skip sets +432 = 1, the state ExecLayer's own empty-queue
//     path (+762E4D) leaves. The 30 s line reports the CRenderer queue pool count (+1656) and process commit as the leak control.
namespace shadowstagger {

constexpr uintptr_t kCascadeRva = 0x2B7070;
constexpr uintptr_t kCountRva = 0xAAD7EC;     // dword_180AAD7EC: this frame's sun cascade count (sub_1802B6680)
constexpr uintptr_t kRendererRva = 0xA40248;  // qword_180A40248 == qword_180A40220 = the CRenderer (RENDER-PATH-MAP §40)
constexpr size_t kPoolCountOff = 1656;        // CRenderer +1656: render-queue pool count (slot 49 +7621A0 appends at +1648)
constexpr size_t kQueueDoneOff = 432;         // CBatchedRenderQueue +432: executed - slot 49 reuses only queues with it set
const unsigned char kCascadeBytes[20] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
                                         0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr int kMaxCascades = 8;
typedef __int64(__fastcall* CascadeFn)(__int64, __int64, __int64, unsigned int, unsigned __int8, __int64, __int64*);
CascadeFn g_true = nullptr;
volatile LONG g_state = 0;        // 0 not tried, 1 live, -1 refused / failed
uintptr_t g_engine = 0;
unsigned long long g_frame = 0;
__int64 g_lastHsm = 0;
int g_lastCount = -1;
bool g_seen[kMaxCascades] = {};
volatile LONG g_rendered[kMaxCascades] = {};
volatile LONG g_skipped[kMaxCascades] = {};
volatile LONG g_resets = 0, g_atlasHits = 0;
volatile LONG g_lastResetCause = 0;   // 1 HSM pointer changed, 2 cascade count changed
unsigned char g_savedMtx[kMaxCascades][64] = {};   // hsm + ((i + 2) << 6) as +2B7070 left it (HSM slot 9's store, final form)
volatile LONG g_restored = 0, g_released = 0, g_noQueue = 0;

__int64 __fastcall Hook(__int64 a1, __int64 hsm, __int64 cam, unsigned int idx, unsigned __int8 first, __int64 a6, __int64* a7)
{
    const int n = ShadowCascadeStagger;
    if (idx == 0) {
        ++g_frame;
        int count = -1;
        if (g_engine) {
            static const uintptr_t s_countRva = EngRva(kCountRva);   // resolved once - this runs per cascade
            count = *reinterpret_cast<volatile int*>(g_engine + s_countRva);
        }
        if (hsm != g_lastHsm || count != g_lastCount) {
            InterlockedExchange(&g_lastResetCause, hsm != g_lastHsm ? 1 : 2);
            InterlockedIncrement(&g_resets);
            memset(g_seen, 0, sizeof(g_seen));
            g_lastHsm = hsm;
            g_lastCount = count;
        }
    }
    bool skip = false;
    if (n >= 2 && idx < static_cast<unsigned>(kMaxCascades) && static_cast<int>(idx) >= ShadowStaggerFirst && hsm == g_lastHsm) {
        const bool atlas = hsm && *reinterpret_cast<const unsigned char*>(hsm + 24) != 0;
        if (atlas) {
            InterlockedIncrement(&g_atlasHits);       // never skip in atlas mode: index 0 clears the whole atlas
        } else if (g_seen[idx] && ((g_frame + idx) % static_cast<unsigned long long>(n)) != 0) {
            skip = true;
        }
    }
    if (skip) {
        // g_seen[idx] => g_savedMtx[idx] holds this HSM's last real matrix for idx (both set together below, both cleared on reset).
        memcpy(reinterpret_cast<void*>(hsm + ((static_cast<__int64>(idx) + 2) << 6)), g_savedMtx[idx], 64);
        InterlockedIncrement(&g_restored);
        const __int64 queue = a7 ? *a7 : 0;
        if (queue) {
            *reinterpret_cast<volatile unsigned char*>(queue + kQueueDoneOff) = 1;
            InterlockedIncrement(&g_released);
        } else {
            InterlockedIncrement(&g_noQueue);   // slot 49 returns 0 while CRenderer+1640 is set: nothing was allocated
        }
        InterlockedIncrement(&g_skipped[idx]);
        return 0;   // the caller (+2B9680) ignores the return value
    }
    const __int64 r = g_true(a1, hsm, cam, idx, first, a6, a7);
    if (idx < static_cast<unsigned>(kMaxCascades)) {
        if (hsm) {
            memcpy(g_savedMtx[idx], reinterpret_cast<const void*>(hsm + ((static_cast<__int64>(idx) + 2) << 6)), 64);
        }
        g_seen[idx] = hsm != 0;
        InterlockedIncrement(&g_rendered[idx]);
    }
    return r;
}

// NvidiaEffects(1, ...) active in the user's video.scr = PCSS may run (it records per cascade inside +2B7070). Returns 1 = PCSS
// possible, 0 = no active NvidiaEffects line with a non-zero first argument, -1 = the file could not be read (REFUSE: unknown).
int PcssPossible(char* path, size_t cap)
{
    path[0] = '\0';
    typedef HRESULT(WINAPI * KnownFolderFn)(const GUID&, DWORD, HANDLE, PWSTR*);
    static const GUID kDocuments = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
    HMODULE shell = LoadLibraryA("shell32.dll");
    KnownFolderFn fn = shell ? reinterpret_cast<KnownFolderFn>(GetProcAddress(shell, "SHGetKnownFolderPath")) : nullptr;
    PWSTR docsW = nullptr;
    if (!fn || FAILED(fn(kDocuments, 0, nullptr, &docsW)) || !docsW) {
        return -1;
    }
    char docs[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, docsW, -1, docs, sizeof(docs), nullptr, nullptr);
    CoTaskMemFree(docsW);
    _snprintf_s(path, cap, _TRUNCATE, "%s\\DyingLight\\out\\settings\\video.scr", docs);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        return -1;
    }
    char line[512];
    int result = 0;
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') { ++p; }
        if (*p == '!') { continue; }   // the keyword reference block, not a setting
        const char* k = strstr(p, "NvidiaEffects(");
        if (!k) { continue; }
        const int v = atoi(k + 14);
        if (v != 0) { result = 1; }
    }
    fclose(f);
    return result;
}

void InstallOnce()
{
    if (g_state != 0) {
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!engine) {
        return;
    }
    char path[MAX_PATH * 2] = {};
    const int pcss = PcssPossible(path, sizeof(path));
    if (pcss != 0) {
        InterlockedExchange(&g_state, -1);
        // [PROBE: SHADOWSTAGGER] gate=shadow_cascade_stagger  site=FLAT+HEADSET  log-only=no (pass-through detour)
        LOG("[SHADOWSTAGGER] REFUSED (logged unconditionally) - %s (%s). Shadows stay stock: every cascade every frame. "
            "shadow_cascade_stagger=%d", pcss > 0 ? "an active NvidiaEffects(1,...) line: NVIDIA PCSS records per cascade"
                                                  : "video.scr could not be read, so PCSS cannot be ruled out",
            path[0] ? path : "Documents folder not resolved", ShadowCascadeStagger);
        return;
    }
    g_engine = reinterpret_cast<uintptr_t>(engine);
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(g_engine + kCascadeRva, kCascadeBytes, sizeof(kCascadeBytes),
                                            reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_true),
                                            "engine sun cascade render +2B7070", &attempts);
    InterlockedExchange(&g_state, ok ? 1 : -1);
    // [PROBE: SHADOWSTAGGER] gate=shadow_cascade_stagger  site=FLAT+HEADSET  log-only=no (pass-through detour)
    LOG("[SHADOWSTAGGER] %s (logged unconditionally) - engine +2B7070 detour %s after %d attempt(s); video.scr %s has no active "
        "NvidiaEffects(1,...). Cascades >= %d refresh one in %d frames, the rest every frame; HSM atlas mode is never staggered. "
        "shadow_cascade_stagger=%d", ok ? "LIVE" : "NOT INSTALLED", ok ? "committed" : "REFUSED (prologue mismatch or commit "
        "failed - see the boneblend line above); shadows stay stock", attempts, path, ShadowStaggerFirst, ShadowCascadeStagger,
        ShadowCascadeStagger);
}

}  // namespace shadowstagger

namespace shadowstagger {

// The SHADOWSTAGGER lines, once a second on this feature's own thread (Thread below) - reached only after the detour is LIVE.
void Report()
{
    static ULONGLONG s_next = 0;
    static LONG s_resetsSeen = 0;
    const LONG resets = g_resets;
    if (resets != s_resetsSeen) {
        s_resetsSeen = resets;
        // [PROBE: SHADOWSTAGGER] gate=shadow_cascade_stagger  site=FLAT+HEADSET  log-only=no (pass-through detour)
        LOG("[SHADOWSTAGGER] full refresh #%ld - %s (cascade count now %d); every cascade renders once before any skip",
            resets, g_lastResetCause == 1 ? "the HSM object changed (level load / video settings)" : "the cascade count changed",
            g_lastCount);
    }
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) {
        return;
    }
    s_next = now + 30000;
    char per[320];
    size_t at = 0;
    per[0] = '\0';
    for (int i = 0; i < kMaxCascades; ++i) {
        const LONG r = InterlockedExchange(&g_rendered[i], 0);
        const LONG s = InterlockedExchange(&g_skipped[i], 0);
        if (r == 0 && s == 0) { continue; }
        const int w = _snprintf_s(per + at, sizeof(per) - at, _TRUNCATE, "%s#%d rendered %ld skipped %ld", at ? ", " : "", i, r, s);
        if (w > 0) { at += static_cast<size_t>(w); }
    }
    const LONG atlas = InterlockedExchange(&g_atlasHits, 0);
    const LONG restored = InterlockedExchange(&g_restored, 0);
    const LONG released = InterlockedExchange(&g_released, 0);
    const LONG noQueue = InterlockedExchange(&g_noQueue, 0);
    // The leak control (INV-587): the CRenderer's render-queue pool count only grows when slot 49 finds no executed queue to reuse.
    // -1 = not readable. Commit: this process's private commit and the system's free commit (both MB, -1 = query failed).
    long long pool = -1;
    void* renderer = nullptr;
    if (g_engine && SafeReadLocal(g_engine + EngRva(kRendererRva), &renderer, sizeof(renderer)) && renderer) {
        unsigned int n = 0;
        if (SafeReadLocal(reinterpret_cast<uintptr_t>(renderer) + kPoolCountOff, &n, sizeof(n))) {
            pool = n;
        }
    }
    long long procMB = -1, freeMB = -1;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        procMB = static_cast<long long>(pmc.PrivateUsage >> 20);
    }
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        freeMB = static_cast<long long>(ms.ullAvailPageFile >> 20);
    }
    // [PROBE: SHADOWSTAGGER] gate=shadow_cascade_stagger  site=FLAT+HEADSET  log-only=no (pass-through detour)
    LOG("[SHADOWSTAGGER] 30 s | %s | skips: matrix restored %ld, queue released %ld, no queue %ld (restored and released must "
        "each equal the skipped total) | render-queue pool %lld (must stay flat - growth = un-executed queues, INV-587) | commit: "
        "process %lld MB, system free %lld MB | cascade count %d | atlas-mode calls (never skipped) %ld | full refreshes so far %ld | "
        "shadow_cascade_stagger=%d, shadow_stagger_first=%d", at ? per : "no cascade calls", restored, released, noQueue, pool,
        procMB, freeMB, g_lastCount, atlas, resets, ShadowCascadeStagger, ShadowStaggerFirst);
}

// Started by ShadowStaggerStart, at most once per process: install (or refuse, logged either way), then report once a second.
DWORD WINAPI Thread(LPVOID)
{
    InstallOnce();
    if (g_state != 1) {
        return 0;
    }
    for (;;) {
        Report();
        Sleep(1000);
    }
}

volatile LONG g_started = 0;

}  // namespace shadowstagger

// 2026-10-16, Jay: "If it's set to 0, it quite literally does nothing at all. No load, no compare, no return. Truly inert." So there
// is NO per-poll / per-frame call any more (it was ShadowStaggerTick on the scePad poll). The only callers are the config load and a
// menu edit of shadow_cascade_stagger, and both call this ONLY when the value is >= 2. Once started (installed or refused) it stays
// for the session: setting it back to 0 leaves the detour as a pass-through, as before.
void ShadowStaggerStart()
{
    using namespace shadowstagger;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        // [PROBE: SHADOWSTAGGER] gate=shadow_cascade_stagger  site=FLAT+HEADSET  log-only=no (pass-through detour)
        LOG("[SHADOWSTAGGER] NOT STARTED - CreateThread failed (%lu); shadows stay stock. shadow_cascade_stagger=%d",
            GetLastError(), ShadowCascadeStagger);
    }
}

bool StreamBudgetSnapshot(unsigned long long* usedBytes, unsigned long long* budgetBytes)
{
    HMODULE rd3d11 = GetModuleHandleA("rd3d11_x64_rwdi.dll");
    return streambudget::ReadQword(rd3d11, streambudget::kUsedRva, usedBytes) &&
           streambudget::ReadQword(rd3d11, streambudget::kBudgetRva, budgetBytes);
}

void ApplyStreamingBudgetOnce()
{
    using namespace streambudget;
    if (g_refused) {
        return;
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE rd3d11 = GetModuleHandleA("rd3d11_x64_rwdi.dll");
    if (!engine || !rd3d11) {
        return;
    }
    void* api = nullptr;
    void* renderer = nullptr;
    if (!SafeReadLocal(reinterpret_cast<uintptr_t>(engine) + EngRva(kEngineApiRva), &api, sizeof(api)) || !api) {
        return;  // renderer not up yet - try again on a later poll
    }
    SafeReadLocal(reinterpret_cast<uintptr_t>(engine) + EngRva(kEngineRendererRva), &renderer, sizeof(renderer));
    const ULONGLONG now = GetTickCount64();
    if (DiagStreamBudget) {
        timer::InstallOnce(rd3d11);   // once; logs LIVE / REFUSED
        timer::Drain();               // slow updates the render thread queued
        if (Probe(rd3d11, now)) {
            timer::Report();
        }
    }
    if (g_engineMB < 0) {
        unsigned long long b = 0;
        if (!ReadQword(rd3d11, kBudgetRva, &b) || b == 0) {
            return;   // the engine has not set its own budget yet - the floor for AUTO must be ITS value
        }
        g_engineMB = static_cast<int>(b >> 20);
        LOG("streambudget: the engine's own budget read as %d MB (rd3d11+0x784b8); stream_budget_mb=%d, "
            "stream_budget_reserve_mb=%d (-1 = AUTO, 20%% of the card's DXGI budget)",
            g_engineMB, StreamBudgetMB, StreamBudgetReserveMB);
    }
    const int setting = StreamBudgetMB;
    if (setting == 0) {
        g_sentSetting = setting;
        return;   // leave the engine alone
    }
    if (!Verify(engine, rd3d11, api, renderer)) {
        return;
    }
    if (setting > 0) {
        if (setting != g_sentSetting) {
            g_sentSetting = setting;
            Send(rd3d11, api, renderer, setting, "stream_budget_mb (fixed)");
        }
        return;
    }
    // AUTO (-1).
    if (setting == g_sentSetting && now < g_nextAuto) {
        return;
    }
    g_nextAuto = now + kAutoEveryMs;
    unsigned long long vb = 0, vu = 0, su = 0;
    long long reserveMB = 0;
    const int mb = AutoMB(rd3d11, &vb, &vu, &su, &reserveMB);
    if (mb < 0) {
        if (setting != g_sentSetting) {
            g_sentSetting = setting;
            LOG("streambudget: AUTO - the DXGI video-memory query FAILED (rd3d11 device unreadable or no IDXGIAdapter3); "
                "nothing sent, the engine keeps %d MB. Retrying every %llu s.", g_engineMB,
                static_cast<unsigned long long>(kAutoEveryMs / 1000));
        }
        return;
    }
    if (setting != g_sentSetting || g_sentMB == INT_MIN || mb - g_sentMB >= kAutoStepMB || g_sentMB - mb >= kAutoStepMB) {
        g_sentSetting = setting;
        char why[240];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "AUTO (streaming used %llu MB + DXGI free %lld MB - reserve %lld MB %s%llu MB budget, floor %d, ceiling %d)",
                    su >> 20, (static_cast<long long>(vb) - static_cast<long long>(vu)) >> 20, reserveMB,
                    StreamBudgetReserveMB >= 0 ? "fixed, of a " : "AUTO 20% of a ", vb >> 20, g_engineMB, kAutoCeilMB);
        Send(rd3d11, api, renderer, mb, why);
    }
}

// ---- Texture-streaming PRIORITY DISTANCE, live-tunable (2026-09-05) ---------
//
// The most promising remaining lever, and the one system with positive
// evidence behind it: /notexturestreaming was the ONLY change that ever made
// the distant NPC render. A missing NPC is not obviously a texture problem -
// unless the streamer hides an object outright until its required assets are
// resident, which fits exactly (force everything resident -> NPC appears;
// re-enable streaming -> it vanishes again at range).
//
// r_device.scr states the governing value in the game's own words:
//     !TextureStreamingLowestPriorirtyDistance(i) // meters. Streaming
//     //   priority <hi;lo> is used in range <0;specified>
//     TextureStreamingLowestPriorirtyDistance(100)
// 100 metres. Past that, everything is lowest streaming priority. Fine on a
// monitor; in VR you routinely look far past 100m and all of it is
// deprioritised - matching the reversible, distance-gated behaviour reported.
//
// Set through SRendererAPI slot 143 CASE 6 - the same dispatcher already
// proven working for case 5 (the memory budget, confirmed by read-back). The
// engine stores it in rd3d11's DAT_1800784b4 as a float of metres*100, so it
// self-verifies the same way. Applied on change only, so the overlay slider
// gives immediate feedback with no relaunch.
volatile LONG g_lastPriorityDistApplied = -1;

void ApplyStreamPriorityDistance()
{
    const int want = static_cast<int>(StreamPriorityDistanceM);
    if (want <= 0 || want == InterlockedCompareExchange(&g_lastPriorityDistApplied, 0, 0)) {
        return;  // unset, or already applied
    }
    HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE rd3d11 = GetModuleHandleA("rd3d11_x64_rwdi.dll");
    if (!engine) {
        return;
    }
    void* api = nullptr;
    __try {
        api = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(engine) + EngRva(0xA40218));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!api) {
        return;  // renderer not up yet
    }

    // DAT_1800784b4 - a float, metres*100, written by case 6.
    const uintptr_t distAddr =
        rd3d11 ? reinterpret_cast<uintptr_t>(rd3d11) + 0x784B4 : 0;
    float before = -1.0f, after = -1.0f;
    if (distAddr) {
        SafeReadLocal(distAddr, &before, sizeof(before));
    }

    if (SafeCallStreamingCmd(api, 6, want)) {
        if (distAddr) {
            SafeReadLocal(distAddr, &after, sizeof(after));
        }
        InterlockedExchange(&g_lastPriorityDistApplied, want);
        LOG("streamdist: slot 143 case 6 called with %d m. rd3d11+0x784b4 "
            "before=%.1f after=%.1f (metres*100)%s",
            want, before, after,
            (after != before)
                ? "  -> CONFIRMED"
                : "  -> UNCHANGED, the call did not land");
    } else {
        LOG("streamdist: call into SRendererAPI slot 143 case 6 faulted");
        InterlockedExchange(&g_lastPriorityDistApplied, want);  // don't spin on it
    }
}

// A read that cannot fault the process. The hook runs on an engine thread and
// must never be the reason it dies.
bool SafeReadLocal(uintptr_t addr, void* out, size_t bytes)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Runs on an engine thread inside the camera copy. No files, no allocation, no
// locks - just floats in and floats out.
// Investigation, 2026-09-04 continued: four real fix attempts on gameplay/
// streaming state (physics-debug mode, DebugRenderer enable x2,
// SetSimulationCamera) all had zero effect on a VR-only, ground-plane-only,
// distance/angle-dependent line/dot rendering artifact. New theory: a tiny
// residual ROTATIONAL mismatch between the two eyes' final computed camera
// matrices - real stereo displays should only ever differ the two eyes by
// TRANSLATION (the IPD offset), never rotation. A small angular error would
// be nearly invisible up close (near-field convergence naturally masks
// small angular error) but grow into visible ghosting/double-geometry with
// distance on a large, flat, texture-rich surface like the ground -
// matching every symptom seen so far, without needing any new D3D11-level
// tooling: this hook already computes and writes the exact final rotation
// (`nm`, in LevelCamera) for every tracked camera, every frame. Purely
// observational - no engine state is touched, unlike everything tried so
// far tonight.
struct EyeDiagSlot {
    volatile uintptr_t obj;
    float rot[9];   // final, post-hook rotation actually written to the object
    float pos[3];
    volatile LONG64 hits;
    volatile LONG64 lastMs;
};
constexpr int kEyeDiagSlots = 8;
EyeDiagSlot g_eyeDiag[kEyeDiagSlots] = {};

// angle (degrees) between two rotation matrices: trace(a^T b) = 1 + 2cos(theta)
float RotationAngleDeg(const float a[9], const float b[9])
{
    float trace = 0.0f;
    for (int k = 0; k < 9; ++k) {
        trace += a[k] * b[k];
    }
    float cosTheta = (trace - 1.0f) * 0.5f;
    if (cosTheta > 1.0f) cosTheta = 1.0f;
    if (cosTheta < -1.0f) cosTheta = -1.0f;
    return acosf(cosTheta) * (180.0f / 3.14159265f);
}

void UpdateEyeDiag(uintptr_t obj, const float nm[9], float px, float py, float pz)
{
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    int slot = -1, free = -1;
    for (int i = 0; i < kEyeDiagSlots; ++i) {
        if (g_eyeDiag[i].obj == obj) { slot = i; break; }
        if (free < 0 && g_eyeDiag[i].obj == 0) { free = i; }
        if (free < 0 && now - g_eyeDiag[i].lastMs > 4000) { free = i; }
    }
    if (slot < 0) {
        if (free < 0) return;
        slot = free;
        g_eyeDiag[slot].obj = obj;
        g_eyeDiag[slot].hits = 0;
    }
    memcpy(g_eyeDiag[slot].rot, nm, sizeof(g_eyeDiag[slot].rot));
    g_eyeDiag[slot].pos[0] = px; g_eyeDiag[slot].pos[1] = py; g_eyeDiag[slot].pos[2] = pz;
    ++g_eyeDiag[slot].hits;
    g_eyeDiag[slot].lastMs = now;

    // Every ~2s, find the two highest-hit-rate slots updated within the last
    // 50ms of each other (the same-frame two eyes) and compare.
    static LONG64 lastCompareMs = 0;
    if (now - lastCompareMs < 2000) {
        return;
    }

    // 2026-09-04: the "top 2 by hit count" comparison below came back with
    // both eyes reading near-identical positions (interEyeDist ~= 0), which
    // undermines confidence the right pair was picked at all - dump every
    // currently-active slot's own data first so this can be checked
    // directly instead of trusting the heuristic blindly.
    for (int i = 0; i < kEyeDiagSlots; ++i) {
        if (!g_eyeDiag[i].obj || now - g_eyeDiag[i].lastMs > 200) continue;
        LOG("camhook: [eye diag full] slot=%d obj=%p hits=%lld pos=(%.3f,%.3f,%.3f)",
            i, reinterpret_cast<void*>(g_eyeDiag[i].obj), g_eyeDiag[i].hits,
            g_eyeDiag[i].pos[0], g_eyeDiag[i].pos[1], g_eyeDiag[i].pos[2]);
    }

    int best1 = -1, best2 = -1;
    LONG64 bestHits1 = -1, bestHits2 = -1;
    for (int i = 0; i < kEyeDiagSlots; ++i) {
        if (!g_eyeDiag[i].obj || now - g_eyeDiag[i].lastMs > 200) continue;
        if (g_eyeDiag[i].hits > bestHits1) {
            bestHits2 = bestHits1; best2 = best1;
            bestHits1 = g_eyeDiag[i].hits; best1 = i;
        } else if (g_eyeDiag[i].hits > bestHits2) {
            bestHits2 = g_eyeDiag[i].hits; best2 = i;
        }
    }
    if (best1 < 0 || best2 < 0) {
        return;
    }
    lastCompareMs = now;
    const float angleDeg = RotationAngleDeg(g_eyeDiag[best1].rot, g_eyeDiag[best2].rot);
    const float dx = g_eyeDiag[best1].pos[0] - g_eyeDiag[best2].pos[0];
    const float dy = g_eyeDiag[best1].pos[1] - g_eyeDiag[best2].pos[1];
    const float dz = g_eyeDiag[best1].pos[2] - g_eyeDiag[best2].pos[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    LOG("camhook: [eye rotation diag] obj1=%p (hits=%lld) obj2=%p (hits=%lld) "
        "angleBetween=%.4f deg  interEyeDist=%.4f",
        reinterpret_cast<void*>(g_eyeDiag[best1].obj), g_eyeDiag[best1].hits,
        reinterpret_cast<void*>(g_eyeDiag[best2].obj), g_eyeDiag[best2].hits,
        angleDeg, dist);
}

void LevelCamera(uintptr_t obj)
{
    // RIGGING STAGE 2 rides here rather than dlvr_OnCameraCopied. This runs
    // only for cameras the engine is actively driving (~2 per frame - the two
    // eyes), where OnCameraCopied fires for every camera copy in the frame,
    // measured at ~35,000/sec. Still the engine's own render thread. The write
    // also carries its own rate guard so the cadence cannot surprise us again.
    if (DiagRigStage2) {
        boneprobe::Stage2StaticPose(obj);
    }

    // Independent of the camera hook itself - runs (self-throttled, and only
    // while the relevant setting is on) regardless of hook op, since this is
    // just riding LevelCamera's own per-frame call cadence to drive
    // unrelated diagnostics.
    FindPlayerMoveStyleViaWeaponAnchor();
    TryDisablePhysicsDebugRender();
    FeedSimulationCamera();
    WatchLineAddOnce();


    if (CamHookOp == HookOp::Observe) {
        return;
    }
    const size_t camOff = static_cast<size_t>(RttiMatrixOffset());
    const size_t viewOff = camOff - 0x30;

    float cam[12];
    memcpy(cam, reinterpret_cast<const void*>(obj + camOff), sizeof(cam));

    // >>> [ANCHORAB]: publish the ENGINE'S OWN camera world position. <<<
    // mtx34 is row-major 3x4 and "the fourth column is the position" (the
    // comment below this block), so it is floats 3/7/11. Read-only with respect
    // to the game; it only copies out.
    //
    // ⚠ THIS SITS AFTER `if (CamHookOp == HookOp::Observe) return;`, so in
    // Observe mode nothing is published and [ANCHORAB] says NO VERDICT rather
    // than comparing against zeros.

    float m[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r * 3 + c] = CamHookTransposed ? cam[c * 4 + r] : cam[r * 4 + c];
        }
    }
    // Refuse anything that is not a rotation rather than write nonsense into a
    // live camera.
    //
    // The basis vectors are the COLUMNS: mtx34::orientation writes the view
    // direction to floats 2, 6 and 10, and builds right = cross(up, forward).
    // So column 0 is right, column 1 is up, column 2 is forward, and the fourth
    // column is the position. A valid camera here is right-handed, which means
    // its determinant is +1 and not merely +-1.
    float axis[3][3];   // axis[i] = column i
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            axis[i][k] = m[k * 3 + i];
        }
    }
    auto dot = [&](int a, int b) {
        return axis[a][0] * axis[b][0] + axis[a][1] * axis[b][1] +
               axis[a][2] * axis[b][2];
    };

    float maxScaleErr = 0.0f, maxOrtho = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float len2 = dot(i, i);
        // Written so NaN fails: a plain out-of-range test lets NaN through,
        // and the stable-looking camera that crashed on lock is exactly where a
        // silently-accepted NaN would do it.
        if (!(len2 > 0.9f && len2 < 1.1f)) {
            ++g_probe.skewed;
            strncpy_s(g_reject, sizeof(g_reject),
                      "matrix was not a rotation - not touched", _TRUNCATE);
            if (DiagCamhookVerbose) LOG("camhook: reject obj=%p axis=%d len2=%.4f - %s", reinterpret_cast<void*>(obj), i,
                len2, g_reject);
            return;
        }
        const float err = fabsf(len2 - 1.0f);
        if (err > maxScaleErr) {
            maxScaleErr = err;
        }
        const float o = fabsf(dot(i, (i + 1) % 3));
        if (o > maxOrtho) {
            maxOrtho = o;
        }
    }
    // Unit-length axes are not enough on their own: a skewed basis passes that
    // test, and the quaternion round trip below would quietly discard the skew.
    if (!(maxOrtho < 0.02f)) {
        ++g_probe.skewed;
        strncpy_s(g_reject, sizeof(g_reject),
                  "camera axes are not perpendicular - not touched", _TRUNCATE);
        if (DiagCamhookVerbose) LOG("camhook: reject obj=%p maxOrtho=%.4f - %s", reinterpret_cast<void*>(obj), maxOrtho,
            g_reject);
        return;
    }

    // right . (up x forward) - positive for the right-handed basis the engine
    // builds. A negative determinant means a mirrored camera, and converting it
    // to a quaternion and back would hand the engine a right-handed matrix
    // instead: the picture would keep its shape but reverse its winding, so
    // front faces would be culled as back faces. Report it rather than do it.
    const float det = axis[0][0] * (axis[1][1] * axis[2][2] - axis[1][2] * axis[2][1]) -
                      axis[0][1] * (axis[1][0] * axis[2][2] - axis[1][2] * axis[2][0]) +
                      axis[0][2] * (axis[1][0] * axis[2][1] - axis[1][1] * axis[2][0]);

    // >>> [ANCHORAB]: publish the camera position HERE, and the position in the
    // function is the whole point. <<<
    //
    // This is past EVERY validation gate above - unit axes, scale error,
    // perpendicularity - so the layer has already decided this object is a real
    // camera. Two earlier placements were both wrong and both cost a run:
    //   v1: published right after the memcpy, BEFORE the validation. Cameras
    //       the layer was about to REJECT got published too, and [ANCHORAB]
    //       reported CAMERA peaks of 5124 m. The column was garbage.
    //   v2: filtered on CamHookIsTarget instead, which reads g_targets - a list
    //       only CamHookToggleTarget fills, from overlay.cpp:1989, i.e. a MANUAL
    //       user toggle. It is empty unless somebody clicked one, so NOTHING was
    //       ever published and the probe printed NO VERDICT for a whole run.
    // The acceptance point is the correct answer to both: it is where the code
    // itself concludes "this is a camera I will drive".
    //
    // mtx34 is row-major 3x4 and the fourth column is the position (see the
    // comment above), so floats 3/7/11. Copies out only; writes nothing.
    //   v3, 2026-09-23: the acceptance point was RIGHT and the single SLOT was
    //       wrong. camhook's own [table] dump shows TEN camera objects actively
    //       driven (slot0/slot1 a matched pair at hits=33065, slot2-9 at 14641
    //       each; 23 distinct objects over the run), so one slot held whichever
    //       was validated last and [ANCHORAB] differenced ACROSS cameras -
    //       gaps of 0.0000..1607.5875 m, lags of -109,848..+18,158 ms, with
    //       no-cam=0 the whole time. Passing `obj` keeps them SEPARATE.
    //       >>> The publish stays HERE. This gate says "is a camera"; it cannot
    //       say "is the player's eye", and choosing one by hit count or address
    //       would be deciding by heuristic before measuring - which is how v1
    //       and v2 were chosen. The consumer now reports EVERY object and the
    //       head is identified by its gap being small and stable. <<<
    {
        const float camPos[3] = { cam[3], cam[7], cam[11] };
        dlvr::PublishCameraWorldPos(obj, camPos);
    }

    g_probe.obj = obj;
    g_probe.det = det;
    g_probe.ortho = maxOrtho;
    g_probe.scale = 1.0f + maxScaleErr;
    memcpy(g_probe.inAxes, &axis[0][0], sizeof(g_probe.inAxes));

    if (det < 0.0f) {
        // REVERTED 2026-09-03 (see CLAUDE.md's "Look back" section): a
        // whole un-mirror "fix" lived here, gated on object-history plus
        // whether Y was held. Real testing proved it was never touching the
        // actual Look-back camera at all - the one object it ever fired on
        // (tracked since before gameplay even starts) is some other,
        // unrelated camera that happens to also go left-handed exactly
        // while Y is held, and "correcting" it broke the HUD for the
        // duration of every hold without ever making the view turn
        // backward. Back to the original, always-safe behaviour: converting
        // a mirrored basis to a quaternion and back would hand the engine a
        // right-handed matrix instead, reversing its winding (front faces
        // culled as back faces) - refuse rather than do that, for any
        // object, unconditionally.
        //
        // det and yHeld are still logged on every reject (throttled) as
        // live investigation data, not because either gates anything now -
        // useful for telling this same already-identified object apart from
        // any new candidate that might show up while hunting for whatever
        // object actually IS the Look-back camera.
        ++g_probe.negDet;
        strncpy_s(g_reject, sizeof(g_reject),
                  "camera basis is left-handed - levelling it would invert the "
                  "winding, so it is not touched", _TRUNCATE);
        static LONG64 lastRejectLogMs = 0;
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - lastRejectLogMs >= 250) {
            lastRejectLogMs = now;
            LOG_PERIODIC("camhook: reject obj=%p det=%.4f yHeld=%d - %s",
                reinterpret_cast<void*>(obj), det, IsYHeld() ? 1 : 0, g_reject);
        }
        return;
    }

    // (The 2026-09-03 yaw-investigation and grapple-hook game-pitch diagnostics
    // that sat here were removed 2026-11-15, removal batch 4: their output was
    // compiled out of BETA and nothing read their results.)

    float q[4], want[4], nm[9];
    QuatFromMat(m, q);
    bool cinPitch = false;   // ADDENDUM 2: this copy keeps the cutscene camera's pitch (set below)

    if (CamHookOp == HookOp::Delta) {
        // A known rotation added to whatever the engine wrote. Nothing here
        // depends on which axis is up or how the matrix is stored, so if this
        // does not tilt the view cleanly then the problem is not the levelling
        // maths.
        const float h = CamHookDeltaDeg * 3.14159265358979f / 360.0f;
        const float sn = sinf(h);
        float d[4] = { 0.0f, 0.0f, 0.0f, cosf(h) };
        d[CamHookAxis == 0 ? 0 : (CamHookAxis == 1 ? 1 : 2)] = sn;
        if (CamHookPreMultiply) {
            QuatMul(d, q, want);      // about a world axis
        } else {
            QuatMul(q, d, want);      // about the camera's own axis
        }
    } else {
        // ADDENDUM 2: in a cutscene the yaw comes from the split (exact near +-90 deg) and the root's own pitch is latched for every
        // copy this frame (applied below, after the root's head turn and before the head's pitch / roll). Otherwise YawOnly, as always.
        float cinYaw = 0.0f, cinP = 0.0f;
        cinPitch = CinPitchOn() && CinSplit(q, &cinYaw, &cinP);
        if (cinPitch) {
            if (obj == g_cinRoot) {
                g_cinAnimPitch = cinP;
                InterlockedExchange(&g_cinAnimPitchOk, 1);
            }
            AxisQuat(CamUpAxis() == 1 ? 2 : 1, cinYaw, want);
        } else {
            YawOnly(q, CamUpAxis(), want);
        }

        // Diagnostic, 2026-09-03: a real test showed two heavily-sampled
        // tracked objects (very likely the two stereo eyes) disagreeing on
        // how far they actually turned - one a clean 140 degrees, the other
        // something like 3x that. Logging each object's BASELINE yaw here,
        // before the look-back rotation is composed at all, to check
        // whether that mismatch starts even before our own code touches
        // anything (the two eyes not actually sharing the same source data
        // the way earlier sessions found they did) or only appears after.
        const float baseYawDeg =
            atan2f(2.0f * (want[3] * want[1] + want[0] * want[2]),
                   1.0f - 2.0f * (want[0] * want[0] + want[1] * want[1])) *
            (180.0f / 3.14159265358979f);

        if (CamHookLookBack) {
            // Per-object throttle, 2026-09-03: this used to be one shared
            // static across every call, so with more than one camera being
            // tracked (CamHookAuto levels every active one, not just a
            // single eye), whichever object happened to log first in a given
            // 40ms window silently starved every other object's line for
            // that window - the "clean" data traced and verified earlier may
            // only ever have been one of several look-back-affected objects,
            // never the others. (8-slot per-object throttle.)
            static uintptr_t lastBaseLogObj[8] = {};
            static LONG64 lastBaseLogMs[8] = {};
            int baseSlot = -1;
            for (int i = 0; i < 8; ++i) {
                if (lastBaseLogObj[i] == obj) { baseSlot = i; break; }
                if (baseSlot < 0 && lastBaseLogObj[i] == 0) { baseSlot = i; }
            }
            const LONG64 baseNow = static_cast<LONG64>(GetTickCount64());
            if (baseSlot >= 0 && baseNow - lastBaseLogMs[baseSlot] >= 40) {
                lastBaseLogObj[baseSlot] = obj;
                lastBaseLogMs[baseSlot] = baseNow;
                if (DiagCamhookVerbose) LOG("camhook: [look back baseline] obj=%p baseYaw=%.1f",
                    reinterpret_cast<void*>(obj), baseYawDeg);
            }
        }

        // "Look back", 2026-09-03 - see LookBackAmount()'s own comment for
        // the gating. Composed here, on the pure character-yaw quaternion,
        // BEFORE head pitch/roll goes on top - so a full 180 while looking
        // back still lets you tilt your head naturally on top of "backward"
        // the same way it already does on top of "forward", and releasing
        // mid-turn eases back from wherever it actually was, not from a
        // fixed start. Deliberately touches only `want` - the copy handed to
        // the RENDERER inside this hook - never the game's own source camera
        // or facing state, so it cannot drag movement direction around the
        // way the reverted head-turn attempt did (see CLAUDE.md): the
        // player keeps walking wherever the game itself still thinks is
        // forward, regardless of which way this makes the view point.
        //
        // Composing on the same up axis as YawOnly means left/right
        // multiplication order does not matter here - two rotations about
        // one shared axis commute - so there is no ordering subtlety to get
        // wrong the way there was for the aim-redirect quaternion work.
        if (CamHookLookBack) {
            const float t = LookBackAmount();
            const float capturedYaw = LookBackCapturedBaseline(obj, baseYawDeg, t > 0.0f);
            if (t > 0.0f) {
                // Full 180 degrees showed real culling/streaming holes on
                // the far side (see CLAUDE.md) - the target angle is now
                // tunable, defaulting to something closer to a natural
                // over-the-shoulder glance (head rotation plus a little
                // torso twist) than a full reversal, both because that is
                // more representative of how a person actually looks behind
                // themselves while running, and because a smaller turn asks
                // less of whatever upstream system turns out to be gating
                // the streaming issue.
                //
                // Built fresh from the CAPTURED baseline yaw, not from the
                // current (possibly feedback-contaminated) `want` - see
                // LookBackCapturedBaseline's own comment. This is still a
                // pure single-axis yaw quaternion, exactly what `want`
                // already held at this point, just reconstructed from a
                // stored angle instead of trusted live.
                const float baseRad = capturedYaw * 3.14159265358979f / 180.0f * 0.5f;
                want[0] = 0.0f; want[1] = 0.0f; want[2] = 0.0f;
                want[CamUpAxis() == 1 ? 2 : 1] = sinf(baseRad);
                want[3] = cosf(baseRad);

                const float angleRad = CamHookLookBackAngleDeg * 3.14159265358979f / 180.0f;
                const float halfAngle = t * angleRad * 0.5f;
                float lb[4] = { 0.0f, 0.0f, 0.0f, cosf(halfAngle) };
                lb[CamUpAxis() == 1 ? 2 : 1] = sinf(halfAngle);
                float turned[4];
                QuatMul(want, lb, turned);
                want[0] = turned[0]; want[1] = turned[1];
                want[2] = turned[2]; want[3] = turned[3];

                // Diagnostic, 2026-09-03: a real test reported a full extra
                // spin, consistently to the left, before settling at the
                // target angle - direction consistency argues against a
                // random double-cover sign flip (which the continuity fix
                // above already guards against regardless) and toward a
                // real logic bug in this composition. Logging the resulting
                // yaw directly, throttled, so the actual path (short arc to
                // the target vs. the long way around) is visible rather
                // than inferred from how it looks in the headset.
                // Per-object throttle - see the matching comment on the
                // [look back baseline] log above; this shared a single
                // static with the same starvation bug.
                static uintptr_t lastLbLogObj[8] = {};
                static LONG64 lastLbLogMs[8] = {};
                int lbSlot = -1;
                for (int i = 0; i < 8; ++i) {
                    if (lastLbLogObj[i] == obj) { lbSlot = i; break; }
                    if (lbSlot < 0 && lastLbLogObj[i] == 0) { lbSlot = i; }
                }
                const LONG64 lbNow = static_cast<LONG64>(GetTickCount64());
                if (lbSlot >= 0 && lbNow - lastLbLogMs[lbSlot] >= 40) {
                    lastLbLogObj[lbSlot] = obj;
                    lastLbLogMs[lbSlot] = lbNow;
                    const float yawDeg =
                        atan2f(2.0f * (want[3] * want[1] + want[0] * want[2]),
                               1.0f - 2.0f * (want[0] * want[0] + want[1] * want[1])) *
                        (180.0f / 3.14159265358979f);
                    LOG("camhook: [look back yaw] obj=%p t=%.3f target=%.1f "
                        "resultYaw=%.1f",
                        reinterpret_cast<void*>(obj), t, CamHookLookBackAngleDeg, yawDeg);
                }
            }
        }

        if (CamHookKeepHead) {
            // The headset's tilt, taken out of the pose we reported and put
            // back on top of the levelled camera. Without this the camera is
            // levelled absolutely and looking up does nothing.
            // Is this head this frame's? The hook runs inside the engine's
            // camera build, which may happen before the engine asks us for the
            // pose - in which case what we are about to apply is one frame old
            // while the pose we declare at submit is current. Counted, not
            // assumed.
            if (!HeadSampledThisFrame()) {
                InterlockedIncrement64(&g_staleHead);
            }
            InterlockedIncrement64(&g_levelCount);
            const XrQuaternionf& h = LastReportedHead();
            const float hq[4] = { h.x, h.y, h.z, h.w };
            float hyaw[4], hinv[4], hpr[4];
            YawOnly(hq, 0, hyaw);          // the reported pose is Y-up OpenXR

            // PUBLISH THE OPENXR -> GAME YAW.
            //
            // This is the one place in the codebase where the game camera's own
            // orientation and the head pose we handed the game are both in scope
            // at the same instant, already yaw-extracted, in code horizon lock
            // proves correct in the headset every session.
            //
            // The rigging work needs this to turn a controller position in play
            // space into a target in game world space. Three attempts to derive
            // it from the skeleton failed: the eyecamera element turned out to be
            // a BONE animated by the body, never slaved to the VR head, so there
            // was no fixed relationship to find. Taking the number from machinery
            // that already works beats deriving a fourth theory.
            //
            // Both spaces are Y-up, so the mapping is a single yaw - the angle
            // carrying the head's heading onto the camera's heading. Rotating
            // (0,0,-1) by each yaw-only quaternion and taking atan2 of the
            // horizontal parts is well conditioned by construction: a yaw-only
            // rotation cannot tilt a horizontal vector out of plane, which is
            // exactly the trap the skeleton probe fell into.
            {
                float cyaw[4];
                {
                    float sy = 0.0f, sp = 0.0f;   // ADDENDUM 2: in a cutscene the split yaw (exact near +-90 deg pitch)
                    if (CinPitchOn() && CinSplit(q, &sy, &sp)) {
                        AxisQuat(CamUpAxis() == 1 ? 2 : 1, sy, cyaw);
                    } else {
                        YawOnly(q, CamUpAxis(), cyaw);
                    }
                }
                // INV-620 (c), spec ADDENDUM 1 R6: when this anchor IS a cutscene's root, its ARRIVING camera has not been given
                // the head turn yet (that happens below, at the add site) - but the picture has. Put the same yaw on it here, so
                // the mapping is (anim - y0) and the hands stay on the controllers while the view turns. An anchor that is a copy
                // made FROM the root already carries it. Main menu: untouched. COST: one compare; on a hit one atan2 + one QuatMul.
                if (obj == g_cinRoot && TurnRootIsCutscene(TurnRoot())) {
                    float r = 0.0f;
                    XrQuaternionf s{};
                    if (CinRootYaw(&r, &s)) {
                        const int upIdx = CamUpAxis() == 1 ? 2 : 1;
                        float ry[4] = { 0.0f, 0.0f, 0.0f, cosf(r * 0.5f) };
                        ry[upIdx] = sinf(r * 0.5f);
                        float t[4];
                        QuatMul(cyaw, ry, t);
                        cyaw[0] = t[0]; cyaw[1] = t[1]; cyaw[2] = t[2]; cyaw[3] = t[3];
                        InterlockedIncrement(&g_cinRelFix);
                    }
                }
                // D3 (SPEC-HEAD-YAW-DECOUPLE-D): pair the camera with the head that went INTO it.
                // Decoupled, the engine was told a yaw-free head (C1) but its camera carries the real
                // head yaw through +4460 (D2) - so the head to subtract is the reported one with the
                // stripped yaw put back (same report, same frame). Pairing with the reported head alone
                // would leave the head yaw in `rel` and swing the arms (and D5's aim) with every turn.
                float hyawRel[4] = { hyaw[0], hyaw[1], hyaw[2], hyaw[3] };
                if (HeadYawDecoupleActive()) {
                    const XrQuaternionf& sy = LastStrippedHeadYaw();
                    const float syq[4] = { sy.x, sy.y, sy.z, sy.w };
                    float real[4];
                    QuatMul(syq, hq, real);
                    YawOnly(real, 0, hyawRel);
                }
                const float cfx = -2.0f * (cyaw[0]*cyaw[2] + cyaw[1]*cyaw[3]);
                const float cfz = -(1.0f - 2.0f*(cyaw[0]*cyaw[0] + cyaw[1]*cyaw[1]));
                const float hfx = -2.0f * (hyawRel[0]*hyawRel[2] + hyawRel[1]*hyawRel[3]);
                const float hfz = -(1.0f - 2.0f*(hyawRel[0]*hyawRel[0] + hyawRel[1]*hyawRel[1]));
                float rel = atan2f(cfx, cfz) - atan2f(hfx, hfz);
                while (rel >  3.14159265f) { rel -= 6.28318531f; }
                while (rel < -3.14159265f) { rel += 6.28318531f; }

                const ULONGLONG nowMs = GetTickCount64();
                const float     relDeg = rel * 57.2957795f;

                // Find or add this object.
                // MAIN-MENU ROOT EXCLUDED (2026-11-15, trace docs/traces/20261115_1715_mm_root_not_anchor.md; Jay: "Root never
                // anchor"): the capture below takes the ARRIVING rotation, and the root's arrives WITHOUT the head yaw camhook adds
                // to it - as anchor it makes the AFW rebuild gate refuse on every head turn and the HUD text double (RUN MS1, where
                // the page-switch catch's extra levelling won it the vote 130 vs 129). Its copies (eyes, AAABF0, AAAE40) carry the
                // yaw and stay candidates. In game g_mmTurnRoot is 0: one load + compare.
                // [LOADBEARING: ui_panel_main_menu_turn anchor] the main-menu root as yaw anchor = rebuild refused on head turns.
                const bool    mmRootNow = g_mmTurnRoot != 0 && obj == g_mmTurnRoot;
                YawCandidate* cand = nullptr;
                const LONG    nc   = g_yawCandCount;
                for (LONG i = 0; !mmRootNow && i < nc && i < kMaxYawCandidates; ++i) {
                    if (g_yawCand[i].obj == obj) { cand = &g_yawCand[i]; break; }
                }
                if (!mmRootNow && !cand && nc < kMaxYawCandidates) {
                    const LONG slot = InterlockedIncrement(&g_yawCandCount) - 1;
                    if (slot < kMaxYawCandidates) {
                        cand = &g_yawCand[slot];
                        cand->obj         = obj;
                        cand->hits        = 0;
                        cand->prevYawDeg  = relDeg;
                        cand->movementDeg = 0.0f;
                    }
                }
                if (cand) {
                    ++cand->hits;
                    cand->lastSeenMs = nowMs;
                    float d = relDeg - cand->prevYawDeg;
                    while (d >  180.0f) { d -= 360.0f; }
                    while (d < -180.0f) { d += 360.0f; }
                    cand->movementDeg += (d < 0.0f) ? -d : d;
                    cand->prevYawDeg = relDeg;
                }

                if (g_yawSettleStart == 0) { g_yawSettleStart = nowMs; }

                // Drop the anchor if it has gone quiet - a level load or a camera
                // swap should re-decide rather than keep publishing from an
                // object nothing is driving any more.
                if (g_yawAnchor) {
                    for (LONG i = 0; i < nc && i < kMaxYawCandidates; ++i) {
                        if (g_yawCand[i].obj == g_yawAnchor &&
                            nowMs - g_yawCand[i].lastSeenMs > 500) {
                            LOG("camhook: [yaw anchor] %p went quiet - re-deciding",
                                reinterpret_cast<void*>(g_yawAnchor));
                            g_yawAnchor      = 0;
                            g_yawSettleStart = nowMs;
                            InterlockedExchange(&g_yawCandCount, 0);
                            memset(g_yawCand, 0, sizeof(g_yawCand));
                            break;
                        }
                    }
                }

                // Decide once the settling window has passed. Busiest wins - the
                // rendered eyes are hit about twice as often as anything else.
                if (!g_yawAnchor && nowMs - g_yawSettleStart > 2000) {
                    LONG64 best = 0;
                    for (LONG i = 0; i < nc && i < kMaxYawCandidates; ++i) {
                        if (g_yawCand[i].hits > best) {
                            best        = g_yawCand[i].hits;
                            g_yawAnchor = g_yawCand[i].obj;
                        }
                    }
                    if (g_yawAnchor) {
                        LOG("camhook: [yaw anchor] chose %p from %d candidate(s) - "
                            "publishing the OpenXR->game yaw from it alone",
                            reinterpret_cast<void*>(g_yawAnchor),
                            static_cast<int>(nc));
                        for (LONG i = 0; i < nc && i < kMaxYawCandidates; ++i) {
                            LOG("camhook: [yaw anchor]   %s %p hits=%lld  avg "
                                "movement %.3f deg/sample",
                                (g_yawCand[i].obj == g_yawAnchor) ? "->" : "  ",
                                reinterpret_cast<void*>(g_yawCand[i].obj),
                                g_yawCand[i].hits,
                                g_yawCand[i].hits
                                    ? g_yawCand[i].movementDeg /
                                          static_cast<float>(g_yawCand[i].hits)
                                    : 0.0f);
                        }
                    }
                }

                // Only the anchor publishes, and ONCE PER FRAME.
                //
                // ===== WHY THIS IS A FRAME GATE AND NO LONGER A HEAD GATE. =====
                // 2026-09-14. The head gate was right about the disease and wrong
                // about the test.
                //
                // THE DISEASE, unchanged and still guarded against: this hook
                // runs SEVERAL TIMES PER FRAME (the rendered eyes are hit about
                // twice as often as anything else), and republishing the SAME
                // head against a NEWER camera each time is what produced the
                // original 3-8.5 deg/tick jitter. One publish per frame still
                // prevents that completely - which is all the head gate was
                // actually buying.
                //
                // WHAT THE HEAD TEST COST. It also refused to publish on frames
                // where the head quaternion was BIT-IDENTICAL to the previous
                // one - which is exactly what happens when you hold your head
                // still and turn with the STICK. The camera has moved, the
                // mapping genuinely should move with it, and nothing was
                // published. MEASURED 2026-09-14: the yaw arrived ~63 times a
                // second against ~85-96 fps - roughly 30% of frames dropped - in
                // jumps of ~2.8 deg, which moves the hand ~29 mm per step at a
                // 0.6 m controller offset. That staircase IS the stutter that
                // appeared the moment the arm started following this value
                // (rig_yaw_camera_blend). A low-pass cannot fix a staircase
                // without adding the lag it was measured to add; publishing at
                // the real rate removes it at source.
                //
                // THE PAIRING IS UNCHANGED AND STILL CORRECT. The camera the
                // engine builds this frame was built from the head we reported
                // LAST frame, so pairing this camera with LastReportedHead() -
                // which is still last frame's at this point - is the temporally
                // consistent choice. That is also why gating on
                // HeadSampledThisFrame() would publish never: it is stale 100%
                // of the time BY DESIGN, not by accident.
                //
                // THE FRAME BOUNDARY is observable without a new counter:
                // g_headSampled is cleared at frame start (ovr2xr.cpp:3206) and
                // set after LocateViews (ovr2xr.cpp:3237). This hook runs while
                // it is 0, so "the first publish since it was last 1" is exactly
                // once per frame.
                if (!g_yawAnchor || obj == g_yawAnchor) {
                    // >>> THE RESET CANNOT LIVE HERE. <<< v1 cleared the flag when
                    // this hook saw HeadSampledThisFrame()==1 - but this hook runs
                    // during the camera build, where it is 0 ONE HUNDRED PERCENT OF
                    // THE TIME. That is the same measurement this file already
                    // quotes ("100998 of 100998"). So the flag latched on the first
                    // publish and the yaw FROZE for the rest of the session: the
                    // arms stopped following the camera entirely. The boundary is
                    // only observable from the side that owns it, so
                    // BeginFrameIfNeeded() calls CamHookNoteFrameBoundary().
                    // NOT LocateOncePerFrame - that early-returns on
                    // g_relocateOnQuery == 0, which is the configured mode, and
                    // a re-arm placed there does not run at all. That mistake
                    // froze the yaw for a whole run.
                    const bool firstThisFrame =
                        (InterlockedExchange(&g_publishedThisFrame, 1) == 0);
                    if (firstThisFrame) {
                        // Kept for the diagnostics that read the last published
                        // head; it no longer gates anything.
                        g_lastHeadQ[0] = hq[0]; g_lastHeadQ[1] = hq[1];
                        g_lastHeadQ[2] = hq[2]; g_lastHeadQ[3] = hq[3];
                        InterlockedExchange(&g_haveLastHeadQ, 1);

                        float outYaw = rel;
                        // rig_yaw_stick_gate=3 (INV-292): the RAW value + its time, BEFORE the filter below. Three stores
                        // and one QPC per frame; nothing that reads g_openxrToGameYaw is affected.
                        {
                            LARGE_INTEGER rq;
                            QueryPerformanceCounter(&rq);
                            InterlockedIncrement(&g_ryxSeq);   // odd = writing
                            MemoryBarrier();
                            g_ryxY[0] = g_ryxY[1];
                            g_ryxT[0] = g_ryxT[1];
                            g_ryxY[1] = rel;
                            g_ryxT[1] = rq.QuadPart;
                            g_ryxCount = g_ryxCount + 1;
                            MemoryBarrier();
                            InterlockedIncrement(&g_ryxSeq);   // even = stable
                        }
                        if (CamHookYawSmoothingMs > 0.5f) {
                            // ===== QPC, NOT GetTickCount64. 2026-09-14. =====
                            //
                            // GetTickCount64 has ~15.625 ms resolution. At ~90 fps
                            // the interval between publishes is ~11 ms - SHORTER
                            // THAN THE CLOCK TICK - so dtMs frequently read 0,
                            // which gives a = 1 - exp(0) = 0 and the filter did
                            // not move AT ALL. It then republished the identical
                            // value and the consumer saw a held tick.
                            //
                            // THE NUMBER IS WHAT GIVES IT AWAY: 1000/15.625 =
                            // 64.0 Hz, and the measured update rate was 63/sec
                            // (median over 85 turning windows, 82% of ticks held).
                            // A rate that lands on a system constant to within
                            // 1.5% is the clock, not the signal. The staircase was
                            // first blamed on the head-change gate, which fit the
                            // symptom but not the number.
                            //
                            // QPC is already the layer's idiom for sub-frame
                            // timing - aimhook.cpp, camscan.cpp, overlay.cpp and
                            // ovr2xr.cpp all use it - so this is not a new
                            // dependency. Frequency is fixed at boot and cached.
                            static LARGE_INTEGER s_qpf = {};
                            if (s_qpf.QuadPart == 0) { QueryPerformanceFrequency(&s_qpf); }
                            LARGE_INTEGER qpc;
                            QueryPerformanceCounter(&qpc);
                            const LONGLONG t0 = qpc.QuadPart;
                            if (!g_haveYawFilter) {
                                g_yawFiltered = rel;
                                InterlockedExchange(&g_haveYawFilter, 1);
                            } else {
                                float dtMs = (s_qpf.QuadPart > 0)
                                    ? static_cast<float>(
                                          1000.0 * static_cast<double>(t0 - g_yawLastMs) /
                                          static_cast<double>(s_qpf.QuadPart))
                                    : 0.0f;
                                if (dtMs < 0.0f)   { dtMs = 0.0f; }
                                if (dtMs > 250.0f) { dtMs = 250.0f; }
                                const float a =
                                    1.0f - expf(-dtMs / CamHookYawSmoothingMs);
                                // Filter the DELTA, wrapped, so the +/-180
                                // boundary cannot make a steady value look like
                                // a full rotation.
                                float d = rel - g_yawFiltered;
                                while (d >  3.14159265f) { d -= 6.28318531f; }
                                while (d < -3.14159265f) { d += 6.28318531f; }
                                g_yawFiltered += a * d;
                                while (g_yawFiltered >  3.14159265f) { g_yawFiltered -= 6.28318531f; }
                                while (g_yawFiltered < -3.14159265f) { g_yawFiltered += 6.28318531f; }
                            }
                            g_yawLastMs = t0;
                            outYaw = g_yawFiltered;
                        }
                        g_openxrToGameYaw = outYaw;
                        InterlockedExchange(&g_haveOpenxrToGameYaw, 1);
                    }
                }
            }
            hinv[0] = -hyaw[0]; hinv[1] = -hyaw[1];
            hinv[2] = -hyaw[2]; hinv[3] = hyaw[3];
            QuatMul(hinv, hq, hpr);        // what is left is pitch and roll

            // Read them off, then re-apply about whichever local axes this
            // engine calls pitch and roll.
            const float sinp = 2.0f * (hpr[3] * hpr[0] - hpr[1] * hpr[2]);
            const float pitch = (fabsf(sinp) >= 1.0f)
                                    ? copysignf(1.5707963f, sinp)
                                    : asinf(sinp);
            const float roll = atan2f(2.0f * (hpr[3] * hpr[2] + hpr[0] * hpr[1]),
                                      1.0f - 2.0f * (hpr[0] * hpr[0] +
                                                     hpr[2] * hpr[2]));

            const float ph = (CamHookHeadPitchFlip ? -pitch : pitch) * 0.5f;
            const float rh = (CamHookHeadRollFlip ? -roll : roll) * 0.5f;
            float rp[4] = { 0.0f, 0.0f, 0.0f, cosf(ph) };
            rp[CamHookHeadPitchAxis] = sinf(ph);
            float rr[4] = { 0.0f, 0.0f, 0.0f, cosf(rh) };
            rr[CamHookHeadRollAxis] = sinf(rh);

            float tmp[4], out[4];
            // NO YAW IS ADDED HERE - option A's C2 did, and it stacked across chained camera copies and
            // left the engine culling against a body-facing camera (D-134). Under design D the head yaw
            // is already in the ENGINE's camera (+4460, D2), so YawOnly(engine) above carries it.
            //
            // ONE EXCEPTION, 2026-10-11 (ui_panel_main_menu_turn; INV-537 add. 1-3; Jay: "Build it with the switch"): THE MAIN
            // MENU. Its camera's heading does not come from a player's look, so no head yaw is in it and the picture rides the
            // head side to side. The yaw is added HERE AT ONE COPY ONLY - the engine's live -> cached-render-camera copy
            // (sub_18003E2B0, made every frame). The cull-side copy (AAABF0) and the eyes' copy (AAAE40) are both made FROM
            // that cached camera, so both inherit it and YawOnly(engine) above keeps it: the picture and the culling agree
            // (v1, Jay: no culling fault) and it cannot stack the way D-134's every-copy version did. v2 put it on AAABF0 -
            // the cull side only - and failed (INV-537 add. 5). uipin.cpp names the object each submit, only while the main
            // menu is up and the switch is on; 0 = nothing added (one compare). The copy hook levels that object on EVERY
            // copy (see dlvr_OnCameraCopied) - on a still page AutoShouldLevel would otherwise drop it after 1500 ms.
            // Axis and sign are option A's (backups/20260927_122040_pre_design_d camhook.cpp:9179), which turned the right way
            // in D-134's run. The real head yaw = the stripped one under the decouple, else the reported head's own.
            // INV-620 (c): a cutscene's root takes this same path (TurnRoot) with the yaw since the cutscene started (CinRootYaw).
            const uintptr_t turnRoot = TurnRoot();
            if (turnRoot && obj == turnRoot) {
                const bool cin = TurnRootIsCutscene(turnRoot);
                float yawRad = 0.0f;
                XrQuaternionf sy{};
                if (cin) {
                    if (CinRootYaw(&yawRad, &sy)) {
                        g_mmTurnSy[0] = sy.x; g_mmTurnSy[1] = sy.y; g_mmTurnSy[2] = sy.z; g_mmTurnSy[3] = sy.w;   // FULL yaw: declared
                        g_mmTurnSyMs = GetTickCount64();
                    } else {
                        g_mmTurnSyMs = 0;
                    }
                    g_cinAddedDeg = yawRad * 57.2957795f;
                    InterlockedIncrement(&g_cinAdds);
                } else if (HeadYawDecoupleActive()) {
                    sy = LastStrippedHeadYaw();                       // ONE snapshot: the yaw added and the yaw declared
                    yawRad = 2.0f * atan2f(sy.y, sy.w);              // OpenXR yaw about +Y
                    g_mmTurnSy[0] = sy.x; g_mmTurnSy[1] = sy.y; g_mmTurnSy[2] = sy.z; g_mmTurnSy[3] = sy.w;
                    g_mmTurnSyMs = GetTickCount64();
                } else {
                    yawRad = 2.0f * atan2f(hyaw[1], hyaw[3]);
                    g_mmTurnSyMs = 0;                                // the declaration refuses without the decouple anyway
                }
                // SIGN: v1 used option A's (negated when the flip is latched) and Jay saw it INVERTED at the main menu
                // (INV-537 add. 4) - so it is the opposite here. (CinRootYaw applies the same rule itself.)
                if (!cin && !HeadYawDecoupleFlipLatched()) { yawRad = -yawRad; }
                const int upIdx = CamUpAxis() == 1 ? 2 : 1;
                g_mmTurnEngineYaw = 2.0f * atan2f(want[upIdx], want[3]);   // the root's own heading BEFORE the head (log: must not creep)
                const float yh = yawRad * 0.5f;
                float ry[4] = { 0.0f, 0.0f, 0.0f, cosf(yh) };
                ry[upIdx] = sinf(yh);
                float withYaw[4];
                QuatMul(want, ry, withYaw);
                want[0] = withYaw[0]; want[1] = withYaw[1]; want[2] = withYaw[2]; want[3] = withYaw[3];
                if (!cin) {
                    g_mmTurnAdded = yawRad;
                    InterlockedIncrement(&g_mmTurnHits);
                }
            }
            // BUGGY STEADY COCKPIT (A, see kVehCockpit): the vehicle root loses the buggy camera's own term. want is YawOnly(engine)
            // here (no turn root), so yaw(want) = cy and want x ry(-term) has yaw cy - term = the look. The term is measured in this
            // same yaw convention, so no OpenXR sign rule enters. COST: one compare per copy; the term only on the root's copy.
            const uintptr_t vehRoot = g_vehRoot;
            if (kVehCockpit && !turnRoot && vehRoot && obj == vehRoot) {
                const int upV = CamUpAxis() == 1 ? 2 : 1;
                float term = 0.0f;
                if (VehCockpitTerm(2.0f * atan2f(want[upV], want[3]), &term)) {
                    const float yh = -term * (0.5f / 57.2957795f);
                    float ry[4] = { 0.0f, 0.0f, 0.0f, cosf(yh) };
                    ry[upV] = sinf(yh);
                    float withYaw[4];
                    QuatMul(want, ry, withYaw);
                    want[0] = withYaw[0]; want[1] = withYaw[1]; want[2] = withYaw[2]; want[3] = withYaw[3];
                }
            }
            // ADDENDUM 2: the cutscene camera's own pitch (latched at the root), after the yaw (and the root's head turn) and before
            // the head's pitch / roll - so downstream copies get the same pitch REPLACED, never the head's pitch stacked on it.
            if (cinPitch && g_cinAnimPitchOk && CutscenePitch) {   // comfort_cutscene_pitch: OFF = level (head pitch only)
                float pa[4], t2[4];
                AxisQuat(CamHookHeadPitchAxis, g_cinAnimPitch, pa);
                QuatMul(want, pa, t2);
                want[0] = t2[0]; want[1] = t2[1]; want[2] = t2[2]; want[3] = t2[3];
                InterlockedIncrement(&g_cinPitchCopies);
            }
            QuatMul(want, rp, tmp);
            QuatMul(tmp, rr, out);
            want[0] = out[0]; want[1] = out[1];
            want[2] = out[2]; want[3] = out[3];

            // The RENDERED view's yaw, once per anchor camera - [HEADTURN] reads it (spec section 5).
            if (g_yawAnchor && obj == g_yawAnchor) {
                const float vy = (CamUpAxis() == 1)
                    ? atan2f(2.0f * (want[3] * want[2] + want[0] * want[1]),
                             1.0f - 2.0f * (want[1] * want[1] + want[2] * want[2]))
                    : atan2f(2.0f * (want[3] * want[1] + want[0] * want[2]),
                             1.0f - 2.0f * (want[0] * want[0] + want[1] * want[1]));
                g_viewYaw = vy;
                InterlockedExchange(&g_haveViewYaw, 1);
                // [VIEWPOS] (diag_view_pos): the head PITCH camhook applied (from the reported head - yaw-free under D,
                // so pitch arrives by camhook's path while yaw arrives by the engine's) and the camera's WORLD POSITION
                // (the engine's - camhook writes rotation only). Plain stores; read once per frame at the frame boundary.
                g_viewPitchApplied = pitch;
                g_viewPos[0] = cam[3]; g_viewPos[1] = cam[7]; g_viewPos[2] = cam[11];
                g_viewAnchorObj = obj;
                // [RENDPOSE] R2 (diag_render_pose / declare_rendered_orientation): the ENGINE camera (q, before camhook)
                // + the head camhook used.
                if (CamHookRendPoseWanted()) {
                    const float cpos[3] = { cam[3], cam[7], cam[11] };
                    const XrVector3f& lhp = LastReportedHeadPos();
                    const float hpos[3] = { lhp.x, lhp.y, lhp.z };
                    RendPoseCapture(q, hq, cpos, hpos);
                }
            }
        }
    }
    MakeQuatContinuous(obj, want);
    MatFromQuat(want, nm);

    // Never hand the engine something that is not a rotation. A NaN here does
    // not produce a wrong picture, it produces a camera at no position at all,
    // and then a crash somewhere far away that looks unrelated.
    for (int i = 0; i < 9; ++i) {
        if (!(nm[i] > -1.001f && nm[i] < 1.001f)) {
            strncpy_s(g_reject, sizeof(g_reject), "computed matrix was not finite",
                      _TRUNCATE);
            LOG("camhook: reject obj=%p nm[%d]=%.4f - %s", reinterpret_cast<void*>(obj), i, nm[i],
                g_reject);
            return;
        }
    }

    // Only the nine rotation floats are touched. The fourth float of each row
    // is the position and is left exactly as the engine wrote it - editing the
    // matrix in place like this cannot move the camera, whatever the storage
    // convention turns out to be.
    const float px = cam[3], py = cam[7], pz = cam[11];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (CamHookTransposed) {
                cam[c * 4 + r] = nm[r * 3 + c];
            } else {
                cam[r * 4 + c] = nm[r * 3 + c];
            }
        }
    }
    // The position must come back out exactly as it went in. If it does not,
    // the translation is not in the fourth float of each row and this layout is
    // wrong - which would show up as the camera being somewhere else entirely.
    if (cam[3] != px || cam[7] != py || cam[11] != pz) {
        strncpy_s(g_reject, sizeof(g_reject),
                  "writing the rotation moved the position - wrong matrix layout",
                  _TRUNCATE);
        LOG("camhook: reject obj=%p - %s", reinterpret_cast<void*>(obj), g_reject);
        return;
    }
    memcpy(reinterpret_cast<void*>(obj + camOff), cam, sizeof(cam));
    g_reject[0] = '\0';
    // Main-menu latch (uipin.cpp, INV-535 add. 1): EVERY camera written, not only the yaw anchor's - the anchor is
    // re-decided for 2 s at a time at the main menu. COST: off (not at the main menu) one bool test per write; on, one
    // interlocked increment + a 48-byte copy + a tick read per write.
    if (g_wvWanted) { WrittenViewRecord(cam, obj); }
    if (g_yawAnchor && obj == g_yawAnchor) {   // fire_reticle_3d: the rendered view, as written
        InterlockedIncrement(&g_rvSeq);   // odd = writing
        MemoryBarrier();
        for (int i = 0; i < 12; ++i) { g_rvMtx[i] = cam[i]; }
        g_rvMs = GetTickCount64();
        MemoryBarrier();
        InterlockedIncrement(&g_rvSeq);   // even = stable
        // diag_view_body: the drawn body in THIS camera's frame, sampled here so both come from the same rendered
        // frame. First line returns while the setting is off.
        ViewBodySample(cam);
    }

    if (DiagEyeRotationCompare) {
        UpdateEyeDiag(obj, nm, px, py, pz);
    }

    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            g_probe.outAxes[i * 3 + k] = nm[k * 3 + i];
        }
    }

    // The inverse of an orthonormal [R | t] is [R^T | -R^T t]. That is the same
    // answer mtx34::invert_from produces here - it goes the long way round, via
    // cofactors and a divide by the determinant, because it also has to handle
    // matrices that are not rotations, but for this one the results agree and
    // the transpose cannot divide by a determinant that is nearly zero.
    float view[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            view[r * 4 + c] = nm[c * 3 + r];
        }
    }
    view[3]  = -(nm[0] * px + nm[3] * py + nm[6] * pz);
    view[7]  = -(nm[1] * px + nm[4] * py + nm[7] * pz);
    view[11] = -(nm[2] * px + nm[5] * py + nm[8] * pz);

    if (CamHookDerive) {
        memcpy(reinterpret_cast<void*>(obj + viewOff), view, sizeof(view));

        // combined = proj * view, the product mul4434_44 forms: a 4x4 times a
        // 3x4 whose implied fourth row is (0,0,0,1), which is why the fourth
        // column picks up proj's own fourth column and the others do not.
        const size_t projOff = camOff + 0x30;    // +0x40 -> +0x70
        const size_t combOff = camOff + 0x70;    // +0x40 -> +0xB0
        float proj[16];
        if (SafeReadLocal(obj + projOff, proj, sizeof(proj))) {
            float comb[16];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    float acc = (c == 3) ? proj[r * 4 + 3] : 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        acc += proj[r * 4 + k] * view[k * 4 + c];
                    }
                    comb[r * 4 + c] = acc;
                }
            }
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                if (!(comb[i] > -1e9f && comb[i] < 1e9f)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                memcpy(reinterpret_cast<void*>(obj + combOff), comb, sizeof(comb));
                ++g_probe.derived;
            } else {
                // Silent before this: view was already rewritten above, but
                // combined is left whatever it was - a mismatch between the
                // two that would show up as a visual glitch with no log
                // trace at all. proj[0]/[5] (roughly the horizontal/vertical
                // scale terms) and proj[10]/[11] (the depth-mapping terms)
                // are enough to tell a sane perspective matrix from garbage
                // at a glance without dumping all 16 floats.
                LOG("camhook: derived combined REJECTED (non-finite) obj=%p proj[0]=%.4f "
                    "proj[5]=%.4f proj[10]=%.4f proj[11]=%.4f", reinterpret_cast<void*>(obj),
                    proj[0], proj[5], proj[10], proj[11]);
            }
        } else {
            // Same mismatch risk as above, different cause: view rewritten,
            // proj unreadable this frame, combined left completely stale.
            LOG("camhook: could not read proj matrix at obj+%#zx (obj=%p) - combined left "
                "stale this frame", projOff, reinterpret_cast<void*>(obj));
        }
        return;
    }

    // ---- the old approach, kept only so the two can be compared -------------
    //
    // This is here because it is what produced every result so far, not because
    // it is defensible: rotating the finished proj*view product is not the same
    // operation as rotating the view inside it, and the fourth row - where the
    // perspective divide lives - is never updated at all.
    if (CamHookWriteCombined) {
        float inv[4] = { -q[0], -q[1], -q[2], q[3] };
        float d[4];
        QuatMul(inv, want, d);          // d = q^-1 * want
        float dm[9];
        MatFromQuat(d, dm);

        const size_t combOff = camOff + 0x70;   // +0x40 -> +0xB0
        float c4[16];
        if (SafeReadLocal(obj + combOff, c4, sizeof(c4))) {
            float out[16];
            memcpy(out, c4, sizeof(out));
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    float acc = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        acc += CamHookCombinedPre ? dm[r * 3 + k] * c4[k * 4 + c]
                                                  : c4[r * 4 + k] * dm[k * 3 + c];
                    }
                    out[r * 4 + c] = acc;
                }
            }
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                if (!(out[i] > -1e9f && out[i] < 1e9f)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                memcpy(reinterpret_cast<void*>(obj + combOff), out, sizeof(out));
            }
        }
    }

    if (CamHookWriteView) {
        memcpy(reinterpret_cast<void*>(obj + viewOff), view, sizeof(view));
    }
}

long long CamHookStaleHeadCount() { return g_staleHead; }
long long CamHookLevelCount() { return g_levelCount; }

// ------------------------------------------------------ automatic locking ---

namespace {

// One row per camera the hook has seen recently. Fixed size, no allocation, no
// locks: this is touched from every job thread inside the copy.
struct AutoSlot {
    volatile uintptr_t obj;        // 0 = free
    float last[9];                 // the rotation as it arrived last time
    volatile LONG64 hits;
    volatile LONG64 lastSeenMs;
    volatile LONG64 lastMovedMs;
};

AutoSlot g_auto[kAutoSlots];
volatile LONG g_autoOverflow = 0;    // cameras that did not fit
uintptr_t g_autoVtable = 0;

// A camera counts as live if its matrix changed within this long. Generous
// enough to survive a paused frame, short enough that a camera which stops
// being driven is released before the next level needs the slot.
constexpr LONG64 kMovedWithinMs = 1500;
constexpr LONG64 kForgetAfterMs = 4000;

}  // namespace

// Runs inside the hook. Returns true if this object should be levelled.
//
// Deliberately does no I/O, takes no lock and allocates nothing; the slot table
// is raced between job threads on purpose. The worst a race can do is mislabel
// one camera as still or moving for one frame, and a wrong answer for one frame
// is invisible - whereas a lock in here would be a deadlock waiting to happen.
bool AutoShouldLevel(uintptr_t obj, const float* rot)
{
    if (!g_autoVtable) {
        return false;
    }
    uintptr_t vt = 0;
    if (!SafeReadLocal(obj, &vt, sizeof(vt)) || vt != g_autoVtable) {
        return false;     // not a CRendererCamera
    }

    const LONG64 now = static_cast<LONG64>(GetTickCount64());

    int slot = -1, free = -1;
    for (int i = 0; i < kAutoSlots; ++i) {
        const uintptr_t o = g_auto[i].obj;
        if (o == obj) {
            slot = i;
            break;
        }
        if (o == 0) {
            if (free < 0) {
                free = i;
            }
        } else if (now - g_auto[i].lastSeenMs > kForgetAfterMs) {
            // Gone: freed with the level it belonged to. Reclaim it, so a new
            // level's cameras always have somewhere to go.
            InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_auto[i].obj),
                                         0, static_cast<LONG64>(o));
            if (free < 0) {
                free = i;
            }
        }
    }

    if (slot < 0) {
        if (free < 0) {
            InterlockedIncrement(&g_autoOverflow);
            return false;
        }
        if (InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(&g_auto[free].obj),
                static_cast<LONG64>(obj), 0) != 0) {
            return false;      // another thread took it; next frame will settle
        }
        slot = free;
        memcpy(g_auto[slot].last, rot, sizeof(g_auto[slot].last));
        g_auto[slot].hits = 0;
        g_auto[slot].lastMovedMs = 0;
        // A genuinely new CRendererCamera-vtable object just started arriving
        // at the hook - the auto-mode equivalent of "locked" in the old
        // manual-picking UI, which never logs anything in this mode. Exactly
        // the signal needed to tell "the same camera got an unusual rotation
        // written to it" apart from "a different camera object took over" -
        // e.g. a genuinely separate look-back/over-the-shoulder camera vs.
        // the same first-person camera just facing a different way.
        LOG("camhook: new object entered auto-tracking, slot %d, obj=%p", slot,
            reinterpret_cast<void*>(obj));
    }

    AutoSlot& a = g_auto[slot];
    InterlockedIncrement64(&a.hits);
    a.lastSeenMs = now;

    // Full-table snapshot, 2026-09-03: the look-back "spin" investigation
    // needs to know which currently-tracked objects are genuinely driven
    // every frame (real render-critical cameras) versus occasional/
    // secondary ones, since per-object yaw-rate heuristics alone were not
    // enough to tell them apart. Throttled globally (not per-object) since
    // this is a whole-table dump, triggered by whichever object happens to
    // call this function when the interval elapses - which object that is
    // does not matter.
    {
        static volatile LONG dumpLock = 0;
        static LONG64 lastDumpMs = 0;
        if (InterlockedCompareExchange(&dumpLock, 1, 0) == 0) {
            if (now - lastDumpMs >= 1000) {
                lastDumpMs = now;
                char line[768];
                int len = 0;
                len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE,
                                    "camhook: [table] ");
                for (int i = 0; i < kAutoSlots && len < static_cast<int>(sizeof(line)) - 64; ++i) {
                    if (!g_auto[i].obj) { continue; }
                    len += _snprintf_s(
                        line + len, sizeof(line) - len, _TRUNCATE,
                        "slot%d=%p(hits=%lld,age=%lldms) ", i,
                        reinterpret_cast<void*>(g_auto[i].obj),
                        static_cast<long long>(g_auto[i].hits),
                        static_cast<long long>(now - g_auto[i].lastSeenMs));
                }
                LOG_PERIODIC("%s", line);
            }
            InterlockedExchange(&dumpLock, 0);
        }
    }

    // Compare against the matrix as it arrived last time, not against what we
    // wrote - otherwise levelling a camera makes it look like it is moving and
    // every camera qualifies.
    float diff = 0.0f;
    for (int i = 0; i < 9; ++i) {
        diff += fabsf(rot[i] - a.last[i]);
    }
    memcpy(a.last, rot, sizeof(a.last));
    if (diff > 1e-4f) {
        a.lastMovedMs = now;
    }
    return (now - a.lastMovedMs) < kMovedWithinMs;
}

// The main-menu root must be a CRendererCamera before it is levelled outside AutoShouldLevel's own decision (the same
// vtable test, so a stale or wrong pointer from uipin is refused, not written to).
bool MainMenuRootIsCamera(uintptr_t obj)
{
    uintptr_t vt = 0;
    return g_autoVtable && SafeReadLocal(obj, &vt, sizeof(vt)) && vt == g_autoVtable;
}

}  // namespace dlvr

// Called by the thunk AFTER the engine's own copy has run, with every volatile
// register already saved. Nothing here may touch a file, allocate, or take a
// lock: it runs on whichever job thread was copying a camera.
extern "C" void dlvr_OnCameraCopied(void* dst, const void* src)
{
    using namespace dlvr;   // src: read only by the main-menu page switch catch below
    InterlockedIncrement64(&g_calls);
    const uintptr_t obj = reinterpret_cast<uintptr_t>(dst);

    // RIGGING STAGE 2 was called from here, and that was a mistake: this
    // function fires ~35,000 times per second (measured - 5.2M calls in 132s,
    // i.e. ~300 camera copies per frame), so a bone write here ran 35 kHz and
    // hammered the recompute cascade. Moved into LevelCamera(), which runs
    // only for actively-driven cameras (~2 per frame), and the write itself
    // now carries its own rate guard. Left as a comment because "this hook is
    // not per-frame" is the kind of thing worth stating where someone would
    // otherwise make the same assumption.

    if (CamHookAuto) {
        // The hook decides for itself. Everything the engine is actually
        // driving arrives here every frame; everything it is not, never does.
        if (!HorizonLock()) {
            return;
        }
        __try {
            // MAIN-MENU PAGE SWITCH CATCH (see g_mmEngCull): this is sub_1802B4EF0's cull copy (AAABF0 <- the drawn view's cache)
            // and its source is not the named root = the engine switched page view this frame, and that cache was refreshed with
            // no head yaw. Name it the root now, give it the yaw (LevelCamera's root path), and repeat the engine's copy so the
            // cull side carries it too; the eyes' copy (AAAE40 <- the same cache) comes next and inherits it. src != root, and
            // sub_1802B4EF0 runs once per level frame, so one cache is never given the yaw twice between two refreshes.
            // COST: in game (root 0) one load + compare; at the main menu one compare per copy; the catch ~once per page switch.
            // [LOADBEARING: ui_panel_main_menu_turn page switch] without it the frame a page switch lands on is drawn with no head
            //      yaw but declared with it - the whole picture jumps by the head's yaw for one frame (INV-537 add. 10 (b)).
            const uintptr_t mmRoot = g_mmTurnRoot;
            const uintptr_t mmCull = g_mmEngCull, mmView = g_mmEngView;
            if (mmRoot && mmView && obj == mmCull) {
                const uintptr_t s = reinterpret_cast<uintptr_t>(src);
                if (s != mmRoot) {
                    uintptr_t view = 0, cache = 0;
                    if (SafeReadLocal(mmView, &view, sizeof(view)) && view &&
                        SafeReadLocal(view + kMmViewCache, &cache, sizeof(cache)) && cache == s && MainMenuRootIsCamera(s)) {
                        g_mmTurnRoot = s;
                        LevelCamera(s);
                        typedef void* (*CopyFn)(void*, const void*);
                        reinterpret_cast<CopyFn>(dlvr_CamCopyTrampoline)(dst, src);
                        InterlockedIncrement(&g_mmViewCaught);
                    } else {
                        InterlockedIncrement(&g_mmViewRefused);
                    }
                }
            }
            const size_t camOff = static_cast<size_t>(RttiMatrixOffset());
            float rot[9];
            const float* cam = reinterpret_cast<const float*>(obj + camOff);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    rot[r * 3 + c] = cam[r * 4 + c];
                }
            }
            // ui_panel_main_menu_turn v3 (INV-537 add. 6): AutoShouldLevel releases a camera whose ARRIVING matrix has not
            // changed for 1500 ms (kMovedWithinMs) - right for a camera nothing drives, wrong for the main menu's root on a
            // still page, where the engine re-copies the same matrix every frame and the head's yaw must still go in. So
            // the ONE object uipin names is levelled on every copy. AutoShouldLevel is still called first: it keeps the
            // object's table row and refuses anything that is not a CRendererCamera. In game the root is 0: one compare.
            const bool autoLevel = AutoShouldLevel(obj, rot);
            const uintptr_t turnRoot = TurnRoot();   // the menu's root, or a cutscene's (INV-620 (c))
            const uintptr_t vehRoot = g_vehRoot;     // the buggy's steady cockpit (kVehCockpit): levelled on every copy too
            if (autoLevel || (turnRoot && obj == turnRoot && MainMenuRootIsCamera(obj)) ||
                (vehRoot && obj == vehRoot && MainMenuRootIsCamera(obj))) {
                if (!autoLevel) { InterlockedIncrement(turnRoot && obj == turnRoot ? &g_mmTurnForced : &g_vcForced); }
                LevelCamera(obj);
                InterlockedIncrement64(&g_levelled);
            }
            // THE BUGGY LEAN: the root's translation + the head's world offset, AFTER LevelCamera (which leaves the position exactly as
            // it arrived), on this copy only - this callback runs right after the engine copied the live camera INTO the root, so the
            // add is on a fresh copy and never stacks. No log here (job thread): counters only, CamHookCutsceneTick logs them.
            // [LOADBEARING: buggy lean] without it the buggy's view never follows the head (INV-306's vehicle gate; CarBuggyCamera is
            //      rigid to the car).
            const uintptr_t leanRoot = g_leanRoot;
            if (leanRoot && obj == leanRoot && MainMenuRootIsCamera(obj)) {
                float d[3];
                if (VehLeanRead(d)) {
                    float* c = reinterpret_cast<float*>(obj + camOff);
                    c[3] += d[0];
                    c[7] += d[1];
                    c[11] += d[2];
                    InterlockedIncrement64(&g_vlApplied);
                } else {
                    InterlockedIncrement64(&g_vlStale);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_faulted, 1);
            HorizonLock() = false;
        }
        return;
    }

    int slot = -1;
    for (int i = 0; i < dlvr::kMaxHookTargets; ++i) {
        if (dlvr::g_targets[i] == obj) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    // Counted even when the levelling is switched off, so a locked object shows
    // whether the hook reaches it at all - separately from whether the maths
    // then does anything.
    InterlockedIncrement64(&dlvr::g_targetHits[slot]);
    if (!HorizonLock()) {
        return;
    }
    __try {
        LevelCamera(obj);
        InterlockedIncrement64(&g_levelled);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        HorizonLock() = false;
    }
}

namespace dlvr {
namespace {

void WriteAbsoluteJmp(uint8_t* at, const void* to)
{
    at[0] = 0xFF;                       // jmp qword ptr [rip+0]
    at[1] = 0x25;
    at[2] = 0x00;
    at[3] = 0x00;
    at[4] = 0x00;
    at[5] = 0x00;
    memcpy(at + 6, &to, 8);
}
constexpr size_t kJmpBytes = 14;

}  // namespace

bool CamHookInstall()
{
    if (g_installed) {
        return true;
    }
    HMODULE mod = GetModuleHandleA(kEngine);
    if (!mod) {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "%s not loaded", kEngine);
        return false;
    }
    // The bytes must still be the copy we disassembled. If neither the known
    // offset nor the signature produces them this fails loudly instead of
    // writing a jump into the middle of something else.
    char how[192] = "";
    g_camCopyRva = dlvr::ResolveSite(
        mod, kCamCopyHintRva, "8B 42 10 89 41 10", kCamCopyPats,
        static_cast<int>(sizeof(kCamCopyPats) / sizeof(kCamCopyPats[0])),
        how, sizeof(how));
    if (!g_camCopyRva) {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "could not find the camera copy (%s) - game updated?", how);
        LOG("camhook: %s", g_status);
        return false;
    }
    g_patched = reinterpret_cast<uintptr_t>(mod) + g_camCopyRva;

    g_trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    static_assert(kStolen + kJmpBytes <= 64, "trampoline too small");
    if (!g_trampoline) {
        strncpy_s(g_status, sizeof(g_status), "VirtualAlloc failed", _TRUNCATE);
        return false;
    }
    memcpy(g_saved, reinterpret_cast<void*>(g_patched), kStolen);
    memcpy(g_trampoline, g_saved, kStolen);
    WriteAbsoluteJmp(g_trampoline + kStolen,
                     reinterpret_cast<void*>(g_patched + kStolen));
    dlvr_CamCopyTrampoline = g_trampoline;

    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_patched), kStolen,
                        PAGE_EXECUTE_READWRITE, &old)) {
        strncpy_s(g_status, sizeof(g_status), "VirtualProtect failed", _TRUNCATE);
        return false;
    }
    uint8_t patch[kStolen];
    memset(patch, 0x90, sizeof(patch));   // nop whatever the jump does not fill
    static_assert(kJmpBytes <= kStolen, "jump does not fit in the stolen bytes");
    WriteAbsoluteJmp(patch, reinterpret_cast<void*>(&dlvr_CamCopyThunk));
    memcpy(reinterpret_cast<void*>(g_patched), patch, kStolen);
    VirtualProtect(reinterpret_cast<void*>(g_patched), kStolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_patched),
                          kStolen);

    g_installed = true;

    // Learn CRendererCamera's vtable once, here, so the hook can identify its
    // targets with a single compare. This walks RTTI inside the engine module -
    // it does not touch the heap, so it costs nothing like the object scan does.
    g_autoVtable = RttiVtableOf("CRendererCamera");
    LOG("camhook: auto target vtable %p (%s)", reinterpret_cast<void*>(g_autoVtable),
        g_autoVtable ? "CRendererCamera" : "NOT FOUND - auto mode will do nothing");

    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "hooked CCamera assignment at +0x%zX (%s)", g_camCopyRva, how);
    LOG("camhook: %s", g_status);
    return true;
}

bool CamHookInstalled() { return g_installed; }

}  // namespace dlvr
