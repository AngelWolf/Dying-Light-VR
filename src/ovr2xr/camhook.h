// Horizon lock, applied inside the engine's own camera update.
//
// Found by putting a hardware breakpoint on a live camera matrix and asking the
// CPU what wrote it: engine_x64_rwdi.dll+0x3EA0B, `mov [rcx+0x40], eax`, inside
// an unrolled struct copy starting at +0x3E9C0. That function copies a CCamera
// from rdx into rcx, field by field, beginning at +0x10 - which is exactly where
// CCamera's data starts, so it is the whole-object assignment, and it confirmed
// the macOS layout transfers to Windows unchanged.
//
// Hooking it solves the problem every previous attempt failed on. Writing from a
// LibOVR entry point was always either too early (the engine recomputed the
// matrix afterwards) or too late (rendering had finished), and writing
// continuously from a thread let the renderer see the matrix change mid-frame,
// which is what the graphical glitching was. Running at the end of this function
// is the one moment that is neither: the engine has just decided where the
// camera points, and nothing has read it yet.
//
// Nothing is fought for and nothing races. The engine writes, we adjust, the
// renderer reads.
#pragma once

#include <cstdint>

namespace dlvr {

// Installs a trampoline over the camera assignment. The first five instructions
// are three bytes each and none is RIP-relative, so fifteen bytes relocate
// cleanly - a twelve-byte absolute jump fits with room to spare.
bool CamHookInstall();
bool CamHookInstalled();

// Which objects get levelled.
//
// More than one, because each eye is its own CRendererCamera: locking a single
// object levels half of stereo and leaves the other eye where the engine put
// it. Still not all of them - the assignment runs for shadow, reflection and
// cutscene cameras too, and tilting those would break things that are not the
// player's view.
//
// A small fixed array rather than a container: this is read inside the hook, on
// an engine thread, and must not allocate or take a lock to answer.
constexpr int kMaxHookTargets = 8;   // g_targets (the manual list; nothing fills it in BETA)

// Two things about the matrix that cannot be settled from outside, and which
// produce a teleported, warped camera when guessed wrong rather than a
// slightly-off one.
//
//   writeView  also rewrite the view matrix at +0x10 from the levelled rotation.
//              Off by default now: it was on, and it is the likelier cause of
//              the camera ending up somewhere else entirely, because it is the
//              only place we synthesise a whole matrix from an assumption about
//              its form rather than editing one in place.
//   transposed treat the stored 3x3 as column-major. Reading it the wrong way
//              round means the quaternion is the inverse rotation, and levelling
//              that and writing it back is a warp, not a correction.
// Which of the camera's matrices to change.
//
// Writing +0x40 alone moved the culling and left the picture alone, which says
// the renderer does not read it - it reads something derived. CCamera holds
// four matrices and the macOS accessors name them all:
//
//     +0x10  view        (world -> camera)
//     +0x40  camera      (camera -> world, position in the last column)
//     +0x70  projection
//     +0xB0  combined    (view and projection already multiplied together)
//
// The combined matrix is the one a renderer hands to shaders, and the engine
// built it before our hook ran. Changing the camera without it leaves the two
// disagreeing: the frustum moves, so things get culled, and the image does not.
//
// So the same rotation is applied to whichever of them is enabled. Combined is
// a 4x4 and only its upper-left 3x3 is touched, which leaves the projection and
// the translation untouched.
// Put the headset's own pitch and roll back after levelling.
//
// Levelling the camera outright removes ALL tilt, the headset's included, so
// looking up stops working - which is not horizon lock, it is a camera bolted
// to the floor. What we want is the game's tilt gone and yours kept:
//
//     new = yaw(camera) . (headset pitch and roll)
//
// The yaw of the whole camera is kept, so the game can still turn you; only its
// pitch and roll are dropped, and the headset's are re-applied on top. The head
// orientation is the one we handed the engine, so it already includes the
// vertical re-centre for the lens - nothing here needs to know about that.
//
// Which of the camera's local axes are pitch and roll is a property of the
// engine, so both are selectable. Same approach as the up axis, which is what
// finally made levelling work.
extern bool CamHookKeepHead;
extern int CamHookHeadPitchAxis;    // 0 = X, 1 = Y, 2 = Z
extern int CamHookHeadRollAxis;
extern bool CamHookHeadPitchFlip;
extern bool CamHookHeadRollFlip;

// "Look back": the game's own Y/Triangle-bound action does not actually turn
// the camera at all (confirmed - traced this whole session, see CLAUDE.md),
// so this is a synthetic override, not a fix for existing behaviour. Eases a
// yaw offset in while Y is held (gated on also sprinting and not being in a
// vehicle) and back out on release, at the same rate either way. Applied
// purely to the rendered copy of the camera inside LevelCamera - never to
// the game's own source camera/facing state - specifically so it cannot drag
// movement direction around the way the reverted head-turn attempt did.
extern bool CamHookLookBack;
extern float CamHookLookBackEaseMs;  // time to fully engage/disengage
extern float CamHookLookBackAngleDeg;  // how far to turn at full engagement

// Diagnostic, 2026-09-04: picking the real sprint state up from
// MovementModule (gamedll's own class - m_MoveStyle/m_CurrentMoveStyle field
// offsets found by tracing their RTTI field registration statically, see
// FindPlayerMoveStyleViaWeaponAnchor()'s own comment in camhook.cpp), rather
// than the IsPlayerSprintingToggle() stopgap. Off by default - several
// earlier approaches under this same flag (a full-process heap-pool scan, a
// pointer chase from PlayerFppVis, a global-singleton vehicle-check chain, a
// broad "any human entity" forward heap scan with a live per-frame field-diff
// monitor) were all tried and superseded across one long investigation - see
// CLAUDE.md for the full history of what was ruled out and why. The current
// approach anchors on WeaponFireControllerRight (a confirmed-reliable,
// player-specific, already-used-elsewhere live object) instead of scanning
// the whole heap, so it is far cheaper - but still does one bounded heap scan
// when first enabled, so treat this as an investigation flag, not something
// to leave on by default.
extern bool DiagPlayerMovementModule;

// Investigation, 2026-09-04: one-shot attempt to disable the game's own
// physics/navmesh debug-render subsystem (ILevel::SetPhysicsDebugRenderMode,
// mode=0), on the theory it's leaking into VR's eye viewports and causing a
// VR-only ground-plane line/dot rendering artifact. Safe to leave off by
// default - does nothing unless enabled, and gives up after one successful
// call (or one failed bounded scan).
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagKillPhysicsDebugRender = false;
#else
extern bool DiagKillPhysicsDebugRender;
#endif

// Investigation, 2026-09-04 continued: the DebugRenderer kill attempts above
// had zero effect - new evidence (the artifact disappears when you walk up
// to it) points at a far-LOD/streaming placeholder instead. Experimental:
// feeds ILevel::SetSimulationCamera the mod's own real, live head pose every
// frame, on the theory the engine's own streaming/LOD reference camera is
// separate from the render camera this mod already drives, and nothing has
// ever told it where VR's headset actually is. Unconfirmed - see CLAUDE.md.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagFeedSimCamera = false;
#else
extern bool DiagFeedSimCamera;
#endif

// Investigation, 2026-09-04 continued: purely observational, touches no
// engine state (unlike the diagnostics above). Compares the final rotation
// matrix actually written to the two highest-hit-rate tracked cameras (the
// real stereo eyes, identified the same way the "Look back" investigation
// did) every ~2s, logging the angular difference between them plus their
// separation distance. Real stereo displays should show ~0 degrees (eyes
// differ only by translation/IPD, never rotation) - a nonzero, especially
// growing-with-distance, angle would directly confirm or rule out a stereo-
// rotation-mismatch theory for the VR-only ground-plane artifact.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagEyeRotationCompare = false;
#else
extern bool DiagEyeRotationCompare;
#endif

// [EYEAXIS], 2026-09-27 (INV-260): the engine places each eye along the SOURCE camera's
// right axis (sub_1802E5530), after camhook has rotated the eye copies. This measures,
// once per frame, whether the resulting stereo baseline follows the rendered view.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagEyeAxis = false;
#else
extern bool DiagEyeAxis;
#endif

// [VIEWPOS], 2026-09-27: per-frame view-vs-head timing (yaw and pitch lags) and camera-position-vs-tracked-head.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagViewPos = false;
#else
extern bool DiagViewPos;
#endif
void CamHookViewPosTick();

// [RENDPOSE], 2026-10-01 (SPEC-RENDERED-ORIENTATION Stage 1, log-only): pair each submitted frame with the head yaw design D
// added into its camera, and log what declaring that rendered orientation would correct. Push: camyaw::Hook (boneblend,
// game thread) after it adds h into +4460 - afterDeg = +4460 after the add, sy = the stripped head-yaw quaternion (x,y,z,w).
// Submit: ovr2xr.cpp at the projection-view build - the declared eye orientations (x,y,z,w) before PitchPose.
extern bool DiagRenderPose;
// Stage 2 (2026-10-01): declare_rendered_orientation, LIVE. Submit returns true only when it is ON and every pairing check
// passed for this frame; outRendered = the XR orientation the frame was rendered with (eye 0, before PitchPose). echo = the
// game's RenderPose[0] orientation (x,y,z,w) - the capture whose reported head equals it is the image's.
extern bool DeclareRenderedOrientation;
bool CamHookRendPoseWanted();   // diag_render_pose || declare_rendered_orientation: feed the ring and the captures
// VR Comfort tab status: 0 = the setting is off; 1 = a frame was declared within the last second; -1 = on but not declaring,
// *why = the last named reason (static string, never null when -1).
int CamHookRendDeclStatus(const char** why);
void CamHookRendPosePush(float afterDeg, float hDeg, const float sy[4]);
bool CamHookRendPoseSubmit(bool posesValid, const float eye0[4], const float eye1[4], const float echo[4], bool haveEcho,
                           bool useRenderPose, float outRendered[4], long long frameIndex);
// [RENDTAG] (INV-325, log-only): called by boneblend's pass-through detour on rd3d11's pose recorder (sub_18003B9B0) with
// the frameIndex it is about to record, on the recording thread. Returns at once while diag_render_pose=0.
void CamHookRendTagRecord(long long frameIndex);

// SPEC-HEAD-YAW-DECOUPLE (R10). The raw settings - read them through
// HeadYawDecoupleActive() / HeadYawDecoupleFlipLatched(), which are latched once
// at startup; these two change live when the overlay edits them.
extern bool HeadYawDecouple;
extern bool HeadYawDecoupleFlip;
bool HeadYawDecoupleFlipLatched();

// camhook's yaw-only extraction, exported so the decouple's C1 (ovr2xr.cpp)
// reuses it instead of a third copy. q/out are {x,y,z,w}; upAxis as
// CamUpAxis() (0 = Y-up about +Y, which is also OpenXR's).
void CamYawOnly(const float q[4], int upAxis, float out[4]);

// The RENDERED view's yaw (radians, about the engine's up axis), from camhook's
// final rotation of the yaw-anchor camera, published once per frame with the
// OpenXR->game yaw. false until published. [HEADTURN] reads it (spec section 5).
bool CamHookGetViewYaw(float& outRadians);
// THE BUGGY LEAN (2026-11-10, vehlean_logic.h): the local player is in a vehicle (published once per submit by
// CamHookCutsceneTick), and the rig's head offset in the world (metres, the arm solve's own controller map) that camhook adds to
// the vehicle view's root camera. COST: one load / one seqlock write.
bool CamHookLocalInVehicle();
void CamHookPublishVehicleHeadOffset(const float d[3]);

// Investigation, 2026-09-04 continued: live, bounded (repeating 15s windows,
// ~15 minutes total then auto-stops - mirrors every other WatchExecStart-
// based diagnostic tool's own safety discipline), execution-breakpoint
// watch on CLineListEx's own "create and add a new line" factory (gamedll
// RVA 0x13304f0, found via vtable slot 2 - see the Ghidra scripts/
// re_analysis output for the class-hierarchy trace). Logs every distinct
// (RCX,RDX) pair that reaches it per window, with RCX's real RTTI class
// name resolved. The most directly-targeted lead yet for who actually
// creates the ground-plane line/dot artifact - arm this, then look at the
// artifact any time within the repeating collection window.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagWatchLineAdd = false;
#else
extern bool DiagWatchLineAdd;
#endif

// Phase 1 of the VR draw-distance investigation: READ-ONLY probe that calls
// the engine's own exported ILevel::GetObjectsVisibilityRange() and logs it.
// Run once in VR and once flatscreen on the same save - if the values differ
// there is a real VR-conditional override; if they match, the divergence is
// in how the value is applied (screen-relative cull size vs our wider
// frustum). Mutates nothing engine-side.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagVisibilityProbe = false;
inline constexpr bool DiagDamageWatch = false;
#else
extern bool DiagVisibilityProbe;
extern bool DiagDamageWatch;
#endif
// R5 step 3: the melee hit-record probe. Read-only, no slot, no DR, flat-valid.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMeleeRecordProbe = false;
#else
extern bool DiagMeleeRecordProbe;
#endif
// R5 step 4: the melee pose observation hook at gamedll+0xDC1C90. Read-only.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMeleePoseProbe = false;
#else
extern bool DiagMeleePoseProbe;
#endif
extern float DamageWatchRvaOverride;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMeleeElemProbe = false;
#else
extern bool  DiagMeleeElemProbe;
#endif
extern float MeleeForceElem;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMeleeBoneCensus = false;
#else
extern bool  DiagMeleeBoneCensus;
#endif
extern float MeleeCensusMs;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagLastHitLocReadWatch = false;
#else
extern bool  DiagLastHitLocReadWatch;
#endif
extern float LocReadWindowMs;

// Kicks off the read-only visibility-range probe (once per process). Called
// from dlvr_FilterScePad, NOT from the camera hook: scePadReadState is polled
// in both VR and flatscreen, whereas the camera hook only ever installs in VR
// - and this probe exists specifically to compare the two.
void ProbeVisibilityRangeOnce();

// Texture-streaming budget (INV-580; rewritten 2026-10-16). Called every scePad poll: sends stream_budget_mb (fixed MB, 0 =
// leave alone, -1 = AUTO from DXGI free VRAM) through SRendererAPI slot 143 case 5 + CRenderer slot 20, re-sent on change,
// and runs the STREAMBUDGET probe when diag_stream_budget is on. See the function's own comment in camhook.cpp.
extern int StreamBudgetMB;
extern int StreamBudgetReserveMB;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagStreamBudget = false;
#else
extern bool DiagStreamBudget;
#endif
void ApplyStreamingBudgetOnce();
// The engine's streaming used / budget, bytes (rd3d11+0x7BD18 / +0x784B8). false if unreadable. Plain reads, any thread.
bool StreamBudgetSnapshot(unsigned long long* usedBytes, unsigned long long* budgetBytes);
// Cumulative microseconds of streaming WORK (the per-frame streaming update + stream-in creations outside it), from the
// STREAMAPPLY detours; 0 and StreamTimerLive() false while they are not installed (diag_stream_budget off / refused).
unsigned long long StreamWorkMicros();
bool StreamTimerLive();
// S1 shadow cascade stagger (INV-585): shadow_cascade_stagger / shadow_stagger_first. NOTHING of it runs at 0 (Jay 2026-10-16: "If
// it's set to 0, it quite literally does nothing at all"): no per-poll / per-frame call. ShadowStaggerStart() is called only by the
// config load and by a menu edit of shadow_cascade_stagger, and only when the value is >= 2; it starts ONE thread (once per process)
// that installs the engine +2B7070 detour (after its refusal checks) and prints the SHADOWSTAGGER lines.
extern int ShadowCascadeStagger;
extern int TextureAniso;   // texture_aniso: the bootstrap applies it (dlssflat.cpp); restart to apply
// Map fixes, ported from DEV 2026-11-07 (all load-time; each needs its dlvr\*.bin and refuses by name without it).
extern bool RoadDrape;     // road_drape: roaddrape.cpp, the farm road drape (INV-692); read at level load
void StartRoadDrapeOnce();
extern bool EdgeStrip;     // edge_strip: edgestrip.cpp, the asphalt edging strips (INV-694); read at pack load
extern bool TerrainBerm;   // terrain_berm: edgestrip.cpp, the farm terrain raise (INV-694); read at pack load
void StartEdgeStripOnce();
extern bool DecalSeat;      // decal_seat: decalseat.cpp, decals seated on their surface (INV-696); read at map load
extern int DecalSeatGapUm;  // decal_seat_gap_um: final gap in front of the surface, micrometres
extern int DecalSeatMaxMm;  // decal_seat_max_mm: largest move, mm
void StartDecalSeatOnce();
void StartDecalPlsOnce();   // decalpls.cpp: the map's DecalPLS entity decals, under decal_seat (INV-697 (12))
extern int ShadowStaggerFirst;
extern bool LensMask;            // F2 lens mask (research doc §1e); the bootstrap reads it through dlvr_LensMask
extern float LensMaskMarginDeg;
extern bool LensMaskTint;
extern bool DlssLensCrop;         // DLSS / FSR lens crop (docs/superpowers/specs/2026-11-11-dlss-lens-crop-design.md); read by the bootstrap through dlvr_LensGeom
extern float DlssLensCropMarginDeg;
extern bool DlssLensCropTint;
extern bool FovDlss;              // fixed foveated DLSS / FSR (docs/superpowers/specs/2026-11-11-foveated-dlss-design.md); read by the bootstrap through dlvr_FovGeom
extern float FovWidth;
extern float FovHeight;
extern float FovHeightOffset;
extern float FovXOffset;
extern float FovRoundness;
extern float FovTransition;
extern float FovSupersample;
extern bool FovPeriphery;
extern int FovPeripheryPreset;
extern float FovPeripheryScale;
extern bool FovBorder;
void ShadowStaggerStart();

// Live-tunable LOD / visibility override. These call the engine's own
// exported ILevel setters roughly once a second, so overlay sliders take
// effect immediately and survive level loads. Values <= 0 (or < 0 for the
// cull knobs) mean "leave that one alone". See the lodoverride namespace in
// camhook.cpp for the full reasoning.
extern bool LodOverride;
extern float LodObjectsRange;
extern float LodMeshCullSize;
extern float LodMeshCullDist;
extern float LodFxRange;
void StartLodOverrideOnce();
// Raster-occlusion A/B (2026-10-03): sets the current scene CLevelView's
// ForceViewNoRastOcclusion byte through the engine's own exported setter.
// See the rastocc namespace in camhook.cpp.
extern bool DiagNoRastOcclusion;
void StartRastOccOnce();
// VR jitter-execute A/B (2026-10-14, INV-561): NOPs the COculusHelper test in CmdPPFX_SMAAScreenJitter::Execute so the
// executed jitter flag matches the job snapshot in VR. See the jitvr namespace in camhook.cpp.
extern bool DiagJitterVrExec;
void StartJitVrOnce();
// Clip-far read + A/B (2026-10-03, INV-368): reads the scene camera's clip
// far through ILevel::GetViewCamera / IBaseCamera::GetClipFar and, while
// clip_far_scale != 1, sets it with IBaseCamera::SetClipFar. See the clipfar
// namespace in camhook.cpp.
extern bool DiagClipFar;
extern float ClipFarScale;
void StartClipFarOnce();
// vr_near_clip_cm (INV-549): the VR first-person near plane in cm; 9.76 = the game's own value (nothing written). Live.
extern float VrNearClipCm;
void StartNearClipOnce();
// AFW alternate-eye A/B (2026-10-03, INV-370): while afw_alt_eye is on, frame N skips eye (N & 1)'s
// Scene replay (CRenderContext slot 45, layer 21). See the afwalt namespace in camhook.cpp.
extern bool AfwAltEye;
// afw_enable (D-184): the ONE user switch, "Alternate Frame Warp" (VR Comfort). afw_alt_eye and afw_rebuild follow it.
extern bool AfwEnable;
// Data3 check: false while the bootstrap does NOT report this build's Data3.pak (DlvrData3State != 1) - SyncFollowers then holds
// afw_alt_eye and afw_rebuild off whatever afw_enable says. Logs once per verdict change. Cheap after the first final verdict.
bool AfwData3Allowed();
// The AFW rebuild gate's eye-position check needs this submit's eye separation (metres), handed in before the pairing.
void CamHookAfwNoteEyeSep(float sepM);
void StartAfwAltEyeOnce();
// afw_held_eye_pose: under afw_alt_eye, the submit declares the HELD eye with its own last rendered orientation.
// CamHookAfwSubmitSkipEye: 0/1 held eye of the image just paired, -1 both rendered, -2 unknown (see camhook.cpp).
extern bool AfwHeldEyePose;
int CamHookAfwSubmitSkipEye();
// afw_rebuild's matrix sink: the bootstrap pushes each GBuffer-pass view-projection (dlvr_AfwPushVp).
void CamHookAfwPushVp(const float vp[16]);
// afw_rebuild (AFW step 4b): the held eye is rebuilt at submit by the bootstrap (DlvrAfwRebuild) when this image's gate
// passes. CamHookAfwRebuildGate: 0 = this image may be used (valid right after CamHookRendPoseSubmit); the residual is
// the GPU-vs-captured camera heading error it measured. afw_rebuild_tint: the debug tint.
extern bool AfwRebuild;
extern bool AfwRebuildTint;
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagAfwRebuildCheck = false;
#else
extern bool DiagAfwRebuildCheck;   // diag_afw_rebuild_check: DlvrAfwRebuild flag bit 1 (the bootstrap's [AFWRB] CHECK)
#endif
extern bool AfwRbFarRule;          // afw_rb_far_rule: flag bit 2 (depth comparisons clamped at the far limit)
extern bool AfwRbStale;            // afw_rb_stale: flag bit 3 (the stale test - moving objects)
extern bool AfwRbEdgeRescue;       // afw_rb_edge_rescue: flag bit 4
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagAfwRebuildRules = false;
inline constexpr bool DiagAfwFgMotion = false;
#else
extern bool DiagAfwRebuildRules;   // diag_afw_rebuild_rules: flag bit 5 (the bootstrap's [AFWRB] RULES counter)
extern bool DiagAfwFgMotion;       // diag_afw_fg_motion: flag bit 7 + depth-wanted bit 2 (the bootstrap's [AFWMOT] counters)
#endif
// diag_afw_heldmap: flag bit 8 + depth-wanted bits 3 / 6 (the bootstrap's [AFWHM] held-pose redraw, [AFWVEH] the buggy probe).
// A real setting in BETA too since 2026-10-29 (it gates [AFWVEH]; was constexpr false under DLVR_BETA_LEAN).
extern bool DiagAfwHeldMap;
extern bool AfwRbFgMove;           // afw_rb_fg_move: flag bit 9 + depth-wanted bit 3 (stage 2b: held body texels follow the map)
extern bool AfwRbFgMoveWeapon;     // afw_rb_fg_move_weapon: flag bit 18 + depth-wanted bit 4 (INV-464: the held weapon in the map)
extern bool AfwRbFgEdgeRing;       // afw_rb_fg_edge_ring: flag bit 19 (INV-465: the hand's old edge is not held world)
extern bool AfwRbFillNear;         // afw_rb_fill_near: flag bit 20 (INV-465: the fill extends the nearest background, away from the hand)
extern bool AfwRbSeedIndexed;      // afw_rb_seed_indexed: flag bit 21 (A1a: the seed mesh drawn indexed - same image, less vertex work)
extern bool AfwRbFgMaskEngine;     // afw_rb_fg_mask_engine: dlvr_AfwDepthWanted bit 5 (mask with the engine's PS / depth write + alpha-tested draws out of the held-pose map)
extern float AfwMvObjectScale;     // afw_mv_object_scale: the bootstrap's MV pass scales the engine object vector by it under AFW
extern bool AfwRbEdgeHold;         // afw_rb_edge_hold: DlvrAfwRebuild flag bit 15 (keep own pixel where "fresh nearer" is edge position)
extern int  AfwRbEdgeHoldRadius;   // afw_rb_edge_hold_radius: flag bits 16-17, the hold's search radius in depth texels (1-3)
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagAfwTrailSrc = false;
inline constexpr bool DiagAfwCapture = false;
#else
extern bool DiagAfwTrailSrc;       // diag_afw_trail_src: flag bit 10 (the bootstrap's [AFWTRAIL] counter)
extern bool DiagAfwCapture;        // diag_afw_capture: flag bit 11 (the bootstrap's [AFWCAP] frame capture, armed on a rising edge)
#endif
extern bool SubmitDrawnImage;      // submit_drawn_image: submit the image each replayed frame drew (U-52 / INV-429 fix, ovr2xr.cpp)
extern bool DiagHiccup;            // diag_hiccup: [HICCUP] per-frame hiccup / double-image events (ovr2xr.cpp HiccupFrame)
// diag_afw_fg_probe ([AFWFG], AFW item 2 stage 0): boneblend hides the player's mesh elements one at a time and the
// bootstrap counts which draws vanish (dlvr_AfwFgProbePhase). NOT log-only: the arms and body blink while it is on.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagAfwFgProbe = false;
#else
extern bool DiagAfwFgProbe;
#endif
// afw_rb_fg_mask (AFW item 2 stage 1): boneblend publishes the foreground draw ranges and the bootstrap redraws each
// matching draw into a depth-valued mask kept with the per-image depth copy. Stage 1 only SHOWS it (yellow in the tint).
extern bool AfwRbFgMask;
// (afw_rb_fg_rules: REMOVED 2026-10-08, D-185 - flag bit 6 is never set.)
int CamHookAfwRebuildGate(float* residualDeg);
const char* CamHookAfwRebuildGateName(int code);
// The eye the current frame renders under afw_alt_eye, -1 = both (AFW off / not installed). Exported to the bootstrap
// as dlvr_AfwRenderedEye (ovr2xr.cpp) for the DLSS per-eye history.
int CamHookAfwRenderedEye();
void StartDamageWatchOnce();

// RIGGING STAGE 0 (2026-09-07): dump the player's skeleton - every element's
// index, name, PARENT and local-space offset, plus the EBones-id -> element
// mapping. Read-only; calls only const accessors and logs. Everything the
// arm-rigging work needs depends on this map, and the parent/local data
// specifically is what makes a chain-driven (non-mesh-breaking) approach
// possible - see the boneprobe namespace in camhook.cpp for why.
//
// Uses the single process-wide WatchExec slot, so every other diag_* that arms
// an exec watch must be OFF while this runs.
extern bool DiagBoneDump;

// rig_exec_client2 - move the bone probe off exec CLIENT 0 (DR1) onto CLIENT 1
// (DR0), so it can coexist with the melee trap. OFF by default: with it off,
// everything behaves exactly as it did before the two-client refactor, which is
// what makes a regression run possible.
extern bool RigExecClient2;
// Re-enables the concluded investigations' own logging (look-back baseline,
// continuity, yaw investigation, camera reject reasons). Off by default.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagCamhookVerbose = false;
#else
extern bool DiagCamhookVerbose;
#endif
// RIGGING STAGE 1: null round-trip. Reads each right-arm bone's local matrix,
// writes it back UNCHANGED, reads again and compares - answering (a) is the
// data live/animating, (b) does our mtx34 layout + calling convention
// round-trip, (c) does a write survive to the next pass. Writes identical
// bytes, so it cannot corrupt the pose. Implies the stage-0 capture.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagRigStage1 = false;
#else
extern bool DiagRigStage1;
#endif
// RIGGING STAGE 2: writes a FIXED wrong pose to r_forearm from the engine's own
// render thread (dlvr_OnCameraCopied), to find out whether a write survives the
// animation tick. Deliberately makes the arm look wrong - that is the signal.
// Log reports held vs clobbered counts. Implies the stage-0 capture.
extern bool DiagRigStage2;
extern float RigStage2AngleDeg;
// After any raw bone-matrix write, call IModelObject::GetAnimBind() then the
// virtual at IAnimBind+0x248 - the notify every engine caller of
// SetMeshElementsMatrices performs. Omitting it for three runs is the leading
// explanation for the body/weapon desync. On by default; turn OFF only to
// reproduce the un-notified behaviour deliberately.
extern bool RigNotifyAnimBind;
// Narrow test: write WORLD matrices instead of LOCAL. Evidence says the local
// path writes a representation the skinning does not read (data persisted, the
// notify landed, the arm still animated) while still dirtying the cache enough
// to cause per-eye rivalry. CorrectItemPosition drives this same skeleton via
// SetElementWorldMatrix every frame, so the world path is engine-proven.
extern bool RigStage2UseWorld;
// RIGGING STAGE 3 (read-only): dump the bone array at model+0x388 (stride 0x78)
// and the bone-index -> mesh-element lookup at model+0x408, to resolve the third
// numbering. HackPABlender's block mask is indexed by raw bone-array index, not
// mesh-element index, and that mapping is the hard prerequisite for path A.
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagRigStage3 = false;
#else
extern bool DiagRigStage3;
#endif
// RIGGING STAGE 4: the engine's own idiom end to end - clear IControlObject
// flag 4, BlockAnimation the right-arm bones (57/58/62), write the pose, notify
// via AnimBind vtable +0x38/+0x160. First stage where the write has a real
// chance of both holding AND rendering.
extern bool DiagRigStage4;
extern float RigNotifyWeight;
void StartBoneDumpOnce();
// Stage 4 tick - must be driven from the scePad poll (update phase), not from
// the camera hook (render phase). See RigStage4Tick's comment in camhook.cpp.
void RigStage4Tick();

// The player model captured by the stage-0 probe (*(PlayerFppVis+0x50), RTTI
// PlayerDI), or null if it has not run/succeeded yet. Exposed so the bone-blend
// probe can say WHICH of the skeletons being blended is the player - every
// human NPC shares this rig, so segment lengths alone cannot tell them apart.
const void* BoneProbeCapturedModel();

// Drop that captured model and re-arm the capture. For ONE caller: the solve
// path, when the model has stopped answering its accessors - which is what a
// player death does to it (INV-81). Rate-limited to one attempt per 5 s
// internally, because the caller runs per tick and a capture arms a hardware
// breakpoint across ~170 threads. Returns true only when a fresh capture was
// actually started, so the caller logs once rather than every tick.
bool BoneProbeRecapture();

// rig_player_live_guard (U-49 / INV-365): forget the captured model and entity at once - no cooldown, nothing
// re-armed. The caller follows with BoneProbeRecapture() until that starts a capture.
void BoneProbeDropModel();

// The entity that model hangs off - PlayerFppVis. Needed to reach the player''s
// IControlObject, whose world transform is a stable anchor where an animated
// bone is not.
uintptr_t BoneProbeCapturedEntity();

// The rotation from OpenXR play space to game world space, in radians about
// the up axis. Both spaces are Y-up, so a single yaw is the whole mapping.
// Measured live from the game camera's own orientation against the head pose we
// handed the game - the pair horizon lock already proves correct. False until a
// camera has been levelled at least once.
bool CamHookGetOpenXrToGameYaw(float& outRadians);

// rig_yaw_stick_gate=3 (INV-292): the SAME once-per-frame publish, but the RAW value (before cam_yaw_smoothing_ms's EMA)
// with its QPC timestamp - the latest [1] and the one before it [0] - and a publish count. Rig-only; every existing
// consumer keeps reading the filtered CamHookGetOpenXrToGameYaw. False until two publishes exist or on a torn read.
bool CamHookGetRawYawPair(float& y0, long long& t0, float& y1, long long& t1, long long& count);

// The RENDERED view's camera matrix (mtx34 as the engine stores it: translation at [3],[7],[11], column k =
// (m[k], m[4+k], m[8+k])), as camhook last wrote it into the yaw-anchor camera - engine yaw x HEAD pitch/roll. NOT the
// engine's look camera, whose pitch drifts (INV-272). For fire_reticle_3d. False before the first write or when older
// than 150 ms.
bool CamHookGetRenderedViewMatrix(float out12[12], unsigned* ageMsOut);
// Main-menu latch (uipin.cpp, INV-535 add. 1): while wanted, EVERY camera matrix camhook writes is kept in a 32-entry
// ring; GetWrittenViewNear returns the newest one whose position is within tolM of `pos` (false = none). Not tied to the
// yaw anchor. COST while wanted: a 48-byte copy + a tick read per camera write; the read scans at most 32 entries.
void CamHookWantWrittenViews(bool on);
// `excludeObj` (0 = none): a camera object whose writes are skipped - the view's cached render camera, which is upstream
// of the copy that gets the main menu's head yaw and so does not carry it.
bool CamHookGetWrittenViewNear(const float pos[3], float tolM, float out12[12], unsigned* ageMsOut, uintptr_t excludeObj = 0);
long long CamHookWrittenViewCount();
// ui_panel_main_menu_turn (INV-537 add. 3): the camera object whose levelling ALSO gets the head's yaw - the view's
// cached render camera, the root of the copy chain - or 0 for none. Called by uipin.cpp each submit. COST in the copy
// hook: one pointer compare per levelled camera; for the root itself two atan2, a sin / cos and one quaternion product.
void CamHookSetMainMenuTurnRoot(uintptr_t cameraObject);
// The main-menu page switch catch (trace 20261115_1700_mm_view_switch_yaw): page switches caught in the copy hook / refused (the cull
// copy's source was not the drawn view's cache, or not a camera) since the last call, and the two engine addresses it watches (0 =
// not resolved yet - no catch).
void CamHookMainMenuViewSwitchStats(long* caught, long* refused, uintptr_t* cullAt, uintptr_t* viewAt);
// INV-620 (c): boneblend's camyaw::Hook (game thread) says a cutscene owns the look and the raw OpenXR yaw (radians) of the stripped
// head at its start; CamHookCutsceneTick (submit thread, once per submit, beside uipin::Tick) names the root and logs [CINYAW].
void CamHookSetCutsceneTurn(bool active, float y0RawRad);
void CamHookCutsceneTick();
void CamHookMainMenuTurnStats(long* hits, float* addedDeg, float* engineYawDeg);
long CamHookMainMenuTurnForced();   // root copies levelled only because it is the root (a still page), since the last call
// Main-menu submits since the last call: declared with the yaw the root was given / left on the located pose because no
// capture was tagged / because the tagged capture carried no yaw; and the largest declared-vs-located yaw among the first.
void CamHookMainMenuDeclStats(long* declared, long* noTag, long* noYaw, float* maxDeg);

// Re-arms the once-per-frame OpenXR->game yaw publish. The levelling hook that
// publishes the yaw runs during the camera build and cannot see the frame
// boundary itself, so the boundary side must tell it.
//
// >>> CALL IT FROM BeginFrameIfNeeded, NOT LocateOncePerFrame. <<< The latter
// early-returns on g_relocateOnQuery == 0 - the configured mode - so a call
// placed there never runs, the publish latches after the first one, and the yaw
// freezes: the arms stop following the camera entirely. That was shipped once.
void CamHookNoteFrameBoundary();

// [EYEAXIS] once-per-frame read of the two eye cameras. A bool check and nothing
// else while diag_eye_axis is off. Called from BeginFrameIfNeeded, next to
// CamHookNoteFrameBoundary, for the same reason.
void CamHookEyeAxisTick();

// Exponential time constant for that yaw, milliseconds. 0 disables filtering.
// Body yaw is slowly varying, so a little costs almost nothing in lag.
extern float CamHookYawSmoothingMs;

// Texture-streaming lowest-priority distance in metres (r_device.scr ships
// 100). Applied live via SRendererAPI slot 143 case 6, verified by reading
// rd3d11+0x784b4 back. The leading lever for distant assets - including NPCs -
// never becoming resident.
extern float StreamPriorityDistanceM;
void ApplyStreamPriorityDistance();

extern bool CamHookWriteCombined;
extern bool CamHookCombinedPre;   // pre- rather than post-multiply
extern bool CamHookWriteView;
extern bool CutscenePitch;   // comfort_cutscene_pitch - cutscenes keep their own camera pitch (INV-620 (c) ADDENDUM 2)
extern bool CamHookTransposed;

// Re-derive `view` and `combined` from the camera matrix the way the engine
// does, instead of nudging them.
//
// CCamera is not four independent matrices, it is one basis plus three things
// computed from it. CCamera::SetView does, in order:
//
//   mtx34::orientation(this+0x40, pos, dir, up)   camera basis
//   mtx34::invert_from(this+0x10, this+0x40)      view = inverse(camera)
//   mul4434_44(this+0xB0, this+0x70, this+0x10)   combined = proj * view
//
// Writing +0x40 alone leaves the other two describing the orientation the engine
// last computed, not the one we just wrote. The old approach patched `combined`
// by multiplying its top-left 3x3 by a delta rotation, which cannot be right:
// the rotation belongs between proj and view, and no multiplication of the
// finished product on either side can put it there. It also never touched the
// fourth row, where the perspective divide lives - so x and y came from the new
// orientation while w came from the old one.
extern bool CamHookDerive;

// ------------------------------------------------------ automatic locking ---
//
// One switch instead of scan-then-pick-rows.
//
// Picking targets by hand cannot survive a level load. The two eye cameras are
// allocated in the main menu and keep their addresses; every other
// CRendererCamera is freed and reallocated when a level loads, so the object
// that culls the world has a different address every session. A remembered list
// of addresses would be wrong by the time it was useful.
//
// It also cannot be driven from a memory scan. The scan finds seven
// CRendererCamera instances and only three of them are doing anything - the
// rest are constructed and never driven, and levelling those is what produced
// the earlier crop of glitching windows.
//
// Both problems disappear if the hook picks its own targets, because the hook
// is already standing exactly where the answer is. Every camera the engine
// actually uses passes through CCamera::operator= every frame; one that is not
// being driven never arrives. So:
//
//   - identity comes from the vtable pointer, compared against the address RTTI
//     gives for CRendererCamera. One compare, no scanning, no guessing.
//   - liveness comes from the matrix changing between two arrivals, which is
//     the same test the user was applying by eye to the table.
//
// New cameras are picked up as they start being driven and dropped when they
// stop, so entering a level adds the culling camera on its own.
extern bool CamHookAuto;

// The auto-mode table is bounded (camhook.cpp g_auto).
constexpr int kAutoSlots = 32;

// A look at the matrix the hook last handled (g_probe). Everything
// here is measured, not assumed - the determinant in particular, because the
// quaternion round trip in the middle of the levelling silently forces a
// right-handed result and would hide a left-handed basis rather than report it.
struct CamProbe {
    unsigned long long obj;    // which object this describes
    float det;                 // determinant of the incoming 3x3: must be +1
    float ortho;               // largest dot product between two different axes
    float scale;               // longest axis length, 1 for a pure rotation
    float inAxes[9];           // incoming basis, columns: right, up, forward
    float outAxes[9];          // what we wrote
    unsigned long long negDet;    // times a left-handed basis was seen
    unsigned long long skewed;    // times the axes were not orthonormal
    unsigned long long derived;   // times view and combined were rebuilt
};

// How often the levelling applied a head orientation belonging to the previous
// frame, and how often it ran at all. A rising ratio means the engine builds its
// camera before it asks us for the pose, so the image is rendered with one head
// and declared to the compositor with another.
long long CamHookStaleHeadCount();
long long CamHookLevelCount();

// What the hook does to the matrix.
//
//   Level  remove pitch and roll, keep the turn about the up axis. This is the
//          goal, but it needs the up axis to be right, and getting that wrong
//          does not tilt the view slightly - it rotates about an axis the engine
//          does not use, which swings the camera somewhere unrecognisable and
//          reads as being teleported.
//   Delta  add a fixed rotation to whatever the engine wrote. This assumes
//          nothing about up, handedness or storage: if the horizon tilts
//          smoothly and by the amount asked for, the convention is right, and
//          levelling can then be built on a footing that has been checked
//          rather than guessed.
//
// Delta first, always. It is the difference between "the hook works and the
// maths is wrong" and "the hook does not work", which from inside the headset
// look the same.
//   Observe  run the hook and count, but write nothing. The same isolation
//            that found the RAX bug: if locking a camera kills the game with
//            this selected, the fault is in hooking that object at all, and no
//            amount of correcting the rotation will help. If it survives, the
//            write is what kills it.
enum class HookOp { Observe, Level, Delta };
extern HookOp CamHookOp;
extern float CamHookDeltaDeg;
extern int   BoneDumpAttempts;
extern int CamHookAxis;        // 0 = X, 1 = Y, 2 = Z
extern bool CamHookPreMultiply;  // world-space rotation instead of local

}  // namespace dlvr
