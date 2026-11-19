// VR swing-triggered melee attack (B1) - docs/SPEC-VR-SWING-ATTACK-2026-09-29.md.
// The swing IS the attack (D-62, D-154): a physical right-hand swing calls the game's own hit function from the
// idle state, with the animation-bound sweep builder answered from controller segments. The FIRE/END decision is
// swing_logic.h (unit-tested); this file does the side effects.
#include "swing.h"

#include "bodygrab.h"       // BodyGrabHandActive - no hit while the right hand holds a body
#include "breakout.h"       // BreakoutMeleeGateClosed - no hit from 500 ms into a zombie grab (a shove reads as a swing)
#include "boneblend.h"
#include "controllers.h"
#include "headpose.h"
#include "holster.h"
#include "holster_logic.h"
#include "meleethrow.h"
#include "rtticam.h"
#include "swing_logic.h"
#include "swingshadow_logic.h"   // diag_swing_shadow - the shadow span and the per-swing classification
#include "bladeline_logic.h"     // the blade-length hit line - the drawn weapon's segment (spec 2026-10-26-blade-length-hit-line)
#include "twohand_logic.h"       // twohand::NormMesh - the mesh-name normalisation the blade table is keyed by
#include "throwable.h"      // ThrowableBusy - ruling R24

#include <windows.h>

#include <cmath>
#include <cstdarg>   // AiAppend ([SWINGAI] v2 rows)
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/buildrva.h"   // the melee family's EGL rvas on the Epic build (tier 2)
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs
#if defined(DLVR_SWING_PROBE)   // the probe variant: swing.cpp's own periodic lines ([SWING STATS]) always print (trace 20261119_1730)
#undef LOG_PERIODIC
#define LOG_PERIODIC(...) LOG(__VA_ARGS__)
#endif

namespace dlvr {

// [DEP: melee_swing_mode] required-for=the VR swing attack (MeleeSwingMode) - SPEC-VR-SWING-ATTACK-2026-09-29
//      tree=INERT at 0: SwingInstall returns before touching anything and nothing is patched.
//      1 = DETECT: slot-16 detour + [SWING] lines, NEVER calls the hit. 2 = HIT: full B1.
//      HEADSET: needs the right GRIP pose, the real head pose, and the rig's grip publish (MeleeGripRead) - so all
//      THREE rig master enables (INV-1) and [SHOULDER] latched (INV-2/3).
int MeleeSwingMode = 2;   // DEFAULT 2 (HIT) 2026-10-04 = Jay's disk (Stage 2 PASS 2026-09-30). melee_r2_suppress ships ON:
                          // at 0 a fresh install had the melee trigger blocked AND no swing - no way to attack.
// [DEP: melee_swing_fire_speed] [DEP: melee_swing_arm_speed] [DEP: melee_swing_end_speed] required-for=the swing
//      detector's thresholds (MeleeSwingFireSpeed / MeleeSwingArmSpeed / MeleeSwingEndSpeed), m/s, app space.
//      tree=INERT at melee_swing_mode=0. Starting values are CPVR's shipped ones (REF §6.1) - theirs, not ours.
float MeleeSwingFireSpeed = 2.5f;
float MeleeSwingArmSpeed  = 1.0f;
float MeleeSwingEndSpeed  = 0.5f;   // DEFAULT 0.5 2026-10-26 (Tier D: Jay's live tuning, spec 2026-10-26-blade-length §9.4)
// [DEP: melee_swing_window_ms] required-for=the velocity window's minimum span (MeleeSwingWindowMs). tree=INERT at 0.
float MeleeSwingWindowMs = 40.0f;
// [DEP: melee_swing_max_ms] required-for=the hard END of a swing (MeleeSwingMaxMs). tree=INERT at mode 0.
float MeleeSwingMaxMs = 600.0f;
// [DEP: melee_swing_min_travel_m] required-for=rebound suppression (MeleeSwingMinTravelM), m, app-space PATH length:
//      Armed + speed >= fire starts a CANDIDATE that FIREs only once the hand has travelled this far, and is dropped
//      (no FIRE, no END, counted) if it slows below end speed first. Stage 1 run 1: 11 FIRE for 5 slashes - each
//      slash's reversal END re-armed and the arm's recoil (~0.1-0.25 m) fired again. tree=INERT at melee_swing_mode=0.
float MeleeSwingMinTravelM = 0.25f;   // DEFAULT 0.25 2026-10-01 (Tier D: Jay's live tuning)
// [DEP: melee_swing_return_ms] [DEP: melee_swing_return_deg] required-for=the return-stroke rule (MeleeSwingReturnMs /
//      MeleeSwingReturnDeg): a candidate that reaches min travel moving within return_deg of "toward the body" (the
//      head-yaw frame's +Z, BackInApp), OPPOSITE the last fired swing, within return_ms of that swing's END, is DROPPED
//      (candidate dropped (return)) instead of FIRED. Stage 1 redo 4: 9 slashes away from the body (z -0.91..-1.00) and
//      5 returns toward it (z +0.69..+0.99, 162-913 ms after END). return_ms 0 = rule off. tree=INERT at mode 0.
float MeleeSwingReturnMs = 250.0f;   // DEFAULT 250 2026-10-26 (Tier D: Jay's live tuning, §9.4)
float MeleeSwingReturnDeg = 25.0f;   // DEFAULT 25 2026-10-26 (Tier D: Jay's live tuning, §9.4)
// [DEP: melee_swing_refuse_toward_body] required-for=Option A (MeleeSwingRefuseTowardBody, Jay 2026-09-30): 1 = ANY
//      candidate inside the melee_swing_return_deg cone around toward-the-body is DROPPED (candidate dropped
//      (toward-body)) - no window, no previous swing; melee_swing_return_ms is then unused. 0 = the window rule above.
//      Evidence: redo 4+5, all 6 toward-body FIREs were returns / a wind-up; stab run, 15 stabs + 18 slashes all
//      toward-body <= -0.39 (none refused). tree=INERT at mode 0.
bool MeleeSwingRefuseTowardBody = false;   // DEFAULT 0 2026-10-26 (Jay: "I'd rather the swing refuse is turned off", §9.4)
// [DEP: melee_swing_return_fast_mps] required-for=the return speed gate (MeleeSwingReturnFastMps, Jay 2026-10-26: "Do the
//      speed-gated return, 4.5 m/s"): inside the return window rule, a candidate whose peak is at least this fast is a
//      BACKSWING and fires; slower stays dropped (return). Run ret250_end05: the dropped backswings peaked 7-12 m/s 26-65 ms
//      after the END, the one recoil 3.1 m/s. Not used by melee_swing_refuse_toward_body. 0 = no gate. DEFAULT 4.5 (Tier D).
//      tree=INERT at mode 0.
float MeleeSwingReturnFastMps = 4.5f;
// [DEP: melee_swing_reversal_deg] required-for=the reversal END (MeleeSwingReversalDeg, Jay 2026-10-26: "Fix the short-travel
//      misses too"): an Active swing ENDs Reversal when its velocity has turned more than this from the velocity one speed
//      window (melee_swing_window_ms) earlier - no longer from the FIRE velocity, so a curved katana / two-handed arc stays one
//      swing (run ret250_end05: katana misses ended after 0.57 m vs 0.99 m for hits, mostly reversal). 30..180, DEFAULT 90
//      (Tier D = Jay's disk since his 2026-10-26 tuning run reversal90; was 120 - set 2026-10-27 for the tester package). tree=INERT at mode 0.
float MeleeSwingReversalDeg = 90.0f;
// [DEP: melee_swing_reversal_ms] required-for=the reversal look-back (MeleeSwingReversalMs, Jay 2026-10-26 "Yes, build it"):
//      the reversal END compares against the velocity at least this many ms old (0 = melee_swing_window_ms). Run reversal120
//      (18 katana swings): at 100 ms the swings that absorbed a backswing turned 129-164 deg, single curved cuts <= 124 deg;
//      at 40 ms the two overlapped. 0..300, DEFAULT 100 (Tier D). tree=INERT at mode 0.
float MeleeSwingReversalMs = 100.0f;
// [DEP: melee_blade_pitch_deg] required-for=the blade axis tilt about grip +X (MeleeBladePitchDeg), spec §4.
//      tree=INERT at mode 0. Live-switchable from the overlay - a feel value for Jay.
float MeleeBladePitchDeg = 0.0f;
// [DEP: melee_hit_margin_m] required-for=the blade-length hit line (MeleeHitMarginM, spec docs/superpowers/specs/2026-10-26-blade-
//      length-hit-line-design.md, Jay: "Yes"): in mode 2 the B1 line runs along the DRAWN weapon, from the hand (or its grip
//      point, whichever is further up the shaft) to that mesh's real tip (bladeline_table.inc) PLUS this many metres. A weapon
//      with no table row keeps today's line (controller, game reach), named. 0..0.30, was DEFAULT 0.10 (Tier D: Jay's tuning). Live.
//      tree=LOADBEARING at melee_swing_mode=2. HEADSET.
float MeleeHitMarginM = 0.30f;   // DEFAULT 0.30 2026-10-26 (Tier D: Jay's live tuning, §9.4)
// [DEP: melee_hit_thickness_m] [DEP: melee_hit_back_m] required-for=the hit CAPSULE (MeleeHitThicknessM / MeleeHitBackM, spec
//      2026-10-26-blade-length-hit-line §9.2, Jay: "Yes, write it up and build it"; the miss probe run: 11 of 21 near misses off
//      to the SIDE, 4 just past the tip, 3 behind the hand): per mode-2 sub-step, after the box quads, the primary pair extended
//      `back` behind the hand and `thickness` past its end, plus two copies of it +-thickness across the swing plane - 3 more
//      quads, no game call; the record (g_phys / g_geo / this+1668 / +1892) stays the un-extended primary. 0..0.40 / 0..0.60,
//      DEFAULTS 0.20 / 0.30 (Tier D). tree=LOADBEARING at melee_swing_mode=2. HEADSET.
float MeleeHitThicknessM = 0.20f;
float MeleeHitBackM = 0.30f;
// [DEP: melee_hit_factor_restore] required-for=the distance-factor restore (MeleeHitFactorRestore, §9.3, INV-630; Jay approved
//      ON with a switch, UNVERIFIED): after the game's own search in B1Build, every target's +56 factor is rewritten to the
//      game's own line's value clamp(1 - |contact - grip| / reach, 0, 1); an entry at exactly 1.0 is left. The hit list, the
//      direction and the damage are untouched. DEFAULT 0 (Jay 2026-10-26, A/B factoroff_run1: "OFF felt better"; backswing
//      whiffs after a forehand hit not reduced by it: 23 / 62 on, 14 / 25 off). tree=INERT at mode 0. HEADSET.
bool MeleeHitFactorRestore = false;
// [DEP: melee_min_reach_m] required-for=the blade line's minimum reach (MeleeMinReachM, spec docs/superpowers/specs/2026-10-26-
//      blade-length-hit-line-design.md §7.3, Jay: "a second slider ... minimum reach from the hand"): in mode 2 the primary B1 line
//      ends at the farther of (tip + melee_hit_margin_m) and (the hand's point on the shaft + this many metres), so short weapons
//      do not fall short. 0 = off. 0..1.20, DEFAULT 0.65 (Tier D: Jay's tuning). Live. tree=LOADBEARING at melee_swing_mode=2.
//      HEADSET.
float MeleeMinReachM = 0.65f;
// [DEP: diag_melee_swing_probe] required-for=[SWING] per-frame detail lines (DiagMeleeSwingProbe), budget 600.
//      tree=INERT. The ARMED / FIRE / END / STATS lines are NOT behind it.
#if !defined(DLVR_BETA_LEAN)   // BETA lean: constexpr false in swing.h (docs/BETA_CENSUS.md)
bool DiagMeleeSwingProbe = false;
#endif
// [DEP: anim_suppress_melee_idle] required-for=keeping the melee ready state on "Stand" (AnimSuppressMeleeIdle) - INV-319;
//      Jay 2026-10-01: "Do we have a way to completely suppress the weapon idle animations?" ... "Build it." Every frame of the
//      primary WeaponMeleeController's slot-16 update (DriverFrame, after the game's update) zeroes the idle-availability
//      bytes +1561..+1564 (Exhausted_Idle / Vigilent / VigilentLeft / Trick) - +DBE690 then can only pick 0 = Stand - and puts
//      a non-zero variant +304 back to 0, logging [IDLESUP] once per catch with the variant. tree=INERT at 0. Needs
//      melee_swing_mode >= 1 (the slot-16 hook). Melee only - the firearm idle is not located in our binary (INV-319).
bool AnimSuppressMeleeIdle = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: diag_swing_shadow] required-for=[SWINGSHADOW] - THE SWING SHADOW PROBE (docs/superpowers/specs/
//      2026-10-26-swing-shadow-probe-design.md, Jay: "Approved"). Every mode-2 swing, the DRAWN weapon (its Holder ->
//      DTrailStart1, per mesh) is swept through the game's own widening + target search (sub_180DCA810) into a SCRATCH
//      list beside B1's real one, and the swing is classified BOTH / GAME ONLY / BLADE ONLY / NEITHER / SPLIT. Read-only for
//      the game (INV-623..627; the search writes only its own arguments, ida/hb10). Needs melee_swing_mode=2.
//      tree=PROBE-ONLY. Log-only. DEFAULT OFF. HEADSET.
#if !defined(DLVR_BETA_LEAN) || defined(DLVR_SWING_PROBE)   // BETA lean: constexpr false in swing.h; back in the probe variant
bool DiagSwingShadow = false;
#endif

namespace {

// ---- gamedll RVAs, all for md5 97046bba5eb07a918acff82a394e8c8b (spec §1) ----
constexpr uintptr_t kUpdateRva   = 0xDB1D10;   // WeaponMeleeController per-frame update, vtable slot 16 (F18)
constexpr uintptr_t kHitRva      = 0xDB4410;   // the whole hit (F2)
constexpr uintptr_t kBuildRva    = 0xDC2000;   // the sweep builder (F3)
constexpr uintptr_t kHandElemRva = 0xDCA6F0;   // (this, hand) -> the R_/L_HandHolder element id (INV-125)
constexpr uintptr_t kReachRva    = 0xDBEC70;   // (this, item) -> reach, xmm0 (F11)
constexpr uintptr_t kClassGate   = 0xDC9050;   // (this) -> item class 10/11/12 uses the player's direction (F12)
constexpr uintptr_t kSlotSelRva  = 0xDC2D50;   // (this) -> 0/1 slot selector used by every caller
constexpr uintptr_t kInactiveRva = 0xDCD7C0;   // WeaponMeleeController inactive update, vtable slot 17 (INV-639; EGL table)
constexpr uintptr_t kPlayerGroupOff = 3324;    // PlayerDI: the active controller group (INV-639; Steam build read)
constexpr int       kScanSlots   = 64;

using UpdateFn   = void(__fastcall*)(void* self);
using InactiveFn = void(__fastcall*)(void* self, float dt);
using HandElemFn = unsigned(__fastcall*)(void* self, unsigned hand);
using ReachFn    = float(__fastcall*)(void* self, void* item);
using BoolSelfFn = bool(__fastcall*)(void* self);
using GetIdFn    = int(__fastcall*)(const void* model, const char* name);
using GetWorldFn = const float*(__fastcall*)(const void* model, int elem);

UpdateFn   g_origUpdate = nullptr;
InactiveFn g_origInactive = nullptr;   // slot 17 - the light stagger feed (INV-639); null = not patched
HandElemFn g_handElem   = nullptr;
ReachFn    g_reach      = nullptr;
BoolSelfFn g_classGate  = nullptr;
BoolSelfFn g_slotSel    = nullptr;
GetIdFn    g_getId      = nullptr;
GetWorldFn g_getWorld   = nullptr;
uintptr_t  g_gd         = 0;
bool       g_installed  = false;
int        g_slot       = -1;
int        g_slot17     = -1;      // the inactive update's slot, once patched (the light stagger feed)
// The light stagger window (game thread - slot 16 and slot 17 both run inside UpdateControllers): open from the first slot-17
// frame in group 30, closed by the next slot-16 frame of the same controller (the reaction handed back).
void*      g_staggerSelf = nullptr;
ULONGLONG  g_staggerT0 = 0;
long       g_staggerN = 0, g_staggerFrames = 0, g_staggerB1At = 0, g_staggerFiresAt = 0, g_staggerLines = 0;
bool       g_staggerGroupBadSaid = false;
LONG       g_notArmedLogged = 0;
double     g_qpcToMs    = 0.0;

template <typename R, typename... A>
R VCall(void* obj, size_t byteOff, A... a)
{
    void** vt = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<R(__fastcall*)(void*, A...)>(vt[byteOff / 8])(obj, a...);
}

double NowMs()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<double>(q.QuadPart) * g_qpcToMs;
}

swing::Detector g_det;

struct GripFrame {
    bool        ok = false;     // pos/blade valid (a fresh rig snapshot)
    float       pos[3] = {};    // game-world grip point
    swing::Vec3 blade{};        // game-world blade direction (unit)
    float       ageMs = -1.0f;  // snapshot age
    bladeline::Seg seg{};       // the drawn weapon's hit segment this frame (blade = false -> today's line, BlFrame)
    bladeline::BoxFrame box{};  // the drawn Holder 3x4 + row for the box quads (ok = false -> no box quads, BlFrame)
};
GripFrame g_cur;
GripFrame g_prev;

// B1 sweep BACKFILL (rebound suppression): a swing now FIREs only after minTravelM of path, so the stroke before FIRE
// must still be swept. Every DriverFrame pushes its world grip here (one struct copy - no allocation), stamped with the
// DETECTOR clock (the frame's swing::Sample::tMs, NowMs) so the detector's candStartMs maps onto it directly. Filled in
// every mode; only mode 2's B1Build reads it. Game thread only (DriverFrame -> B1Frame -> g_hit -> B1Build).
struct WorldFrame {
    bool        ok;        // GripFrame::ok
    float       pos[3];    // game-world grip point
    swing::Vec3 blade;     // game-world blade direction (unit)
    long long   qpc;       // the rig snapshot's QPC (0 = no snapshot)
    double      tMs;       // detector clock of this DriverFrame
    bladeline::Seg seg;    // GripFrame::seg - the drawn weapon's segment (blade = false -> today's line)
    bladeline::BoxFrame box;   // GripFrame::box - the drawn Holder 3x4 + row (ok = false -> no box quads)
};
constexpr int kWorldRing = 64;   // 64 frames = 711 ms at 90 Hz, past melee_swing_max_ms's default + a candidate
WorldFrame g_wr[kWorldRing] = {};
int        g_wrHead = -1;        // index of the NEWEST entry (this frame's, once ReadWorldGrip has run)
int        g_wrN = 0;
double     g_candStartMs = 0.0;  // this swing's candidate start (detector clock), latched at FIRE
bool       g_backfillPending = false;   // set at FIRE; B1Build's first run this swing consumes it

// Counters. Per 1 s window (reset at each [SWING STATS]) unless named g_tot*.
long g_calls = 0, g_idle = 0, g_notIdleFrames = 0, g_samples = 0, g_repeats = 0;
long g_refHolster = 0, g_refNoPose = 0, g_refNonFinite = 0, g_refStaleGrip = 0, g_refNoGrip = 0;
long g_refNotIdle = 0, g_refNoItem = 0, g_refNoBuilder = 0, g_fires = 0, g_ends = 0, g_faults = 0;
long g_refMultiSelf = 0;          // R1: B1 refused because the last window saw > 1 WeaponMeleeController
long g_refNoWindow = 0;           // m4: B1 refused because no STATS window has completed yet (selves == 0)
long g_refNoExport = 0;           // m1: B1 refused because GetElementWorldMatrix / GetElementID did not resolve
long g_b1Calls = 0, g_buildOk = 0, g_buildZero = 0;
long g_candDropped = 0;           // candidates dropped (no FIRE, no END): slowed first, pose/holster, stall, blocked,
                                  // relatch (SwingUpdateThunk) - printed next to fires/ends
long g_backfillTrunc = 0;         // B1Build backfills whose candidate start was older than the whole ring (capped)
long g_dropLinesSuppressed = 0;   // fix round 1: dropped candidates NOT logged because g_dropBudget ran out (window)
#if defined(DLVR_SWING_PROBE)     // the probe variant: uncapped (trace 20261119_1730, Jay: "nothing log wise is capped")
long g_dropBudget = 0x7FFFFFFF;
#else
long g_dropBudget = 120;          // fix round 1: [SWING] candidate dropped lines left this SESSION
#endif
long g_totFires = 0, g_totFaults = 0, g_detailBudget = 600;
bool g_disabled = false;          // a fault disables the WHOLE driver for the session - fail CLOSED (M3)
ULONGLONG g_statsStart = 0;

// ---- Fix round 1 (coordinator review, binding rulings F1/F2/M3/M6) ----
// F1: slot 16 is shared by every WeaponMeleeController instance and nothing proves only the player's
// exists. Latch a PRIMARY self and refuse to feed the detector from any other. 500 ms hysteresis
// before switching primary, so one other instance's single call does not thrash it.
// Gate: also read OUTSIDE the driver gate by SwingUpdateThunk's unrelated-controller early-out - volatile, one aligned
// 64-bit load on x64. Written only inside the gate.
void* volatile g_primarySelf = nullptr;
// R4: also read by SwingStatsTick on the 1 Hz thread - volatile, and an aligned 64-bit load/store on x64.
volatile ULONGLONG g_primaryLastTick = 0;
// The primary's EMeleeControllerState (+0x110, -1 = ready), stamped by DriverFrame each call; read by item_rel_hold mode 3's
// melee hold (boneblend, SwingPrimaryMeleeState) on another thread - volatile, aligned 32-bit.
volatile LONG g_primaryState = -1;
volatile LONG g_otherSelfCalls = 0;   // Gate: incremented outside the gate by other controllers - Interlocked only
int       g_lastWindowSelves = 0;   // last STATS window's distinct-self count. B1Frame refuses while it is > 1 (R1).

// R2: race-safe without a lock. Slot 16 may be entered from two threads at once, so the slot index is TAKEN with
// InterlockedIncrement and written only when it is in bounds - an out-of-bounds index is saturation, never a write.
// The count therefore keeps rising past kMaxTrack once saturated; readers clamp it (SeenCount) and report the
// saturation as count > kMaxTrack. Two racing threads with the SAME new value may both take a slot (a duplicate
// entry, over-counting by one) - the only cost of having no lock.
constexpr int kMaxTrack = 8;
template <typename T>
void TrackSeen(T val, T (&arr)[kMaxTrack], volatile LONG& count)
{
    LONG n = count;
    if (n > kMaxTrack) { n = kMaxTrack; }
    for (LONG i = 0; i < n; ++i) {
        if (arr[i] == val) { return; }
    }
    const LONG idx = InterlockedIncrement(&count) - 1;
    if (idx >= 0 && idx < kMaxTrack) { arr[idx] = val; }
}
int  SeenCount(LONG c) { return c > kMaxTrack ? kMaxTrack : static_cast<int>(c); }
bool SeenSat(LONG c) { return c > kMaxTrack; }
void*         g_seenSelves[kMaxTrack]  = {};
volatile LONG g_seenSelvesCount        = 0;
DWORD         g_seenThreads[kMaxTrack] = {};
volatile LONG g_seenThreadsCount       = 0;

// U-60 (b), 2026-10-17 (Jay: "No new key, just change the behaviour"): in co-op the OTHER player's
// WeaponMeleeController updates on this machine too (Jay's two-sided session: the host's second self appeared
// 0.2 s after the client's first melee draw), so R1's "exactly one" refused every swing for both players. A
// controller is the LOCAL player's when its player (vtable +32 = gamedll+0x144470, `return *(this+8)`,
// ida/bow2_ours_out.txt:1590) is the game's local player object [[[gamedll+0x1C16348]+1344]+184]+88 with +80 set -
// boneblend SwPlayerObjNow's chain, the game's own local-player test in DropByActor (INV-570).
// A REMOTE controller is kept out of `selves` (R1) and out of the primary latch (F1). It is judged REMOTE only
// once some controller has matched the local player this session - until then, and whenever the chain does not
// read, a controller is NOT JUDGED and is counted exactly as before (R1 still fails closed). So a wrong identity
// (+8 not the PlayerDI) can never exclude the player's own controller: it leaves the old behaviour, and says so.
// [LOADBEARING: swing local-player controller test] SwingUpdateThunk `if (cls == kSelfRemote) { ... return; }`
// - without it co-op melee never calls the hit (U-60 2026-10-17 (8)).
// COST (per slot-16 call, ~90/s per controller): one virtual call (a single load) + five guarded loads + a scan of
// kNoteSlots pointers. LOG only on a controller's first classification or a change of it, capped per session.
constexpr int kSelfLocal = 0, kSelfRemote = 1, kSelfUnjudged = 2;
volatile LONG g_localMatched     = 0;   // 1 once any controller's player equalled the local player object
volatile LONG g_remoteSelfCalls  = 0;   // slot-16 calls from REMOTE controllers, this STATS window - Interlocked only
#if defined(DLVR_SWING_PROBE)   // the probe variant: classification lines never stop (trace 20261119_1730)
constexpr int kNoteSlots = 64;
#else
constexpr int kNoteSlots = 8;
#endif
void* volatile g_noteSelf[kNoteSlots] = {};
volatile LONG  g_noteCls[kNoteSlots]  = {};
volatile LONG  g_noteLines = 0;         // classification lines this session, capped at kNoteCap
#if defined(DLVR_SWING_PROBE)   // the probe variant: uncapped (trace 20261119_1730)
constexpr LONG kNoteCap = 0x7FFFFFFF;
#else
constexpr LONG kNoteCap = 48;
#endif

uintptr_t GuardedReadPtr(uintptr_t a)
{
    __try {
        return *reinterpret_cast<const uintptr_t*>(a);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The game's local player object, or 0. *why: 0 = read, 1 = [gamedll+0x1C16348] null, 2 = +1344 null, 3 = +184 null,
// 4 = no player object (+80 or +88 null).
uintptr_t LocalPlayerObj(int* why)
{
    static const uintptr_t s_camRva = ::dlvr::buildrva::Gd(0x1C16348);   // its EGL rva on the Epic build; resolved once
    const uintptr_t g = GuardedReadPtr(g_gd + s_camRva);
    if (!g) { *why = 1; return 0; }
    const uintptr_t a = GuardedReadPtr(g + 1344);
    if (!a) { *why = 2; return 0; }
    const uintptr_t lc = GuardedReadPtr(a + 184);
    if (!lc) { *why = 3; return 0; }
    const uintptr_t h = GuardedReadPtr(lc + 80);
    const uintptr_t o = GuardedReadPtr(lc + 88);
    if (!h || !o) { *why = 4; return 0; }
    *why = 0;
    return o;
}

// First classification of a controller, or a change of it: one line, capped per session. Racy by design (slot 16
// runs on several threads) - a race can only print a duplicate line, never miss the first one of a new slot.
void NoteSelfClass(void* self, uintptr_t owner, uintptr_t local, int why, int cls)
{
    int slot = -1;
    for (int i = 0; i < kNoteSlots; ++i) {
        if (g_noteSelf[i] == self) { slot = i; break; }
    }
    if (slot >= 0 && g_noteCls[slot] == cls) { return; }
    if (slot < 0) {
        for (int i = 0; i < kNoteSlots; ++i) {
            if (InterlockedCompareExchangePointer(&g_noteSelf[i], self, nullptr) == nullptr) { slot = i; break; }
        }
    }
    if (slot < 0) {
        // Every slot holds an older controller (each death / sleep makes new ones): say so ONCE, then stay quiet -
        // the classification itself still runs on every call, only its line is lost.
        static volatile LONG s_fullSaid = 0;
        if (InterlockedExchange(&s_fullSaid, 1) == 0) {
            // [PROBE: SWING] gate=melee_swing_mode  site=FLAT  log-only=yes
            LOG("swing: [SWING] controller classification lines STOPPED - all %d tracking slots are taken; controller "
                "%p and later ones are still classified (this one: class %d, 0 local 1 remote 2 not judged) but not "
                "logged",
                kNoteSlots, self, cls);
        }
        return;
    }
    InterlockedExchange(&g_noteCls[slot], cls);
    if (InterlockedIncrement(&g_noteLines) > kNoteCap) { return; }
    const char* verdict = cls == kSelfLocal ? "LOCAL - counted by R1, may be the primary"
                        : cls == kSelfRemote ? "REMOTE - excluded from R1's count and from the primary latch"
                        : why != 0 ? "NOT JUDGED (the local-player chain did not read) - counted as before"
                                   : "NOT JUDGED (no controller has matched the local player yet) - counted as before";
    // [PROBE: SWING] gate=melee_swing_mode  site=FLAT  log-only=yes
    LOG("swing: [SWING] controller %p: its player (vtable +32) %p, the game's local player %p (chain %d: 0 = read) -> "
        "%s | lines %ld of %ld",
        self, reinterpret_cast<void*>(owner), reinterpret_cast<void*>(local), why, verdict,
        static_cast<long>(g_noteLines), static_cast<long>(kNoteCap));
}

int ClassifySelf(void* self)
{
    int why = 0;
    const uintptr_t local = LocalPlayerObj(&why);
    const uintptr_t owner = reinterpret_cast<uintptr_t>(VCall<void*>(self, 32));
    int cls = kSelfUnjudged;
    if (local) {
        if (owner == local) {
            cls = kSelfLocal;
            InterlockedExchange(&g_localMatched, 1);
        } else if (g_localMatched) {
            cls = kSelfRemote;
        }
    }
    NoteSelfClass(self, owner, local, why, cls);
    return cls;
}

// The driver gate (Stage 1 run 1 `threads 7`; Jay 2026-09-30 "Go ahead and build it"). Everything after TrackSeen -
// the primary latch, the detector, the grip ring, the path buffer, the per-swing state, the counters and (mode 2) the
// hit - runs inside swing::Gate. Non-blocking: an entry while another thread holds it SKIPS its frame and is counted
// (`overlap skipped` in [SWING STATS]); the first one is logged with both threads. Whether the primary's calls ever
// overlap is [UNKNOWN] (INV-282: the 7-thread rotation cannot decide it) - this counter is the measurement.
// Cost per call: one lock cmpxchg + one plain release store.
swing::Gate    g_gate;
volatile DWORD g_gateHolderTid  = 0;         // written by the holder right after entering - for the first-overlap line
void* volatile g_gateHolderSelf = nullptr;
volatile LONG  g_overlapSaid    = 0;
long           g_overlapAtStats = 0;         // g_gate.Overlaps() at the last [SWING STATS] - inside the gate only

// F2: SwingStatsTick's "zero calls is silence, not evidence" detector needs to know the last time
// DriverFrame actually printed [SWING STATS], and the install time as the baseline before the first one.
// Single-writer (CLAUDE.md): DriverFrame is the only writer of g_lastStatsEmit.
volatile ULONGLONG g_lastStatsEmit = 0;
ULONGLONG          g_installTick   = 0;
// M3: the exception code from the fault that disabled the driver, read by SwingStatsTick.
DWORD g_lastFaultCode = 0;

// M6: the holster zone, latched with hysteresis like holster::Machine (enter inside r, leave only past
// r * kExitScale) instead of re-evaluated fresh every sample - a hand resting on the enter radius must
// not chatter in and out.
bool g_inHolster = false;

constexpr uintptr_t kQuadInitRva = 0xC46780;   // (entry) -> entry, 52-byte quad init (melee17:1305)
constexpr uintptr_t kPhysRva     = 0xDB51F0;   // (this, record, swingDelta) props/physics -> GiveDamage (INV-129)
constexpr uintptr_t kGeoRva      = 0xDC84B0;   // (this, record, rayLen in xmm2) world geometry (melee22:0xDC2BB9)
constexpr uintptr_t kPrepRva     = 0xDB63A0;   // (this, &HitParams, item, record)
constexpr uintptr_t kSearchRva   = 0xDCA810;   // (this, &HitParams, targets, this+1892) widening + search
constexpr uintptr_t kClear1928   = 0x0C19C0;   // list clear (melee20)
constexpr uintptr_t kClear1960   = 0xDCE180;   // list clear (melee20)
constexpr uintptr_t kIatMalloc   = 0x137A3D8;  // gamedll's IAT slots, api-ms-win-crt-heap (melee18)
constexpr uintptr_t kIatRealloc  = 0x137A3E0;
constexpr uintptr_t kIatFree     = 0x137A3D0;
// gamedll+0xDC2000's first 14 bytes: mov rax,rsp / mov [rax+18h],r8 / mov [rax+10h],rdx / push rbp / push rbx /
// push rsi. melee21_dc2000_prologue_ours.txt: 0 RIP-relative operands, 0 jumps/calls into +1..+13 (the check was
// validated on gamedll+0xDC1C90, whose bytes it reproduced exactly = boneblend.cpp kMeleePoseBytes).
const unsigned char kBuildBytes[] = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56 };

using HitFn      = void(__fastcall*)(void* self, void* record, int hand);
using BuildFn    = char(__fastcall*)(void* self, void* targets, void* item, float* record, unsigned hand);
using QuadInitFn = void*(__fastcall*)(void* entry);
using PhysFn     = void(__fastcall*)(void* self, float* record, float* swingDelta);
using GeoFn      = void(__fastcall*)(void* self, float* record, float rayLen);
using PrepFn     = void(__fastcall*)(void* self, void* hitParams, void* item, float* record);
using SearchFn   = void(__fastcall*)(void* self, void* hitParams, void* targets, float* dir);
using ClearFn    = void(__fastcall*)(void* list);
using ReallocFn  = void*(__cdecl*)(void* p, size_t n);
using FreeFn     = void(__cdecl*)(void* p);

HitFn      g_hit      = nullptr;
BuildFn    g_origBuild = nullptr;   // the trampoline
QuadInitFn g_quadInit = nullptr;
PhysFn     g_phys     = nullptr;
GeoFn      g_geo      = nullptr;
PrepFn     g_prep     = nullptr;
SearchFn   g_search   = nullptr;
ClearFn    g_clear1928 = nullptr;
ClearFn    g_clear1960 = nullptr;
ReallocFn  g_realloc  = nullptr;
FreeFn     g_free     = nullptr;
// Written by SwingInstall on camhook's 1 Hz thread, read by B1Frame on the game thread - volatile. Set true only
// AFTER the g_* function pointers above are assigned and the detour is committed.
volatile bool g_builderLive = false;
// Fix round 1 (I1): the builder is installed only in mode 2, after slot 16 is live, and retried once a second while
// not live. "Refused for good" = gamedll IAT unresolved, InstallDetour refused (a prologue mismatch - boneblend logs
// it - or its VirtualAlloc failed), or kMaxBuilderTries commit failures (each retry VirtualAllocs a fresh trampoline).
bool g_builderRefused = false;
int  g_builderTries   = 0;
constexpr int kMaxBuilderTries = 10;

// Set ONLY around B1's own call to sub_180DB4410, on the game thread. The builder hook answers a call as B1's only
// when all three match - every other call (every R2 attack) goes to the original untouched.
volatile bool  g_b1Active = false;
volatile DWORD g_b1Tid = 0;
void* volatile g_b1Self = nullptr;   // m2: the WeaponMeleeController B1 called the hit on

// This swing's constants, resolved once at FIRE on the game thread (spec §2.3).
float g_reachM = 0.0f;
float g_rayLen = 0.0f;
// This swing's per-swing reset ran and its constants resolved. Cleared on EVERY FIRE by DriverFrame, in any mode (so a
// 1 -> 2 switch mid-swing never reuses the previous swing's reach / uncleared lists), and by ResetSwingState.
bool g_swingReady = false;

// Final-review I4: per-swing counters, reset at FIRE (DriverFrame), printed at END. Game thread only.
long  g_swB1Calls = 0;       // B1Frame calls of sub_180DB4410 this swing
// The LAST reason B1Frame refused this swing (spec 2026-10-18-beta-leak-hardening section 6.5, Jay: "Might as well"): U-60's
// `multiself` lived only in the periodic counter, which was off, so it was inferred, not read. Reset at FIRE, printed at END.
const char* g_swRefusal = "none";
long  g_swBuildOk = 0;       // B1Build returned 1 this swing
long  g_swBuildZero = 0;     // B1Build returned 0 this swing
int   g_swBlocked = 0;       // 1 once B1Frame saw this+772 set and called Block()
long  g_swSamples = 0;       // non-repeat detector samples from FIRE through END, both included
long  g_swBackfillQuads = -1;   // quads B1Build's first run emitted this swing (-1 = no B1Build run this swing)
long  g_swBackfillFrames = 0;   // ring frames that backfill spanned (start frame .. this frame, both included)
int   g_swBackfillTrunc = 0;    // 1 = candStartMs was older than the whole ring - backfill started at the ring's oldest

// Final-review C1(2), U-d without an axis convention: per ACTIVE frame, |angle(bladeXR_app, detector velocity) -
// angle(world blade, world displacement)|. Printed at END. Game thread only. Rebound suppression: reset at the
// CANDIDATE start (not FIRE) and accumulated over Candidate AND Active frames - the candidate is the same stroke, and
// its frames are the start of the slash. A dropped candidate's values are discarded by the next candidate's reset.
int    g_udN = 0;
double g_udSum = 0.0;
float  g_udMax = 0.0f;

// Final-review C1(1): the LATEST frame's XR blade, written by DriverFrame every frame (one Rotate + one yaw
// extraction), read by the STATS block on the same thread - never recomputed there from stale poses.
swing::Vec3 g_bladeXrHy{ 0.0f, 0.0f, 0.0f };   // head-yaw frame, exactly as LogFire computes it
bool        g_bladeXrOk = false;               // this frame had both poses

// Stage 1 redo 3 (Jay, 2026-09-30): which way the hand MOVED, so a wind-up / return stroke can be told from a slash and a
// flick from a stroke - FIRE / END / drop lines only, log-only, the detector is untouched. APP-space grip positions,
// directions printed in the head-yaw frame (+X right, +Y up, -Z ahead). Game thread (DriverFrame / the relatch path).
swing::Vec3 g_lastP{ 0.0f, 0.0f, 0.0f };   // the last non-repeat, pose-valid, finite sample - mirrors the detector's
bool        g_lastPOk = false;             // prevP_ (cleared on a pose loss / non-finite sample / detector reset)
float       g_lastHeadQ[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // head orientation of the latest pose-valid frame
bool        g_lastHeadOk = false;
swing::Vec3 g_candStartP{ 0.0f, 0.0f, 0.0f };   // where the current candidate's PATH starts (the detector's prevP_)
bool        g_candStartOk = false;
swing::Vec3 g_prevNetHy{ 0.0f, 0.0f, 0.0f };    // the previous swing's net stroke direction, taken at its END
bool        g_prevNetOk = false;
double      g_lastEndMs = -1.0;                 // detector time of the previous END (-1 = none yet)
double      g_lastT = 0.0;                      // time of g_lastP (the detector's prevT_)

// Stage 1 redo 4 (Jay, 2026-09-30), the RAISE instrument: redo 3 #7 and redo 4 #15 FIRED on a slow raise after a long
// still pause (0.30 m in 42 ms). A real fast move and a tracking jump read the same in FIRE/END. So every non-repeat
// sample of the current candidate's path is kept - segment length, time step, and the grip's locationFlags - and
// printed at its FIRE or drop: a jump is ONE segment carrying most of the path, and may be a not-TRACKED sample.
// Game thread only. Per sample: one Len and one 32-bit flags read while a candidate is open, nothing otherwise.
struct PathSeg { float lenM; float dtMs; long flags; };
#if defined(DLVR_SWING_PROBE)   // the probe variant: every segment kept (trace 20261119_1730)
constexpr int kPathSegMax = 512;
#else
constexpr int kPathSegMax = 24;
#endif
PathSeg g_pathSeg[kPathSegMax] = {};
int     g_pathSegN = 0;        // stored (<= kPathSegMax)
int     g_pathSegTotal = 0;    // all segments of this candidate, stored or not
bool    g_pathOpen = false;    // a candidate's path is being recorded
// Per 1 s STATS window: non-repeat samples whose grip position was NOT tracked (inferred), and whose flags were unknown.
long    g_notTracked = 0;
long    g_flagsUnknown = 0;
long    g_candReturn = 0;      // candidates dropped by the return-stroke rule (also inside candidates dropped)

// HitParams, read out of sub_180DC2000:1074-1090 and its consumer sub_180DB63A0:64-82 (spec F8).
struct HitParams {
    void*         quads;    // +0x00  52-byte quads, gamedll heap
    unsigned      count;    // +0x08
    unsigned      cap;      // +0x0C
    unsigned      tag;      // +0x10  = 59
    unsigned      pad0;     // +0x14
    void*         player;   // +0x18  (this)->vfunc +32
    float         v[3];     // +0x20  player vfunc +1864's vec3
    unsigned      pad1;     // +0x2C
    void*         list1928; // +0x30  this + 1928
    unsigned char one;      // +0x38  = 1
    unsigned char pad2[7];
};
static_assert(offsetof(HitParams, count) == 0x08, "HitParams layout");
static_assert(offsetof(HitParams, tag) == 0x10, "HitParams layout");
static_assert(offsetof(HitParams, player) == 0x18, "HitParams layout");
static_assert(offsetof(HitParams, v) == 0x20, "HitParams layout");
static_assert(offsetof(HitParams, list1928) == 0x30, "HitParams layout");
static_assert(offsetof(HitParams, one) == 0x38, "HitParams layout");

inline float* F(void* self, size_t off) { return reinterpret_cast<float*>(static_cast<char*>(self) + off); }
inline int*   I(void* self, size_t off) { return reinterpret_cast<int*>(static_cast<char*>(self) + off); }
inline unsigned char* B(void* self, size_t off) { return static_cast<unsigned char*>(self) + off; }

void Norm3(const float* a, const float* b, float* out)   // out = normalise(a - b), sub_180DC2000's own 1e-6 guard
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    const float l = std::sqrt(x * x + y * y + z * z);
    if (l > 0.000001f) { x /= l; y /= l; z /= l; }
    out[0] = x; out[1] = y; out[2] = z;
}

#include "bladeline_table.inc"   // kBladeRows / kBladeRowsN / kBladeBoxes / kBladeBoxesN - GENERATED (tools/bladeline_table.py)

// One sub-step of sub_180DC2000's loop body (melee3_origination_ours.c:1095-1198), from segment A (prev) to segment
// B (cur), appending ONE quad to hp - plus, revision 2 (spec §7.4), one quad per box edge of the drawn weapon when both frames
// have a good Holder read of the same row. Every game call is the original's, in the original's order; the box quads make none.
enum SubStepResult { kSubStepOk, kSubStepBlocked, kSubStepNoMem };
long g_swBlSteps = 0, g_swFbSteps = 0;   // this swing's sub-steps on the blade line / on today's line (the END line)
long g_swBoxQuads = 0;                    // this swing's box quads appended (the END line)
long g_swCapQuads = 0;                    // this swing's capsule quads appended (§9.2, the END line)
long g_swFacN = 0;                        // this swing's targets whose factor was restored (§9.3, the END line)
double g_swFacBefore = 0.0, g_swFacAfter = 0.0;   // their factor sums, before / after
double g_swSearchMs = 0.0, g_swSearchMaxMs = 0.0;   // this swing's g_search time, sum / max per call (the END line)
double g_winSearchMaxMs = 0.0;            // the [SWING STATS] window's max g_search call
bool g_blFirstSaid = false;               // the session's first blade-line sub-step was logged

// The segment pair for one sub-step: both frames' drawn segments when both have one, else BOTH today's line (grip, grip +
// reach x blade) - bladeline::PickPair, so a quad never bridges the two. Counts the sub-step for the END line.
void BlPair(const bladeline::Seg& sa, const float* pa, const swing::Vec3& ba, const bladeline::Seg& sb, const float* pb,
            const swing::Vec3& bb, bladeline::Seg* oa, bladeline::Seg* ob)
{
    const float da[3] = { ba.x, ba.y, ba.z }, db[3] = { bb.x, bb.y, bb.z };
    bladeline::PickPair(sa, sb, pa, da, pb, db, g_reachM, oa, ob);
    if (oa->blade) { ++g_swBlSteps; } else { ++g_swFbSteps; }
    if (oa->blade && !g_blFirstSaid) {
        g_blFirstSaid = true;
        const float* s = ob->s;
        const float* e = ob->e;
        const float len = std::sqrt((e[0] - s[0]) * (e[0] - s[0]) + (e[1] - s[1]) * (e[1] - s[1]) + (e[2] - s[2]) * (e[2] - s[2]));
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] blade line FIRST sub-step (logged unconditionally) | start (%.3f %.3f %.3f) end (%.3f %.3f %.3f) = "
            "%.3f m along the drawn weapon, margin %.2f m (today's line would be %.3f m from the grip)",
            s[0], s[1], s[2], e[0], e[1], e[2], static_cast<double>(len), static_cast<double>(MeleeHitMarginM),
            static_cast<double>(g_reachM));
    }
}

// One quad {prev start, prev end, cur start, cur end} appended to hp by the original's growth rule (:1143-1168), through
// gamedll's own IAT realloc (F9/F10) and quad init. False = realloc failed (hp.quads still owns the old block - fail CLOSED).
bool AppendQuad(HitParams& hp, const float* ps, const float* pe, const float* cs, const float* ce)
{
    const unsigned need = hp.count + 1;
    if (need > hp.cap) {
        unsigned cap = (((10u * hp.cap) >> 3) + 16u) & 0xFFFFFFF0u;
        if (need > cap) { cap = need; }
        void* grown = g_realloc(hp.quads, static_cast<size_t>(52) * cap);
        if (!grown) { return false; }
        hp.quads = grown;
        hp.cap = cap;
    }
    float* e = static_cast<float*>(g_quadInit(static_cast<char*>(hp.quads) + static_cast<size_t>(52) * hp.count));
    for (int i = 0; i < 3; ++i) {         // :1153-1165
        e[i] = ps[i];
        e[3 + i] = pe[i];
        e[6 + i] = cs[i];
        e[9 + i] = ce[i];
    }
    hp.count = need;                      // :1168
    return true;
}

SubStepResult B1SubStep(void* self, float* record, HitParams& hp, const bladeline::Seg& A, const bladeline::Seg& Bs,
                        const bladeline::BoxFrame& ba, const bladeline::BoxFrame& bb)
{
    // record: +0x00 cur start, +0x0C cur end (:1120-1128), +0x18 prev start, +0x24 prev end (INV-129 loop). The
    // original CARRIES prev from the previous sub-step (:1184-1189); it is set here from A so a skipped non-ok ring
    // frame never splices two segments together - for adjacent ring frames it is the same value the carry leaves.
    // [LOADBEARING: melee_hit_margin_m] the segments are the drawn weapon's (hand / Holder -> tip + margin) when BlPair found
    // both, else today's (grip, grip + reach x blade) - the spec's §4.2 / §4.3.
    for (int i = 0; i < 3; ++i) {
        record[0 + i] = Bs.s[i];
        record[3 + i] = Bs.e[i];
        record[6 + i] = A.s[i];
        record[9 + i] = A.e[i];
    }
    float delta[3];                       // normalise(prev end - cur end) -> this+0x684 and DB51F0's 3rd arg (:1129-1142)
    Norm3(&record[9], &record[3], delta);
    F(self, 1668)[0] = delta[0];
    F(self, 1668)[1] = delta[1];
    F(self, 1668)[2] = delta[2];

    // :1143-1168 - the primary quad (prev start, prev end, cur start, cur end). The original does not check the realloc; we
    // fail CLOSED (the caller frees hp.quads).
    if (!AppendQuad(hp, &record[6], &record[9], &record[0], &record[3])) { return kSubStepNoMem; }

    // Revision 2 (spec §7.4): the drawn weapon's box edges, prev edge k -> cur edge k, only between two good frames of the
    // same row and only on the blade line. No game call, no this+1668 / +1892 / record write - those stay the primary line's.
    // [LOADBEARING: melee_swing_mode] the box quads ARE mode 2's hit shape (no key of their own - fix without a new key).
    // COST: up to kMaxBoxSegs x (2 x 2 point transforms + one 52-byte quad) per sub-step; no game call.
    if (A.blade && Bs.blade && bladeline::BoxPairOk(ba, bb) && ba.row < static_cast<int>(kBladeRowsN)) {
        const bladeline::BladeRow& row = kBladeRows[ba.row];
        float ps[bladeline::kMaxBoxSegs][3], pe[bladeline::kMaxBoxSegs][3];
        float cs[bladeline::kMaxBoxSegs][3], ce[bladeline::kMaxBoxSegs][3];
        const int na = bladeline::RowSegments(ba.m, row, kBladeBoxes, kBladeBoxesN, ps, pe, bladeline::kMaxBoxSegs);
        const int nb = bladeline::RowSegments(bb.m, row, kBladeBoxes, kBladeBoxesN, cs, ce, bladeline::kMaxBoxSegs);
        // Same row -> same count. A different count means a non-finite read dropped a box in one frame: edge k would then
        // pair two different edges, so that sub-step adds no box quads at all.
        for (int k = 0; na == nb && k < na; ++k) {
            if (!AppendQuad(hp, ps[k], pe[k], cs[k], ce[k])) { return kSubStepNoMem; }
            ++g_swBoxQuads;
        }
    }

    // §9.2 the hit CAPSULE: the primary pair extended back behind the hand and past its end, plus its two copies across the
    // swing plane. Extra quads only - the record above (g_phys / g_geo / this+1668 / +1892) stays the un-extended primary.
    // [LOADBEARING: melee_hit_thickness_m] [LOADBEARING: melee_hit_back_m] the capsule IS the fix for the probe's near misses.
    // COST: 3 quads (2 Capsule + 1 ThickCopies, a few dozen flops) per blade-line sub-step; no game call.
    if (A.blade && Bs.blade && (MeleeHitThicknessM > 0.0f || MeleeHitBackM > 0.0f)) {
        bladeline::Seg ca = A, cb = Bs;
        if (bladeline::Capsule(ca.s, ca.e, MeleeHitBackM, MeleeHitThicknessM) &&
            bladeline::Capsule(cb.s, cb.e, MeleeHitBackM, MeleeHitThicknessM)) {
            if (!AppendQuad(hp, ca.s, ca.e, cb.s, cb.e)) { return kSubStepNoMem; }
            ++g_swCapQuads;
            float as[2][3], ae[2][3], bs[2][3], be[2][3];
            const int nc = bladeline::ThickCopies(ca, cb, MeleeHitThicknessM, as, ae, bs, be);
            for (int k = 0; k < nc; ++k) {
                if (!AppendQuad(hp, as[k], ae[k], bs[k], be[k])) { return kSubStepNoMem; }
                ++g_swCapQuads;
            }
        }
    }

    g_phys(self, record, delta);          // :1169
    g_geo(self, record, g_rayLen);        // :1170

    float dir[3];                         // normalise(cur end - prev end) -> this+0x764 (1892), :1171-1183
    Norm3(&record[3], &record[9], dir);
    F(self, 1892)[0] = dir[0];
    F(self, 1892)[1] = dir[1];
    F(self, 1892)[2] = dir[2];
    for (int i = 0; i < 6; ++i) { record[6 + i] = record[i]; }   // current becomes previous, :1184-1189

    if (*B(self, 772)) { return kSubStepBlocked; }               // :1190-1194 - the swing hit a surface (F6)
    return kSubStepOk;
}

#if !defined(DLVR_BETA_LEAN) || defined(DLVR_SWING_PROBE)   // BETA lean: compiled out - DEV + the probe variant (trace 20261119_1730)
// ===== diag_swing_shadow - THE SWING SHADOW PROBE (spec docs/superpowers/specs/2026-10-26-swing-shadow-probe-design.md) =====
// Per DriverFrame (probe on): the drawn weapon's Holder / DTrailStart1 / DTrailEnd1 and the player's hand holder, into a ring
// parallel to g_wr (same index, same clock). In B1Build, after the game's own g_search: the same span's quads built from
// those drawn segments go through the SAME g_search into a scratch targets vector; both target lists are read; nothing the
// game uses is written (no this+1892 / +1668 / +772, no hit list, no g_phys / g_geo / g_prep). Game thread only.
#define SHADOW_LOG(...) ::dlvr::LogTo("dlvr_swingshadow.log", __VA_ARGS__)

constexpr uintptr_t kDebugDrawByteRva = 0x1C1208C;   // byte_181C1208C - gates sub_180C48E30's DebugRenderer path (ida/hb10)
const volatile unsigned char* g_dbgByte = nullptr;     // resolved in InstallBuilderHook, never per frame

struct ShadowFrame {
    float hold[3];    // the weapon model's Holder world position (= the drawn grip)
    float holdY[3];   // its world +Y column (the mesh shaft frame, INV-626)
    float tip[3];     // DTrailStart1 world (the mesh's own distal trail point, spec C1)
    float end1[3];    // DTrailEnd1 world (0 0 0 when the mesh has none)
    float hh[12];     // the player's R_HandHolder world 3x4 (rows, t at 3 / 7 / 11)
};
ShadowFrame g_sw[kWorldRing] = {};
double      g_swT[kWorldRing] = {};
bool        g_swOk[kWorldRing] = {};

// The weapon model's element ids, per model pointer (resolved on a change only - never per frame).
const void* g_swModel = nullptr;
int         g_swIdHold = -1, g_swIdTip = -1, g_swIdEnd = -1;
long        g_swModelLines = 0;   // model-change lines left in the main log (cap below)

// Session totals (the 5 s line) and refusals, each named.
long g_shArmedSaid = 0, g_shModeSaid = 0;
long g_shFrames = 0, g_shRefNoExport = 0, g_shRefNoPlayer = 0, g_shRefNoModel = 0, g_shRefNoHold = 0, g_shRefNoTip = 0;
long g_shRefRead = 0, g_shRefDebug = 0, g_shRefNoPair = 0, g_shRefMem = 0, g_shCalls = 0, g_shOverflow = 0;
long g_shCls[7] = {};             // indexed by swingshadow::Cls
ULONGLONG g_shLast5s = 0;         // the 5 s line's own stamp (INV-9 / INV-10)

// Per swing (FIRE .. END).
constexpr int kShKeys = 64;   // was 16 - raised so no set fills in the probe run (Jay 2026-11-19, trace 20261119_1730)
swingshadow::Key g_shGame[kShKeys] = {}, g_shBlade[kShKeys] = {};
int    g_shNGame = 0, g_shNBlade = 0;
bool   g_shOpen = false;          // a swing is being shadowed (opened at FIRE with the probe on)
bool   g_shComplete = false;      // every B1Build run of this swing was shadowed
long   g_shSwCalls = 0;           // shadow calls this swing
double g_shUsSum = 0.0, g_shUsMax = 0.0;
bool   g_shFireGeo = false;       // the FIRE geometry below was taken (the swing's first shadowed B1Build run)
float  g_shFireReach = 0.0f, g_shFireStartVsHold = -1.0f, g_shFireAngle = -1.0f, g_shFireClosest = -1.0f;
float  g_shFireArm[3] = { -1.0f, -1.0f, -1.0f };   // asked, drawn, reach (m)
float  g_shFireWeaponLen = -1.0f;
int    g_shTrunc = 0;
const char* g_shReason = "none";  // the swing's last shadow refusal, named in the SUMMARY (review I6)
double g_shTotUsSum = 0.0, g_shTotUsMax = 0.0;   // the WHOLE ShadowAfterSearch incl. its file rows (review I4)
long   g_shSkipGeo = 0;           // runs whose newest ring frame was not ok -> no geometry row (review I2)
double g_shSpeed = 0.0;           // this DriverFrame's detector speed, m/s (set in DriverFrame before B1Frame)
double g_shTMs = 0.0;             // this DriverFrame's detector clock, ms
bool   g_shResolve = false;       // re-resolve the weapon's element ids on the next frame (set at FIRE - review I5, ABA)
long   g_shRefNoItem = 0, g_shLiveSaid = 0;

void* ItemOf(void* self, void* player);   // defined beside RayLenOf - B1Frame's own "melee item in the hand" check

void ShAddKey(swingshadow::Key* set, int& n, swingshadow::Key k)
{
    for (int i = 0; i < n; ++i) { if (set[i].obj == k.obj) { return; } }
    if (n < kShKeys) { set[n++] = k; } else { ++g_shOverflow; }
}

// ===== The MISS probe (Jay 2026-10-26: "Build a miss probe ... Whatever information we need, it has to be in this run") =====
// Inside diag_swing_shadow: after the blade search, a THIRD log-only search on an INFLATED "near" shape - per ring frame pair,
// three lines from the drawn Holder out to kNearLenM along the drawn shaft, the middle one and copies kNearOffM either side of
// the swing plane - through the SAME g_search. Every object it touches gets its contact's distance to the REAL hit shape of
// that pair's two frames (the primary segment + the box edges, g_wr[].seg / .box); at END each one is HIT (in the game's set
// this swing), INSIDE NOT HIT (dist <= kInsideM, not hit), or NEAR MISS (dist). The distance is from the near shape's contact
// point - an UPPER bound on how close the object was. Nothing the game uses is written.
constexpr float kNearLenM = 2.0f, kNearOffM = 0.30f, kInsideM = 0.03f;
struct NearRec { unsigned long long obj; float dist; int elem; long call; float contact[3]; };
NearRec g_near[kShKeys] = {};
int     g_nearN = 0;
long    g_nearCalls = 0, g_nearOverflow = 0;
long    g_missCls[4] = {};   // session: swings with HIT / INSIDE NOT HIT / NEAR MISS only / NOTHING NEAR

// ===== [SWINGAI] - the game's OWN candidate list at every B1 search (trace docs/traces/20261119_1730_swing_probe.md; Jay 2026-11-19:
// "Setup the probe then. We need everything from this run") =====
// The miss probe above runs the game's search, so a zombie that never became a CANDIDATE reads NOTHING NEAR there. This reads the
// list the gather (+C4B400) itself walks - the AI manager's (GameDI +1344 -> vfunc +1952 = level -> +2544 -> +424 / +432) - and
// for every entry within kAiNearM of the player's head point (player vfunc +1840, ray 1's origin) runs the gather's OWN extents test
// against THIS search's sphere (the game's sub_180C51830 centre / sub_180C51900 radius on the real HitParams) and records what the
// gather and the per-candidate check read (HumanAI by slot 37 == +5E41C0, IsDead, active byte +3064, IsEnemy = player slot 42 +336,
// player slot 270 +2160). At END: one line per swing + one per object. Log-only; game memory is never written. Steam rvas only.
constexpr uintptr_t kAiGameDiRva = 0x1C16348, kAiCentroidRva = 0xC51830, kAiRadiusRva = 0xC51900, kAiBoxDistRva = 0x1C27B0;
// v2 (RUN SP1 read; Jay: "WHEN I SAID WE NEED EVERYTHING, WHY WAS THAT NOT INCLUDED" -> "Do it."): for every HumanAI within kAiNearM,
// the search's OWN per-candidate path replicated step by step (ida/sw2-sw5, C48E30 +C49378..+C49B84): the gate (candidate +0x888,
// player +0x870, byte_181C12082), ray 1, the concept (*(obj+824)), the model (*(concept+72)), the 21 bones (GetMeshElemFromBoneID,
// the skip masks read from *(obj+3312), GetElementExtentsLocal + GetElementWorldMatrix), every quad's two triangles against each
// bone box (swingshadow::TriAabb, exact) + a sampled distance, and ray 2 to an overlapping bone's box centre (approximate).
// Slot 37 (+5E41C0) is used for IDENTITY only - it returns +4793 first and is not an alive reading (D-378); it is never called.
constexpr uintptr_t kAiIsDeadRva = 0x5E41C0;   // HumanAI vtable slot 37 - the HumanAI identity test only, never called
constexpr uintptr_t kAiGateRva = 0x191820;     // HumanAI slot 273 (+0x888) - sub_180191820 (sw3: IsEnemy / +320 == 5 / +1744)
constexpr uintptr_t kAiByte12082Rva = 0x1C12082, kAiBoneTabRva = 0x139EAA0;
constexpr uintptr_t kAiConceptVtRva = 0x151D010, kAiGetterRva = 0x144470;   // (obj+816)'s vtable, its slot 1 = return this[1]
constexpr int       kAiBones = 21;
const unsigned char kAiBoneIds[kAiBones] = { 8, 5, 4, 3, 2, 1, 0, 18, 20, 22, 19, 21, 23, 10, 11, 12, 13, 14, 15, 16, 17 };
constexpr float     kAiNearM = 3.0f;
constexpr int       kAiMax = 64;   // objects tracked per swing; past it still counted ("past the cap") - no line cap per session
using AiCentroidFn = float*(__fastcall*)(float* out, const void* hp);
using AiRadiusFn   = float(__fastcall*)(const void* hp, const float* centre);   // __m128, the low lane = the radius
using AiBoxDistFn  = float(__fastcall*)(const float* ext, const float* p);      // squared distance point -> local box
using AiW2LFn      = float*(__fastcall*)(const void* co, float* out, const float* in);
using AiExtFn      = const float*(__fastcall*)(const void* co);
using AiXformFn    = const float*(__fastcall*)(const void* co);
using AiElemFn     = int(__fastcall*)(const void* model, unsigned char bone);              // IModelObject::GetMeshElemFromBoneID
using AiElemExtFn  = float*(__fastcall*)(const void* model, float* out, int elem);         // IModelObject::GetElementExtentsLocal
using AiElemMtxFn  = const float*(__fastcall*)(const void* model, int elem);              // IModelObject::GetElementWorldMatrix
using AiRayFn      = unsigned char(__fastcall*)(void* self, unsigned char kind, void* coll, const float* from, float* to,
                                                unsigned short flags, void* ignore, unsigned a8, unsigned a9, int a10);
const char* const kAiRayName = "?Raytrace@IGSObject@@QEAAEEPEAUSCollision@@AEBVvec3@@AEAV3@GPEAVIControlObject@@IIH@Z";
struct AiRec {
    void*  obj; float dist; bool human, dead, active, enemy; uintptr_t vt;
    int    inCalls;    // B1 searches in which the gather's extents test passed for it
    float  minD2R2;    // smallest (box distance^2 / radius^2) over the searches (< 1 = inside the sphere)
    // v2, per swing (counts are B1 searches):
    int    gateFail, ray1Block, ray1Clear, ray1Target, ray1Water, noConcept, noModel, masksUnread, ovSearches, ovRay1Pass,
           ovNotHitByGame, gameHitSearches, ray2Block, faults;
    int    bonesNoElem, bonesSkipped;   // summed over searches
    float  bestD;                       // least triangle-box distance over every bone, quad and search (0 = overlap)
    int    bestBone, bestSearch;
    float  pos[3];                      // root position at the closest search
    unsigned rawTeam, rawState; unsigned char raw2352, raw4793;
    char   ray1Who[48];
};
AiRec        g_ai[kAiMax] = {};
int          g_aiN = 0, g_aiOver = 0;
long         g_aiCalls = 0, g_aiListMax = 0, g_aiNearMax = 0, g_aiSwings = 0;
long         g_aiChkAgree = 0, g_aiChkMiss = 0, g_aiSesAgree = 0, g_aiSesMiss = 0;   // known-case check: game hits the replica overlaps
double       g_aiUsMax = 0.0;   // this swing's slowest SwingAiSearch, microseconds
AiElemFn     g_aiElem = nullptr;
AiElemExtFn  g_aiElemExt = nullptr;
AiElemMtxFn  g_aiElemMtx = nullptr;
AiRayFn      g_aiRay = nullptr;
int          g_aiReady = 0;          // 0 untried, 1 ready, -1 refused (named once)
const char*  g_aiWhy = "not tried";
int          g_aiSlot270 = -1;       // player slot 270 at the last search (-1 = unread)
float        g_aiSphere[4] = { 0, 0, 0, -1 };   // the last search's centre + radius
AiCentroidFn g_aiCentroid = nullptr;
AiRadiusFn   g_aiRadius = nullptr;
AiBoxDistFn  g_aiBoxDist = nullptr;
AiW2LFn      g_aiW2L = nullptr;
AiExtFn      g_aiExt = nullptr;
AiXformFn    g_aiXform = nullptr;

bool AiHeadOk(uintptr_t at, const unsigned char* want, int n)
{
    __try {
        for (int i = 0; i < n; ++i) { if (reinterpret_cast<const unsigned char*>(at)[i] != want[i]) { return false; } }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Once: the three gamedll helpers by their first bytes (set-aside gamedll 97046bba), the three engine exports by name.
bool AiResolve()
{
    if (g_aiReady != 0) { return g_aiReady == 1; }
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!en || !g_gd) { return false; }   // retried next search
    static const unsigned char hC[7] = { 0x44, 0x8B, 0x4A, 0x08, 0x4C, 0x8B, 0xC1 };
    static const unsigned char hR[8] = { 0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0xA8, 0x00 };
    static const unsigned char hB[8] = { 0xF3, 0x0F, 0x10, 0x0A, 0x0F, 0x57, 0xC0, 0xF3 };
    g_aiW2L   = reinterpret_cast<AiW2LFn>(GetProcAddress(en, "?PointWorldToLocal@IControlObject@@QEBA?AVvec3@@AEBV2@@Z"));
    g_aiExt   = reinterpret_cast<AiExtFn>(GetProcAddress(en, "?GetExtentsLocal@IControlObject@@QEBAAEBVextents@@XZ"));
    g_aiXform = reinterpret_cast<AiXformFn>(GetProcAddress(en, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
    const bool cOk = AiHeadOk(g_gd + kAiCentroidRva, hC, 7), rOk = AiHeadOk(g_gd + kAiRadiusRva, hR, 8);
    const bool bOk = AiHeadOk(g_gd + kAiBoxDistRva, hB, 8);
    g_aiElem    = reinterpret_cast<AiElemFn>(GetProcAddress(en, "?GetMeshElemFromBoneID@IModelObject@@QEBAHE@Z"));
    g_aiElemExt = reinterpret_cast<AiElemExtFn>(GetProcAddress(en, "?GetElementExtentsLocal@IModelObject@@QEBA?AVextents@@H@Z"));
    g_aiElemMtx = reinterpret_cast<AiElemMtxFn>(GetProcAddress(en, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_aiRay     = reinterpret_cast<AiRayFn>(GetProcAddress(en, kAiRayName));
    // The search's bone table (dword_18139EAA0, 21 dwords, the low byte read - +C4943A) must hold exactly kAiBoneIds.
    unsigned char tab[4 * kAiBones] = {};
    for (int i = 0; i < kAiBones; ++i) { tab[4 * i] = kAiBoneIds[i]; }
    const bool tOk = AiHeadOk(g_gd + kAiBoneTabRva, tab, 4 * kAiBones);
    static const unsigned char hGet[5] = { 0x48, 0x8B, 0x41, 0x08, 0xC3 };   // mov rax, [rcx+8]; ret
    const bool gOk = AiHeadOk(g_gd + kAiGetterRva, hGet, 5);
    static const unsigned char hGate[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };   // sub_180191820's first bytes (our gamedll)
    const bool gateOk = AiHeadOk(g_gd + kAiGateRva, hGate, 6);
    g_aiWhy = !g_aiW2L ? "PointWorldToLocal export" : !g_aiExt ? "GetExtentsLocal export" : !g_aiXform ? "GetWorldXform export"
            : !cOk ? "+C51830 bytes (not the Steam gamedll?)" : !rOk ? "+C51900 bytes" : !bOk ? "+1C27B0 bytes"
            : !g_aiElem ? "GetMeshElemFromBoneID export" : !g_aiElemExt ? "GetElementExtentsLocal export"
            : !g_aiElemMtx ? "GetElementWorldMatrix export" : !g_aiRay ? "IGSObject::Raytrace export"
            : !tOk ? "the bone table +139EAA0 (not 8 5 4 3 2 1 0 18 20 22 19 21 23 10..17)" : !gOk ? "+144470 getter bytes"
            : !gateOk ? "+191820 (candidate gate) bytes" : nullptr;
    if (g_aiWhy) {
        g_aiReady = -1;
        // [PROBE: SWINGAI] gate=diag_swing_shadow  site=FLAT  log-only=yes
        LOG("swing: [SWINGAI] REFUSED - %s not found / not matching; the AI-list readout is off this launch (the shadow / miss lines "
            "are unaffected)", g_aiWhy);
        return false;
    }
    g_aiCentroid = reinterpret_cast<AiCentroidFn>(g_gd + kAiCentroidRva);
    g_aiRadius = reinterpret_cast<AiRadiusFn>(g_gd + kAiRadiusRva);
    g_aiBoxDist = reinterpret_cast<AiBoxDistFn>(g_gd + kAiBoxDistRva);
    g_aiWhy = "none";
    g_aiReady = 1;
    // [PROBE: SWINGAI] gate=diag_swing_shadow  site=FLAT  log-only=yes
    LOG("swing: [SWINGAI] READY (logged unconditionally) - centroid +C51830, radius +C51900, box distance +1C27B0, gate +191820, "
        "getter +144470, bone table +139EAA0 (21 ids) all match the Steam gamedll; engine exports PointWorldToLocal / "
        "GetExtentsLocal / GetWorldXform / GetMeshElemFromBoneID %p / GetElementExtentsLocal %p / GetElementWorldMatrix %p / "
        "Raytrace %p | per B1 search: every AI-list entry within %.1f m of the head point gets the gather's extents test, the "
        "search's gate, ray 1, and its 21 bone boxes against every quad (rows Q| Z| K| in dlvr_swingshadow.log)",
        reinterpret_cast<void*>(g_aiElem), reinterpret_cast<void*>(g_aiElemExt), reinterpret_cast<void*>(g_aiElemMtx),
        reinterpret_cast<void*>(g_aiRay), static_cast<double>(kAiNearM));
    return true;
}

// The AI manager's list (the gather's): data + count. False = a read faulted or a pointer was null.
bool AiList(uintptr_t* data, unsigned* count)
{
    __try {
        const uintptr_t gdi = *reinterpret_cast<const uintptr_t*>(g_gd + kAiGameDiRva);
        if (!gdi) { return false; }
        void* lv = *reinterpret_cast<void* const*>(gdi + 1344);
        if (!lv) { return false; }
        const uintptr_t level = reinterpret_cast<uintptr_t>(VCall<void*>(lv, 1952));
        if (!level) { return false; }
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(level + 2544);
        if (!mgr) { return false; }
        *data = *reinterpret_cast<const uintptr_t*>(mgr + 424);
        *count = *reinterpret_cast<const unsigned*>(mgr + 432);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One list entry: its root distance to the head point, and (when near) everything the gather / the check read. False = faulted.
bool AiProbeOne(void* obj, void* player, const float head[3], const float c[3], float r2, float* dist, AiRec* out, float* d2r2)
{
    __try {
        const float* x = g_aiXform(static_cast<char*>(obj) + 24);
        const float dx = x[3] - head[0], dy = x[7] - head[1], dz = x[11] - head[2];
        *dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (*dist > kAiNearM) { return true; }
        float local[3] = {};
        g_aiW2L(static_cast<char*>(obj) + 24, local, c);
        const float d2 = g_aiBoxDist(g_aiExt(static_cast<char*>(obj) + 24), local);
        *d2r2 = r2 > 0.0f ? d2 / r2 : -1.0f;
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        out->vt = vt;
        out->human = *reinterpret_cast<const uintptr_t*>(vt + 8 * 37) == g_gd + kAiIsDeadRva;
        out->dead = false;   // v2: slot 37 is NOT called (D-378) - the raw +4793 / +828 / +2352 bytes are logged instead
        out->active = out->human ? *reinterpret_cast<const unsigned char*>(static_cast<char*>(obj) + 3064) != 0 : false;
        out->enemy = VCall<unsigned char>(player, 336, obj) != 0;
        out->pos[0] = x[3]; out->pos[1] = x[7]; out->pos[2] = x[11];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- v2: the search's per-candidate path (C48E30 +C49378..). Every game read is guarded; nothing is written to game memory. ----
struct AiRayHit { bool ok, hit; unsigned long long back; unsigned short surf; };
struct AiCand {   // one candidate at one search
    unsigned team, state; unsigned char b2352, b3064, b4793;
    int c888, p870, b12082, gate;            // -1 = not read
    void* concept; void* model; const char* why;   // why = the first step that could not be read ("ok" = all read)
    const char* modelSrc;                    // "getter" (concept+64 slot 1 is a plain read) | "object" (the zombie itself - v2.1)
    uintptr_t getterRva; unsigned char getterBytes[16];   // concept+64 slot 1, for the static read (never called)
    unsigned m56, m58; int masks;            // masks: 1 read, 0 cache stale / absent (the game would rebuild it)
};
struct AiBone { int elem; bool skip, ok; int ov; float dist; float m[12]; float ext[6]; float cw[3]; };

AiRayHit AiCast(void* player, const float* from, const float* to)
{
    AiRayHit r{};
    __try {
        alignas(16) unsigned char coll[96] = {};
        *reinterpret_cast<int*>(coll + 32) = -1;
        alignas(16) float f[4] = { from[0], from[1], from[2], 0.0f };
        alignas(16) float t[4] = { to[0], to[1], to[2], 0.0f };
        char* p = static_cast<char*>(player);
        r.hit = g_aiRay(p + 40, 0x17, coll, f, t, 0x11, p + 24, 8u, 0u, 0) != 0;   // C48E30 +C49339 (INV-633's replica)
        r.back = *reinterpret_cast<const unsigned long long*>(coll + 24);
        r.surf = *reinterpret_cast<const unsigned short*>(coll + 56);
        r.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r.ok = false; }
    return r;
}

// The candidate's gate / concept / model / masks, exactly the reads the search makes (the calls replaced by reads where sw3 / sw4
// showed them to be getters). False = a read faulted (why says which step).
bool AiCandRead(void* obj, void* player, AiCand* c)
{
    c->c888 = c->p870 = c->b12082 = c->gate = -1;
    c->concept = c->model = nullptr;
    c->masks = 0;
    c->modelSrc = "-";
    c->getterRva = 0;
    c->why = "raw fields";
    __try {
        char* o = static_cast<char*>(obj);
        c->team = *reinterpret_cast<const unsigned*>(o + 320);
        c->state = *reinterpret_cast<const unsigned*>(o + 828);
        c->b2352 = *reinterpret_cast<const unsigned char*>(o + 2352);
        c->b3064 = *reinterpret_cast<const unsigned char*>(o + 3064);
        c->b4793 = *reinterpret_cast<const unsigned char*>(o + 4793);
        c->why = "gate";
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        if (*reinterpret_cast<const uintptr_t*>(vt + 0x888) != g_gd + kAiGateRva) { c->why = "slot +0x888 is not +191820"; return true; }
        c->c888 = VCall<unsigned char>(obj, 0x888, player) != 0 ? 1 : 0;   // sub_180191820 - reads (sw3)
        c->p870 = VCall<unsigned char>(player, 0x870) != 0 ? 1 : 0;        // PlayerDI slot 270 - this+320 == 1 (INV-634)
        c->b12082 = *reinterpret_cast<const unsigned char*>(g_gd + kAiByte12082Rva) != 0 ? 1 : 0;
        c->gate = (c->c888 || !c->p870 || c->b12082) ? 1 : 0;              // +C49392..+C493B9
        c->why = "concept";
        const uintptr_t vt816 = *reinterpret_cast<const uintptr_t*>(o + 816);
        if (vt816 != g_gd + kAiConceptVtRva || *reinterpret_cast<const uintptr_t*>(vt816 + 8) != g_gd + kAiGetterRva) {
            c->why = "concept getter (+816 vtable) not the known one";
            return true;
        }
        c->concept = *reinterpret_cast<void* const*>(o + 824);                // +0x610 -> (this+816)->[1] = *(this+824)
        if (!c->concept) { c->why = "concept null (the search tests no bone)"; return true; }
        c->why = "model";
        char* v44 = static_cast<char*>(c->concept) + 64;
        const uintptr_t vt44 = *reinterpret_cast<const uintptr_t*>(v44);
        const unsigned char* f1 = reinterpret_cast<const unsigned char*>(*reinterpret_cast<const uintptr_t*>(vt44 + 8));
        c->getterRva = reinterpret_cast<uintptr_t>(f1);
        for (int k = 0; k < 16; ++k) { c->getterBytes[k] = f1[k]; }
        if (f1[0] == 0x48 && f1[1] == 0x8B && f1[2] == 0x41 && f1[3] == 0x08 && f1[4] == 0xC3) {
            c->model = *reinterpret_cast<void* const*>(v44 + 8);
            c->modelSrc = "getter";
        } else {
            // v2.1 (RUN SP2: never a plain read): the zombie object itself - bodygrab's SkelLimb reads elements on this same
            // pointer in play, and the search's GetElementExtentsLocal takes it (+C494A6). The known-case check decides.
            c->model = obj;
            c->modelSrc = "object";
        }
        if (!c->model) { c->why = "model null"; return true; }
        c->why = "masks";
        const float* cache = *reinterpret_cast<const float* const*>(o + 3312);   // sub_1805E5080's cache - READ, never rebuilt
        if (cache && cache[19] == 0.0f) {
            c->m56 = *reinterpret_cast<const unsigned*>(cache + 56);
            c->m58 = *reinterpret_cast<const unsigned*>(cache + 58);
            c->masks = 1;
        }
        c->why = "ok";
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One bone: its element, local box and world matrix, read with the search's own exports. False = faulted.
bool AiBoneRead(void* obj, void* model, unsigned char id, AiBone* b)
{
    b->ok = false;
    __try {
        b->elem = g_aiElem(model, id);
        if (b->elem < 0) { return true; }
        float ext[16] = {};
        g_aiElemExt(obj, ext, b->elem);
        for (int k = 0; k < 6; ++k) { b->ext[k] = ext[k]; }
        const float* m = g_aiElemMtx(model, b->elem);
        for (int k = 0; k < 12; ++k) { b->m[k] = m[k]; }
        b->ok = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The geometry, on our copies only: every quad's two triangles (p0 p1 p3 / p0 p2 p3 - C48E30 +C49597..) in the element frame
// against the element's local box. Sets ov (quads overlapping) and dist (least sampled distance, 0 = overlap) and cw (box centre).
void AiBoneGeom(AiBone* b, const float* q, unsigned nq)
{
    b->ov = 0;
    b->dist = 1e9f;
    const float mn[3] = { std::fmin(b->ext[0], b->ext[3]), std::fmin(b->ext[1], b->ext[4]), std::fmin(b->ext[2], b->ext[5]) };
    const float mx[3] = { std::fmax(b->ext[0], b->ext[3]), std::fmax(b->ext[1], b->ext[4]), std::fmax(b->ext[2], b->ext[5]) };
    const float cl[3] = { 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2]) };
    swingshadow::Xform34(b->m, cl, b->cw);
    float inv[12];
    if (!swingshadow::Inverse34(b->m, inv)) { b->dist = -2.0f; return; }   // -2 = singular element matrix
    for (unsigned i = 0; i < nq; ++i) {
        const float* e = q + 13 * static_cast<size_t>(i);
        float p[4][3];
        for (int k = 0; k < 4; ++k) { swingshadow::Xform34(inv, e + 3 * k, p[k]); }
        const float d1 = swingshadow::TriAabbDist(mn, mx, p[0], p[1], p[3]);
        const float d2 = swingshadow::TriAabbDist(mn, mx, p[0], p[2], p[3]);
        if (d1 == 0.0f || d2 == 0.0f) { ++b->ov; }
        b->dist = std::fmin(b->dist, std::fmin(d1, d2));
    }
}

int AiAppend(char* buf, size_t cap, int at, const char* fmt, ...)
{
    if (at < 0 || static_cast<size_t>(at) >= cap) { return at; }
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(buf + at, cap - static_cast<size_t>(at), fmt, ap);
    va_end(ap);
    if (w < 0) { return at; }
    return (static_cast<size_t>(at + w) >= cap) ? static_cast<int>(cap) - 1 : at + w;
}

constexpr unsigned kAiQMax = 8192;           // quads copied per search (a corrupt-count guard; the count is logged)
float  g_aiQ[kAiQMax * 13];
char   g_aiRow[1 << 20];                      // one row at a time (game thread)

// Per B1 search, after the game's own (ShadowAfterSearch). COST: two pure game calls + per list entry one xform read; for an entry
// within kAiNearM one engine transform, one box distance, a vtable read and three virtual queries.
// The game's hits of THIS search call (the targets vector: { data, count }, 64-byte entries, +0 the object). False = unreadable.
bool AiGameHits(void* targets, unsigned long long* out, int* n, int cap)
{
    *n = 0;
    if (!targets) { return true; }
    __try {
        const char* data = *reinterpret_cast<char* const*>(targets);
        const unsigned count = *reinterpret_cast<const unsigned*>(static_cast<char*>(targets) + 8);
        for (unsigned i = 0; data && i < count && i < 4096; ++i) {
            const unsigned long long o = *reinterpret_cast<const unsigned long long*>(data + 64 * static_cast<size_t>(i));
            bool dup = false;
            for (int j = 0; j < *n; ++j) { if (out[j] == o) { dup = true; break; } }
            if (!dup && *n < cap) { out[(*n)++] = o; }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool AiCopyQuads(const HitParams& hp, unsigned* nq)
{
    *nq = hp.count < kAiQMax ? hp.count : kAiQMax;
    __try {
        std::memcpy(g_aiQ, hp.quads, static_cast<size_t>(*nq) * 52);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *nq = 0; return false; }
}

// Per B1 search, after the game's own (ShadowAfterSearch). COST: the old readout + per HumanAI within kAiNearM: ~12 guarded reads,
// two pure virtual queries, 21 x (3 engine const queries + a 3x4 inverse + per quad 4 transforms and two triangle-box tests with a
// 13-point sampled distance) + up to one Raytrace; plus one Raytrace per search and the Q / Z / K rows. The first search of a swing
// carries the backfill (~50-170 quads) - the slowest; its time is logged (us max on the [SWINGAI] line, us= on every Z row).
void SwingAiSearch(const HitParams& hp, void* targets)
{
    if (!AiResolve() || !hp.player || !hp.count) { return; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    float c[3] = {}, head[3] = {}, hb[4] = {};
    const float* hd = nullptr;
    __try {
        g_aiCentroid(c, &hp);
        const float r = g_aiRadius(&hp, c);
        g_aiSphere[0] = c[0]; g_aiSphere[1] = c[1]; g_aiSphere[2] = c[2]; g_aiSphere[3] = r;
        hd = VCall<const float*>(hp.player, 1840, static_cast<void*>(hb));
        head[0] = hd[0]; head[1] = hd[1]; head[2] = hd[2];
        g_aiSlot270 = VCall<unsigned char>(hp.player, 2160) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_aiWhy = "sphere / head point read faulted"; return; }
    uintptr_t data = 0;
    unsigned count = 0;
    if (!AiList(&data, &count)) { g_aiWhy = "AI manager list unreadable"; return; }
    ++g_aiCalls;
    if (static_cast<long>(count) > g_aiListMax) { g_aiListMax = static_cast<long>(count); }

    // This search's quads (the WIDENED array the game searched), ray 1's end point, ray 1, the game's hits.
    unsigned nq = 0;
    if (!AiCopyQuads(hp, &nq)) { g_aiWhy = "quad copy faulted"; }
    float cen[3] = {};
    const bool cenOk = swingshadow::Centroid(g_aiQ, nq, cen);
    const AiRayHit r1 = cenOk ? AiCast(hp.player, head, cen) : AiRayHit{};
    unsigned long long hits[64];
    int nHits = 0;
    if (!AiGameHits(targets, hits, &nHits, 64)) { g_aiWhy = "targets vector unreadable"; }
    {
        int at = AiAppend(g_aiRow, sizeof(g_aiRow), 0, "Q|swing=%ld|n=%ld|count=%u|copied=%u|head=%.4f,%.4f,%.4f|centroid=%.4f,%.4f,%.4f|"
                          "ray1hit=%d|ray1back=%llX|ray1surf=%u|ray1ok=%d|gameHits=%d|quads=", g_totFires, g_aiCalls, hp.count, nq,
                          head[0], head[1], head[2], cen[0], cen[1], cen[2], r1.hit ? 1 : 0, r1.back, static_cast<unsigned>(r1.surf),
                          r1.ok ? 1 : 0, nHits);
        for (unsigned i = 0; i < nq; ++i) {
            const float* e = g_aiQ + 13 * static_cast<size_t>(i);
            at = AiAppend(g_aiRow, sizeof(g_aiRow), at, "%s%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f",
                          i ? ";" : "", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12]);
        }
        // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        SHADOW_LOG("%s", g_aiRow);
    }

    const float r2 = g_aiSphere[3] * g_aiSphere[3];
    long nearHere = 0;
    bool replicaOv[64] = {};   // per game hit: did the replica overlap it this call (the known-case check)
    for (unsigned i = 0; i < count && i < 4096; ++i) {
        void* obj = nullptr;
        __try { obj = *reinterpret_cast<void* const*>(data + 8ull * i); } __except (EXCEPTION_EXECUTE_HANDLER) { obj = nullptr; }
        if (!obj) { continue; }
        AiRec tmp{};
        float dist = 1e9f, d2r2 = -1.0f;
        if (!AiProbeOne(obj, hp.player, head, c, r2, &dist, &tmp, &d2r2)) { g_aiWhy = "an entry's read faulted"; continue; }
        if (dist > kAiNearM) { continue; }
        ++nearHere;
        int j = 0;
        for (; j < g_aiN; ++j) { if (g_ai[j].obj == obj) { break; } }
        if (j == g_aiN) {
            if (g_aiN >= kAiMax) { ++g_aiOver; continue; }
            g_ai[g_aiN] = AiRec{};
            g_ai[g_aiN].obj = obj; g_ai[g_aiN].dist = dist; g_ai[g_aiN].human = tmp.human; g_ai[g_aiN].vt = tmp.vt;
            g_ai[g_aiN].minD2R2 = 1e9f; g_ai[g_aiN].bestD = 1e9f; g_ai[g_aiN].bestBone = -1; g_ai[g_aiN].bestSearch = -1;
            std::snprintf(g_ai[g_aiN].ray1Who, sizeof(g_ai[g_aiN].ray1Who), "-");
            ++g_aiN;
        }
        AiRec& a = g_ai[j];
        if (dist < a.dist) { a.dist = dist; a.pos[0] = tmp.pos[0]; a.pos[1] = tmp.pos[1]; a.pos[2] = tmp.pos[2]; }
        if (a.pos[0] == 0.0f && a.pos[1] == 0.0f && a.pos[2] == 0.0f) { a.pos[0] = tmp.pos[0]; a.pos[1] = tmp.pos[1]; a.pos[2] = tmp.pos[2]; }
        a.active = tmp.active; a.enemy = tmp.enemy;   // the latest search's reading
        if (d2r2 >= 0.0f && d2r2 < 1.0f) { ++a.inCalls; }
        if (d2r2 >= 0.0f) { a.minD2R2 = std::fmin(a.minD2R2, d2r2); }

        const unsigned long long ou = reinterpret_cast<unsigned long long>(obj);
        int hitIdx = -1;
        for (int k = 0; k < nHits; ++k) { if (hits[k] == ou) { hitIdx = k; break; } }
        if (hitIdx >= 0) { ++a.gameHitSearches; }
        if (!tmp.human) { continue; }   // the bone path below is read only for a HumanAI (slot 37 identity)

        // The search's gate, concept, model, masks.
        AiCand cd{};
        if (!AiCandRead(obj, hp.player, &cd)) { ++a.faults; g_aiWhy = "a candidate read faulted"; continue; }
        a.rawTeam = cd.team; a.rawState = cd.state; a.raw2352 = cd.b2352; a.raw4793 = cd.b4793;
        if (cd.gate == 0) { ++a.gateFail; }
        if (!cd.concept) { ++a.noConcept; }
        else if (!cd.model) { ++a.noModel; }
        if (cd.model && !cd.masks) { ++a.masksUnread; }
        // Ray 1 for THIS candidate (C48E30 +C493F7 condition: clear, or hit this candidate, or surface 3).
        const unsigned long long self18 = ou + 0x18;
        const swingshadow::Los v1 = r1.ok ? swingshadow::LosVerdict(r1.hit, r1.back, r1.surf, &self18, 1) : swingshadow::Los::BlockedWorld;
        const bool r1pass = r1.ok && (v1 == swingshadow::Los::Clear || v1 == swingshadow::Los::Target || v1 == swingshadow::Los::Water);
        char who[48] = "-";
        if (v1 == swingshadow::Los::Clear) { ++a.ray1Clear; }
        else if (v1 == swingshadow::Los::Target) { ++a.ray1Target; }
        else if (v1 == swingshadow::Los::Water) { ++a.ray1Water; }
        else {
            ++a.ray1Block;
            if (!r1.back) { std::snprintf(who, sizeof(who), "%s", r1.ok ? "world (no model)" : "ray 1 not cast"); }
            else if (!RttiClassNameOf(static_cast<uintptr_t>(r1.back), who, sizeof(who))) { std::snprintf(who, sizeof(who), "%llX", r1.back); }
            std::snprintf(a.ray1Who, sizeof(a.ray1Who), "%s", who);
        }

        // The 21 bones.
        AiBone bones[kAiBones] = {};
        int nNoElem = 0, nSkip = 0, nOv = 0, firstOv = -1;
        float bestHere = 1e9f;
        int bestBoneHere = -1;
        for (int b = 0; b < kAiBones; ++b) {
            AiBone& bn = bones[b];
            bn.elem = -1;
            bn.dist = -1.0f;
            if (!cd.model) { continue; }
            if (!AiBoneRead(obj, cd.model, kAiBoneIds[b], &bn)) { ++a.faults; continue; }
            if (bn.elem < 0) { ++nNoElem; continue; }
            bn.skip = cd.masks && ((cd.m58 >> kAiBoneIds[b]) & 1u) && ((cd.m56 >> kAiBoneIds[b]) & 1u);   // sub_1805E5080's test
            if (bn.skip) { ++nSkip; }
            if (!bn.ok) { continue; }
            AiBoneGeom(&bn, g_aiQ, nq);
            if (bn.ov > 0 && !bn.skip) { ++nOv; if (firstOv < 0 || bn.ov > bones[firstOv].ov) { firstOv = b; } }
            if (bn.dist >= 0.0f && bn.dist < bestHere) { bestHere = bn.dist; bestBoneHere = kAiBoneIds[b]; }
        }
        a.bonesNoElem += nNoElem;
        a.bonesSkipped += nSkip;
        if (bestBoneHere >= 0 && bestHere < a.bestD) { a.bestD = bestHere; a.bestBone = bestBoneHere; a.bestSearch = static_cast<int>(g_aiCalls); }
        // Ray 2 (APPROXIMATE: to the most-overlapped bone's box centre - the game's contact comes from sub_181131020, not called).
        const char* r2s = "not cast (no overlap)";
        if (nOv > 0) {
            ++a.ovSearches;
            if (r1pass && cd.gate != 0) {
                ++a.ovRay1Pass;
                const AiRayHit r2h = AiCast(hp.player, cen, bones[firstOv].cw);
                const swingshadow::Los v2 = r2h.ok ? swingshadow::LosVerdict(r2h.hit, r2h.back, r2h.surf, &self18, 1)
                                                   : swingshadow::Los::BlockedWorld;
                r2s = swingshadow::LosName(v2);
                if (v2 == swingshadow::Los::Blocked || v2 == swingshadow::Los::BlockedWorld) { ++a.ray2Block; }
                if (hitIdx < 0) { ++a.ovNotHitByGame; }
            } else {
                r2s = !r1pass ? "not cast (ray 1 refused)" : "not cast (gate refused)";
            }
            if (hitIdx >= 0) { replicaOv[hitIdx] = true; }
        }

        // Z row: everything for this candidate at this search.
        int at = AiAppend(g_aiRow, sizeof(g_aiRow), 0,
                          "Z|swing=%ld|n=%ld|obj=%p|pos=%.4f,%.4f,%.4f|dHead=%.3f|sphereD2R2=%.3f|team=%u|state=%u|b2352=%u|act3064=%u|"
                          "b4793=%u|enemy=%d|c888=%d|p870=%d|b12082=%d|gate=%d|ray1=%s|ray1who=%s|concept=%p|model=%p|modelSrc=%s|"
                          "getter=%llX(+%llX):%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X|read=%s|"
                          "m56=%08X|m58=%08X|masks=%d|gameHit=%d|bonesNoElem=%d|bonesSkipped=%d|bonesOverlap=%d|best=%d:%.4f|ray2=%s|bones=",
                          g_totFires, g_aiCalls, obj, tmp.pos[0], tmp.pos[1], tmp.pos[2], static_cast<double>(dist),
                          static_cast<double>(d2r2), cd.team, cd.state, cd.b2352, cd.b3064, cd.b4793, tmp.enemy ? 1 : 0, cd.c888,
                          cd.p870, cd.b12082, cd.gate, swingshadow::LosName(v1), who, cd.concept, cd.model, cd.modelSrc,
                          static_cast<unsigned long long>(cd.getterRva),
                          static_cast<unsigned long long>(cd.getterRva >= g_gd ? cd.getterRva - g_gd : 0),
                          cd.getterBytes[0], cd.getterBytes[1], cd.getterBytes[2], cd.getterBytes[3], cd.getterBytes[4],
                          cd.getterBytes[5], cd.getterBytes[6], cd.getterBytes[7], cd.getterBytes[8], cd.getterBytes[9],
                          cd.getterBytes[10], cd.getterBytes[11], cd.getterBytes[12], cd.getterBytes[13], cd.getterBytes[14],
                          cd.getterBytes[15], cd.why, cd.m56, cd.m58,
                          cd.masks, hitIdx >= 0 ? 1 : 0, nNoElem, nSkip, nOv, bestBoneHere, static_cast<double>(bestHere), r2s);
        for (int b = 0; b < kAiBones; ++b) {
            const AiBone& bn = bones[b];
            at = AiAppend(g_aiRow, sizeof(g_aiRow), at, "%s%u:%d:%d:%d:%.4f:%.4f,%.4f,%.4f", b ? ";" : "", kAiBoneIds[b], bn.elem,
                          bn.skip ? 1 : 0, bn.ov, static_cast<double>(bn.dist), bn.cw[0], bn.cw[1], bn.cw[2]);
        }
        // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        SHADOW_LOG("%s", g_aiRow);
        // K row: each bone's world 3x4 + local extents (for an offline replay of the same test).
        at = AiAppend(g_aiRow, sizeof(g_aiRow), 0, "K|swing=%ld|n=%ld|obj=%p|bones=", g_totFires, g_aiCalls, obj);
        for (int b = 0; b < kAiBones; ++b) {
            const AiBone& bn = bones[b];
            if (!bn.ok) { at = AiAppend(g_aiRow, sizeof(g_aiRow), at, "%s%u:-", b ? ";" : "", kAiBoneIds[b]); continue; }
            at = AiAppend(g_aiRow, sizeof(g_aiRow), at,
                          "%s%u:%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f:%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", b ? ";" : "",
                          kAiBoneIds[b], bn.m[0], bn.m[1], bn.m[2], bn.m[3], bn.m[4], bn.m[5], bn.m[6], bn.m[7], bn.m[8], bn.m[9],
                          bn.m[10], bn.m[11], bn.ext[0], bn.ext[1], bn.ext[2], bn.ext[3], bn.ext[4], bn.ext[5]);
        }
        // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        SHADOW_LOG("%s", g_aiRow);
    }
    // The known-case check: every object the GAME hit this call should overlap in the replica (only HumanAI within kAiNearM are
    // replicated - a hit on anything else counts as NOT CHECKED, not as a miss).
    for (int k = 0; k < nHits; ++k) {
        int j = 0;
        for (; j < g_aiN; ++j) { if (reinterpret_cast<unsigned long long>(g_ai[j].obj) == hits[k]) { break; } }
        if (j == g_aiN || !g_ai[j].human) { continue; }
        if (replicaOv[k]) { ++g_aiChkAgree; ++g_aiSesAgree; } else { ++g_aiChkMiss; ++g_aiSesMiss; }
    }
    if (nearHere > g_aiNearMax) { g_aiNearMax = nearHere; }
    QueryPerformanceCounter(&t1);
    const double us = static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpcToMs * 1000.0;
    if (us > g_aiUsMax) { g_aiUsMax = us; }
    // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
    SHADOW_LOG("E|swing=%ld|n=%ld|quads=%u|nearAI=%ld|gameHits=%d|check agree=%ld miss=%ld|us=%.1f", g_totFires, g_aiCalls, nq, nearHere,
               nHits, g_aiChkAgree, g_aiChkMiss, us);
}

// At END, after [SWINGMISS]: the swing's line + one per object. `verdict` = the miss probe's.
void SwingAiEnd(const char* verdict)
{
    if (g_aiReady != 1) { return; }
    ++g_aiSwings;   // uncapped - Jay 2026-11-19: "nothing log wise is capped for this probe"
    // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
    LOG("swing: [SWINGAI] #%ld %s | game hit %d | B1 searches read %ld, AI list max %ld, within %.1f m of the head point max %ld per "
        "search, %d object(s) (%d past the cap) | last sphere centre (%.3f %.3f %.3f) r %.3f m | player slot 270 %d (1 = the "
        "IsEnemy gate applies) | last problem: %s | KNOWN-CASE CHECK: of the game's hits on a HumanAI within %.1f m, the replica's "
        "bone test also overlapped %ld, did NOT %ld (session %ld / %ld) | slowest search %.0f us",
        g_totFires, verdict, g_shNGame, g_aiCalls, g_aiListMax,
        static_cast<double>(kAiNearM), g_aiNearMax, g_aiN, g_aiOver, static_cast<double>(g_aiSphere[0]),
        static_cast<double>(g_aiSphere[1]), static_cast<double>(g_aiSphere[2]), static_cast<double>(g_aiSphere[3]), g_aiSlot270,
        g_aiWhy, static_cast<double>(kAiNearM), g_aiChkAgree, g_aiChkMiss, g_aiSesAgree, g_aiSesMiss, g_aiUsMax);
    for (int i = 0; i < g_aiN; ++i) {
        const AiRec& a = g_ai[i];
        bool inGame = false, inNear = false;
        float nearD = -1.0f;
        for (int j = 0; j < g_shNGame; ++j) { if (g_shGame[j].obj == reinterpret_cast<unsigned long long>(a.obj)) { inGame = true; } }
        for (int j = 0; j < g_nearN; ++j) {
            if (g_near[j].obj == reinterpret_cast<unsigned long long>(a.obj)) { inNear = true; nearD = g_near[j].dist; }
        }
        // [PROBE: SWINGAI] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        LOG("swing: [SWINGAI]   #%ld obj %p vt +%llX %s | closest %.2f m from the head point at (%.3f %.3f %.3f) | raw: team(+320) "
            "%u state(+828) %u +2352 %u active(+3064) %d +4793 %u (NOT an alive reading) | IsEnemy %d | inside the search sphere on "
            "%d of %ld searches (best box^2/r^2 %.2f, < 1 = inside) | in the game's hits %d (on %d searches) | near probe %s%.3f | "
            "SEARCH PATH: gate refused %d, ray 1 clear %d / this zombie %d / water %d / BLOCKED %d (last by %s), no concept %d, no "
            "model %d, skip masks unread %d, bones with no element %d, bones skipped (masks) %d | BONES: closest %.3f m (bone %d, "
            "search %d; 0 = a quad triangle overlaps its box) | overlapped on %d searches, %d of them with ray 1 + gate passing, %d "
            "of THOSE not hit by the game | ray 2 (approx.) blocked %d | faults %d",
            g_totFires, a.obj, static_cast<unsigned long long>(a.vt - g_gd), a.human ? "HumanAI" : "not HumanAI",
            static_cast<double>(a.dist), static_cast<double>(a.pos[0]), static_cast<double>(a.pos[1]), static_cast<double>(a.pos[2]),
            a.rawTeam, a.rawState, static_cast<unsigned>(a.raw2352), a.active ? 1 : 0, static_cast<unsigned>(a.raw4793),
            a.enemy ? 1 : 0, a.inCalls, g_aiCalls, a.minD2R2 > 1e8f ? -1.0 : static_cast<double>(a.minD2R2), inGame ? 1 : 0,
            a.gameHitSearches, inNear ? "touched, dist " : "not touched ", inNear ? static_cast<double>(nearD) : -1.0,
            a.gateFail, a.ray1Clear, a.ray1Target, a.ray1Water, a.ray1Block, a.ray1Who, a.noConcept, a.noModel, a.masksUnread,
            a.bonesNoElem, a.bonesSkipped, a.bestD > 1e8f ? -1.0 : static_cast<double>(a.bestD), a.bestBone, a.bestSearch,
            a.ovSearches, a.ovRay1Pass, a.ovNotHitByGame, a.ray2Block, a.faults);
    }
}

void SwingAiReset()
{
    g_aiN = 0; g_aiOver = 0; g_aiCalls = 0; g_aiListMax = 0; g_aiNearMax = 0;
    g_aiChkAgree = 0; g_aiChkMiss = 0; g_aiUsMax = 0.0;
    g_aiWhy = g_aiReady == 1 ? "none" : g_aiWhy;
}

float PointSegDist(const float* p, const float* a, const float* b)
{
    const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float t = dd > 1e-12f ? ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1] + (p[2] - a[2]) * d[2]) / dd : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float q[3] = { a[0] + d[0] * t - p[0], a[1] + d[1] * t - p[1], a[2] + d[2] * t - p[2] };
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}

swingshadow::V3 ShV(const float* p) { return { p[0], p[1], p[2] }; }

// The weapon model RayLenOf reaches: player+816 -> its holder object (vfunc +8) -> +4128 + 8 x slot.
const void* ShWeaponModel(void* self, void* player)
{
    void* holder = VCall<void*>(static_cast<char*>(player) + 816, 8);
    return holder ? *reinterpret_cast<void**>(static_cast<char*>(holder) + 8 * (g_slotSel(self) ? 1 : 0) + 4128) : nullptr;
}

// Every DriverFrame, after ReadWorldGrip (so g_wrHead is this frame's index). COST with the probe on: 4 g_getWorld reads,
// one VCall pair to the weapon model, 0-3 g_getId on a model change only; probe off: one bool test.
void ShadowReadFrame(void* self, double tMs)
{
    if (g_wrHead < 0) { return; }
    g_swOk[g_wrHead] = false;
    g_swT[g_wrHead] = tMs;
    if (!DiagSwingShadow || MeleeSwingMode != 2) { return; }
    ++g_shFrames;
    if (!g_getId || !g_getWorld || !g_handElem || !g_slotSel) { ++g_shRefNoExport; return; }
    void* player = VCall<void*>(self, 32);
    if (!player) { ++g_shRefNoPlayer; return; }
    if (!ItemOf(self, player)) { ++g_shRefNoItem; return; }   // B1Frame's own check: no melee item, no weapon reads (I5)
    const void* wm = ShWeaponModel(self, player);
    if (!wm) { ++g_shRefNoModel; return; }
    if (wm != g_swModel || g_shResolve) {
        // A new weapon model (or a FIRE - a freed model can come back at the same address): resolve the ids. Only a REAL
        // change leaves this frame not-ok, so no quad bridges two weapons (Review Focus 2).
        const int idH = g_getId(wm, "Holder"), idT = g_getId(wm, "DTrailStart1"), idE = g_getId(wm, "DTrailEnd1");
        const bool changed = wm != g_swModel || idH != g_swIdHold || idT != g_swIdTip || idE != g_swIdEnd;
        g_shResolve = false;
        g_swModel = wm;
        g_swIdHold = idH;
        g_swIdTip = idT;
        g_swIdEnd = idE;
        if (changed) {
            if (true) {   // was `g_swModelLines < 40` - uncapped for the probe run (trace 20261119_1730)
                ++g_swModelLines;
                // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
                LOG("swing: [SWINGSHADOW] weapon model %p: Holder %d, DTrailStart1 %d, DTrailEnd1 %d%s (the mesh is named by "
                    "the AFWCAT / TWOHAND lines beside this one)",
                    wm, g_swIdHold, g_swIdTip, g_swIdEnd,
                    g_swIdHold < 0 ? " - NO Holder: REFUSED for this model" :
                    (g_swIdTip < 0 ? " - NO DTrailStart1: REFUSED for this model" : ""));
            }
            return;
        }
    }
    if (g_swIdHold < 0) { ++g_shRefNoHold; return; }
    if (g_swIdTip < 0) { ++g_shRefNoTip; return; }
    const float* h = g_getWorld(wm, g_swIdHold);
    const float* t = g_getWorld(wm, g_swIdTip);
    const float* e = g_swIdEnd >= 0 ? g_getWorld(wm, g_swIdEnd) : nullptr;
    const float* hh = g_getWorld(player, static_cast<int>(g_handElem(self, 0)));
    if (!h || !t || !hh) { ++g_shRefRead; return; }
    ShadowFrame& f = g_sw[g_wrHead];
    f.hold[0] = h[3]; f.hold[1] = h[7]; f.hold[2] = h[11];
    f.holdY[0] = h[1]; f.holdY[1] = h[5]; f.holdY[2] = h[9];
    f.tip[0] = t[3]; f.tip[1] = t[7]; f.tip[2] = t[11];
    f.end1[0] = e ? e[3] : 0.0f; f.end1[1] = e ? e[7] : 0.0f; f.end1[2] = e ? e[11] : 0.0f;
    for (int i = 0; i < 12; ++i) { f.hh[i] = hh[i]; }
    g_swOk[g_wrHead] = swingshadow::Finite(ShV(f.hold)) && swingshadow::Finite(ShV(f.tip));
    if (!g_swOk[g_wrHead]) { ++g_shRefRead; }
}

// At FIRE (probe on): open the swing. A swing that FIREd before the probe was on is never opened -> INCOMPLETE.
void ShadowFireStart()
{
    g_shOpen = DiagSwingShadow && MeleeSwingMode == 2;
    g_shComplete = true;
    g_shNGame = g_shNBlade = 0;
    g_nearN = 0;   // the miss probe's near set, per swing
    SwingAiReset();   // [SWINGAI] per swing (trace 20261119_1730)
    g_shSwCalls = 0;
    g_shUsSum = g_shUsMax = 0.0;
    g_shTotUsSum = g_shTotUsMax = 0.0;
    g_shFireGeo = false;
    g_shTrunc = 0;
    g_shReason = "none";
    g_shResolve = true;
    // Review I1: never print the previous swing's FIRE geometry as this one's - -1 = not measured this swing.
    g_shFireReach = g_shFireStartVsHold = g_shFireAngle = g_shFireClosest = g_shFireWeaponLen = -1.0f;
    g_shFireArm[0] = g_shFireArm[1] = g_shFireArm[2] = -1.0f;
}

void ShadowTargetsRead(void* vec, swingshadow::Key* set, int& n, const char* which)
{
    // The targets vector: { void* data +0; uint32 count +8; uint32 cap +12 }, 64-byte entries (sub_180DCEB00's realloc
    // v5 << 6; sub_180C4D810's writer): +0 the target object, +8 the element, +24..+32 the contact point,
    // +48 the bone (GetBoneIDFromMeshElem, sub_180C48E30:575), +44 the score (sub_180C48E30:566-649).
    const char* data = *reinterpret_cast<char* const*>(vec);
    const unsigned count = *reinterpret_cast<const unsigned*>(static_cast<char*>(vec) + 8);
    for (unsigned i = 0; data && i < count && i < 4096; ++i) {   // 4096 = a corrupt-count guard only (was 64 - a log cap)
        const char* en = data + 64 * static_cast<size_t>(i);
        const unsigned long long obj = *reinterpret_cast<const unsigned long long*>(en);
        const int elem = *reinterpret_cast<const int*>(en + 8);
        const float* cp = reinterpret_cast<const float*>(en + 24);
        ShAddKey(set, n, { obj, elem });
        SHADOW_LOG("T|swing=%ld|list=%s|n=%ld|obj=%llX|elem=%d|bone48=%d|contact=%.4f,%.4f,%.4f|score44=%.4f|f56=%.3f", g_totFires,
                   which, g_shSwCalls, obj, elem, *reinterpret_cast<const int*>(en + 48), cp[0], cp[1], cp[2],
                   *reinterpret_cast<const float*>(en + 44), *reinterpret_cast<const float*>(en + 56));
    }
}

// In B1Build, right after the game's own g_search, only when this run built quads. COST with the probe on, per B1Build run
// of an active swing: one more widening + search (the game's own cost, x1), the quads' realloc, two g_free, one file row
// per frame and per target; probe off: one bool test at the call site.
// The REAL hit shape's segments of one ring frame (what B1SubStep swept for it): the primary segment (the drawn line, or
// today's controller line) + the box edges. Returns the count (<= 1 + kMaxBoxSegs).
int RealSegs(const WorldFrame& w, float s[][3], float e[][3])
{
    int n = 0;
    for (int k = 0; k < 3; ++k) {
        s[0][k] = w.seg.blade ? w.seg.s[k] : w.pos[k];
    }
    e[0][0] = w.seg.blade ? w.seg.e[0] : w.pos[0] + g_reachM * w.blade.x;
    e[0][1] = w.seg.blade ? w.seg.e[1] : w.pos[1] + g_reachM * w.blade.y;
    e[0][2] = w.seg.blade ? w.seg.e[2] : w.pos[2] + g_reachM * w.blade.z;
    n = 1;
    if (w.seg.blade && w.box.ok && w.box.row >= 0 && w.box.row < static_cast<int>(kBladeRowsN)) {
        n += bladeline::RowSegments(w.box.m, kBladeRows[w.box.row], kBladeBoxes, kBladeBoxesN, s + 1, e + 1,
                                    bladeline::kMaxBoxSegs);
    }
    return n;
}

// Distance from a point to the real shape swept between two ring frames: segments of both frames and their halfway blend.
float RealShapeDist(const float* c, int ia, int ib)
{
    float sa[1 + bladeline::kMaxBoxSegs][3], ea[1 + bladeline::kMaxBoxSegs][3];
    float sb[1 + bladeline::kMaxBoxSegs][3], eb[1 + bladeline::kMaxBoxSegs][3];
    const int na = RealSegs(g_wr[ia], sa, ea), nb = RealSegs(g_wr[ib], sb, eb);
    float best = 1e9f;
    for (int i = 0; i < na; ++i) { best = std::fmin(best, PointSegDist(c, sa[i], ea[i])); }
    for (int i = 0; i < nb; ++i) { best = std::fmin(best, PointSegDist(c, sb[i], eb[i])); }
    for (int i = 0; i < na && i < nb; ++i) {   // the same segment k at the frame halfway (same row -> same order)
        float ms[3], me[3];
        for (int k = 0; k < 3; ++k) { ms[k] = 0.5f * (sa[i][k] + sb[i][k]); me[k] = 0.5f * (ea[i][k] + eb[i][k]); }
        best = std::fmin(best, PointSegDist(c, ms, me));
    }
    // §9.2: the capsule B1SubStep swept for this pair - the extended primary of both frames and its two copies.
    const WorldFrame& wa = g_wr[ia];
    const WorldFrame& wb = g_wr[ib];
    if (wa.seg.blade && wb.seg.blade && (MeleeHitThicknessM > 0.0f || MeleeHitBackM > 0.0f)) {
        bladeline::Seg ca = wa.seg, cb = wb.seg;
        if (bladeline::Capsule(ca.s, ca.e, MeleeHitBackM, MeleeHitThicknessM) &&
            bladeline::Capsule(cb.s, cb.e, MeleeHitBackM, MeleeHitThicknessM)) {
            best = std::fmin(best, PointSegDist(c, ca.s, ca.e));
            best = std::fmin(best, PointSegDist(c, cb.s, cb.e));
            float as[2][3], ae[2][3], bs[2][3], be[2][3];
            const int nc = bladeline::ThickCopies(ca, cb, MeleeHitThicknessM, as, ae, bs, be);
            for (int k = 0; k < nc; ++k) {
                best = std::fmin(best, PointSegDist(c, as[k], ae[k]));
                best = std::fmin(best, PointSegDist(c, bs[k], be[k]));
            }
        }
    }
    return best;
}

// The near search (cost with the probe on: one more widening + search per B1Build run, as the shadow's own; 3 quads per pair).
void ShadowNearSearch(void* self, const HitParams& hp, const int pairs[][2], int np)
{
    HitParams nhp{};
    nhp.tag = 59;
    nhp.player = hp.player;
    nhp.v[0] = hp.v[0]; nhp.v[1] = hp.v[1]; nhp.v[2] = hp.v[2];
    nhp.list1928 = hp.list1928;
    nhp.one = 1;
    float dir[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < np; ++i) {
        const ShadowFrame& a = g_sw[pairs[i][0]];
        const ShadowFrame& b = g_sw[pairs[i][1]];
        float da[3], db[3], m[3], nrm[3];
        Norm3(a.tip, a.hold, da);
        Norm3(b.tip, b.hold, db);
        for (int k = 0; k < 3; ++k) { m[k] = b.tip[k] - a.tip[k]; }
        nrm[0] = db[1] * m[2] - db[2] * m[1];
        nrm[1] = db[2] * m[0] - db[0] * m[2];
        nrm[2] = db[0] * m[1] - db[1] * m[0];
        const float ln = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        const int nOff = ln > 1e-6f ? 3 : 1;   // no motion between the frames: the middle line only
        for (int k = 0; k < 3; ++k) { nrm[k] = ln > 1e-6f ? nrm[k] / ln : 0.0f; }
        for (int o = 0; o < nOff; ++o) {
            const float off = o == 0 ? 0.0f : (o == 1 ? kNearOffM : -kNearOffM);
            float ps[3], pe[3], cs[3], ce[3];
            for (int k = 0; k < 3; ++k) {
                ps[k] = a.hold[k] + nrm[k] * off;
                pe[k] = a.hold[k] + da[k] * kNearLenM + nrm[k] * off;
                cs[k] = b.hold[k] + nrm[k] * off;
                ce[k] = b.hold[k] + db[k] * kNearLenM + nrm[k] * off;
            }
            if (!AppendQuad(nhp, ps, pe, cs, ce)) { g_free(nhp.quads); ++g_shRefMem; return; }
        }
        Norm3(b.tip, a.tip, dir);
    }
    struct { void* data; unsigned count; unsigned cap; unsigned tag; unsigned pad; } scratch = { nullptr, 0, 0, 59, 0 };
    g_search(self, &nhp, &scratch, dir);
    ++g_nearCalls;
    const char* data = static_cast<const char*>(scratch.data);
    for (unsigned i = 0; data && i < scratch.count && i < 4096; ++i) {   // corrupt-count guard only (was 64)
        const char* en = data + 64 * static_cast<size_t>(i);
        const unsigned long long obj = *reinterpret_cast<const unsigned long long*>(en);
        const int elem = *reinterpret_cast<const int*>(en + 8);
        const float* cp = reinterpret_cast<const float*>(en + 24);
        float best = 1e9f;
        for (int p = 0; p < np; ++p) { best = std::fmin(best, RealShapeDist(cp, pairs[p][0], pairs[p][1])); }
        SHADOW_LOG("T|swing=%ld|list=near|n=%ld|obj=%llX|elem=%d|bone48=%d|contact=%.4f,%.4f,%.4f|dist=%.4f|f56=%.3f",
                   g_totFires, g_shSwCalls, obj, elem, *reinterpret_cast<const int*>(en + 48), cp[0], cp[1], cp[2],
                   static_cast<double>(best), static_cast<double>(*reinterpret_cast<const float*>(en + 56)));
        int j = 0;
        for (; j < g_nearN; ++j) { if (g_near[j].obj == obj) { break; } }
        if (j == g_nearN) {
            if (g_nearN >= kShKeys) { ++g_nearOverflow; continue; }
            g_near[g_nearN++] = { obj, 1e9f, elem, g_shSwCalls, { cp[0], cp[1], cp[2] } };
        }
        if (best < g_near[j].dist) {
            g_near[j].dist = best;
            g_near[j].elem = elem;
            g_near[j].call = g_shSwCalls;
            for (int k = 0; k < 3; ++k) { g_near[j].contact[k] = cp[k]; }
        }
    }
    g_free(scratch.data);
    g_free(nhp.quads);
}

void ShadowAfterSearchBody(void* self, const HitParams& hp, void* targets, bool backfill)
{
    if (g_dbgByte && *g_dbgByte) { ++g_shRefDebug; g_shComplete = false; g_shReason = "debug-draw byte set"; return; }
    int pairs[kWorldRing][2];
    bool trunc = false;
    const int np = swingshadow::ShadowPairs(g_swT, g_swOk, g_wrN, g_wrHead, kWorldRing, g_candStartMs, backfill, pairs,
                                            kWorldRing, &trunc);
    if (trunc) { g_shTrunc = 1; }
    // The game's list first - read whatever the shadow does next.
    ShadowTargetsRead(targets, g_shGame, g_shNGame, "game");
    if (np == 0) {
        ++g_shRefNoPair;
        g_shComplete = false;
        g_shReason = "no ok frame pair (no Holder / DTrailStart1, a failed read, no melee item, or the weapon changed)";
        return;
    }

    HitParams shp{};
    shp.tag = 59;
    shp.player = hp.player;
    shp.v[0] = hp.v[0]; shp.v[1] = hp.v[1]; shp.v[2] = hp.v[2];
    shp.list1928 = hp.list1928;
    shp.one = 1;
    float dir[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < np; ++i) {
        const ShadowFrame& a = g_sw[pairs[i][0]];
        const ShadowFrame& b = g_sw[pairs[i][1]];
        const unsigned need = shp.count + 1;   // B1SubStep's growth rule, through the gamedll IAT realloc (F9 / F10)
        if (need > shp.cap) {
            unsigned cap = (((10u * shp.cap) >> 3) + 16u) & 0xFFFFFFF0u;
            if (need > cap) { cap = need; }
            void* grown = g_realloc(shp.quads, static_cast<size_t>(52) * cap);
            if (!grown) { ++g_shRefMem; g_shComplete = false; g_shReason = "quad allocation failed"; g_free(shp.quads); return; }
            shp.quads = grown;
            shp.cap = cap;
        }
        float* e = static_cast<float*>(g_quadInit(static_cast<char*>(shp.quads) + static_cast<size_t>(52) * shp.count));
        for (int k = 0; k < 3; ++k) {          // prev start, prev end, cur start, cur end (B1SubStep :526-529)
            e[k] = a.hold[k];
            e[3 + k] = a.tip[k];
            e[6 + k] = b.hold[k];
            e[9 + k] = b.tip[k];
        }
        shp.count = need;
        Norm3(b.tip, a.tip, dir);              // normalise(cur end - prev end) - LOCAL, never this+1892
    }
    struct { void* data; unsigned count; unsigned cap; unsigned tag; unsigned pad; } scratch = { nullptr, 0, 0, 59, 0 };
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    g_search(self, &shp, &scratch, dir);
    QueryPerformanceCounter(&q1);
    const double us = static_cast<double>(q1.QuadPart - q0.QuadPart) * g_qpcToMs * 1000.0;
    ++g_shCalls;
    ++g_shSwCalls;
    g_shUsSum += us;
    if (us > g_shUsMax) { g_shUsMax = us; }
    ShadowTargetsRead(&scratch, g_shBlade, g_shNBlade, "blade");
    g_free(scratch.data);
    g_free(shp.quads);                         // the widening may have realloc'd it - free what it handed back (F9)
    ShadowNearSearch(self, hp, pairs, np);     // the miss probe's inflated "near" search (its cost at its definition)

    // This frame's geometry: the B1 hit line (g_cur, reach) vs the drawn blade segment (the newest shadow frame) - ONLY when
    // that frame is ok (review I2: a not-ok slot holds a frame from a ring lap ago), never the FIRE geometry from it either.
    if (!g_swOk[g_wrHead]) {
        ++g_shSkipGeo;
        SHADOW_LOG("F|swing=%ld|n=%ld|tMs=%.1f|speed=%.2f|headOk=0|pairs=%d|us=%.1f", g_totFires, g_shSwCalls, g_shTMs,
                   g_shSpeed, np, us);
        return;
    }
    const ShadowFrame& c = g_sw[g_wrHead];
    // The line the game tests THIS frame: the blade-length line when this frame had one, else today's (grip + reach x blade).
    const swingshadow::V3 l0 = g_cur.seg.blade ? ShV(g_cur.seg.s) : ShV(g_cur.pos);
    const swingshadow::V3 l1 = g_cur.seg.blade ? ShV(g_cur.seg.e)
                                               : swingshadow::V3{ g_cur.pos[0] + g_reachM * g_cur.blade.x,
                                                                  g_cur.pos[1] + g_reachM * g_cur.blade.y,
                                                                  g_cur.pos[2] + g_reachM * g_cur.blade.z };
    const swingshadow::V3 b0 = ShV(c.hold), b1 = ShV(c.tip);
    float sP = 0.0f, tP = 0.0f;
    const float closest = swingshadow::SegSegDist(l0, l1, b0, b1, &sP, &tP);
    const swingshadow::V3 lineDir = swingshadow::Sub(l1, l0), bladeDir = swingshadow::Sub(b1, b0);
    const float ang = swingshadow::AngleDeg(lineDir, bladeDir);
    const swingshadow::V3 hhY = { c.hh[1], c.hh[5], c.hh[9] };
    const float startVsHold = std::sqrt(swingshadow::Dot(swingshadow::Sub(l0, b0), swingshadow::Sub(l0, b0)));
    const float weaponLen = std::sqrt(swingshadow::Dot(bladeDir, bladeDir));
    float arm[3] = { -1.0f, -1.0f, -1.0f };
    float sh[3] = { 0.0f, 0.0f, 0.0f };
    const bool haveArm = SwingArmRead(sh, &arm[0], &arm[1], &arm[2]);
    SHADOW_LOG("F|swing=%ld|n=%ld|tMs=%.1f|speed=%.2f|headOk=1|lineBlade=%d|ageMs=%.1f|reach=%.3f|line=%.4f,%.4f,%.4f>%.4f,%.4f,%.4f|blade=%.4f,%.4f,%.4f>%.4f,%.4f,%.4f|"
               "end1=%.4f,%.4f,%.4f|angLineBlade=%.2f|closest=%.4f|s=%.3f|t=%.3f|startVsHold=%.4f|angLineHHY=%.2f|"
               "angBladeHHY=%.2f|angBladeHoldY=%.2f|hh=%.5f,%.5f,%.5f,%.4f,%.5f,%.5f,%.5f,%.4f,%.5f,%.5f,%.5f,%.4f|pairs=%d|"
               "trunc=%d|us=%.1f|arm=%d,%.4f,%.4f,%.4f|shoulder=%.4f,%.4f,%.4f",
               g_totFires, g_shSwCalls, g_shTMs, g_shSpeed, g_cur.seg.blade ? 1 : 0, static_cast<double>(g_cur.ageMs),
               static_cast<double>(g_reachM), l0.x, l0.y, l0.z,
               l1.x, l1.y, l1.z, b0.x, b0.y, b0.z, b1.x, b1.y, b1.z, c.end1[0], c.end1[1], c.end1[2], ang, closest, sP, tP,
               startVsHold, swingshadow::AngleDeg(lineDir, hhY), swingshadow::AngleDeg(bladeDir, hhY),
               swingshadow::AngleDeg(bladeDir, ShV(c.holdY)), c.hh[0], c.hh[1], c.hh[2], c.hh[3], c.hh[4], c.hh[5], c.hh[6],
               c.hh[7], c.hh[8], c.hh[9], c.hh[10], c.hh[11], np, trunc ? 1 : 0, us, haveArm ? 1 : 0, arm[0], arm[1], arm[2],
               sh[0], sh[1], sh[2]);
    if (!g_shFireGeo) {
        // The swing's first shadowed run = its FIRE geometry for the SUMMARY, plus the widening's axis (U-66 4): PlayerDI
        // slot 197 (vfunc +1576, the call sub_180DCA810 makes) beside the look dir (+1864) and the hand holder's Y.
        g_shFireGeo = true;
        g_shFireReach = g_reachM;
        g_shFireStartVsHold = startVsHold;
        g_shFireAngle = ang;
        g_shFireClosest = closest;
        g_shFireWeaponLen = weaponLen;
        for (int k = 0; k < 3; ++k) { g_shFireArm[k] = arm[k]; }
        float t197[4] = {}, tLook[4] = {};
        const float* ax = VCall<const float*>(hp.player, 1576, static_cast<void*>(t197));
        const float* lk = VCall<const float*>(hp.player, 1864, static_cast<void*>(tLook));
        SHADOW_LOG("A|swing=%ld|slot197=%.5f,%.5f,%.5f|look=%.5f,%.5f,%.5f|hhY=%.5f,%.5f,%.5f|holdY=%.5f,%.5f,%.5f",
                   g_totFires, ax[0], ax[1], ax[2], lk[0], lk[1], lk[2], hhY.x, hhY.y, hhY.z, c.holdY[0], c.holdY[1],
                   c.holdY[2]);
    }
}

// The call site's entry: times the WHOLE shadow (search + reads + file rows, review I4) on the game thread.
void ShadowAfterSearch(void* self, const HitParams& hp, void* targets, bool backfill)
{
    if (!g_shOpen) { return; }
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    ShadowAfterSearchBody(self, hp, targets, backfill);
    SwingAiSearch(hp, targets);   // [SWINGAI]: the game's own candidate list + per-bone path at THIS search (trace 20261119_1730)
    QueryPerformanceCounter(&q1);
    const double us = static_cast<double>(q1.QuadPart - q0.QuadPart) * g_qpcToMs * 1000.0;
    g_shTotUsSum += us;
    if (us > g_shTotUsMax) { g_shTotUsMax = us; }
}

// At END: classify and write the SUMMARY (only for a swing the probe opened).
void ShadowEndSwing(bool madeB1)
{
    if (!g_shOpen) { return; }
    g_shOpen = false;
    const bool complete = g_shComplete && g_shSwCalls > 0;
    const swingshadow::Tally t = swingshadow::Classify(g_shGame, g_shNGame, g_shBlade, g_shNBlade, madeB1, complete);
    ++g_shCls[static_cast<int>(t.cls)];
    // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
    LOG("swing: [SWINGSHADOW] #%ld %s | game %d blade %d both %d (game only %d, blade only %d) | FIRE: reach %.3f m, hit "
        "line start vs drawn holder %.3f m, angle line vs blade %.1f deg, closest %.3f m | arm asked %.3f drawn %.3f reach "
        "%.3f m | weapon holder->DTrailStart1 %.3f m (-1 = not measured this swing) | shadow calls %ld, search us mean %.1f "
        "max %.1f, whole shadow incl. rows us total %.1f max %.1f | ring truncated %d | last refusal %s",
        g_totFires, swingshadow::ClsName(t.cls), g_shNGame, g_shNBlade, t.both, t.gameOnly, t.bladeOnly,
        static_cast<double>(g_shFireReach), static_cast<double>(g_shFireStartVsHold), static_cast<double>(g_shFireAngle),
        static_cast<double>(g_shFireClosest), static_cast<double>(g_shFireArm[0]), static_cast<double>(g_shFireArm[1]),
        static_cast<double>(g_shFireArm[2]), static_cast<double>(g_shFireWeaponLen), g_shSwCalls,
        g_shSwCalls ? g_shUsSum / static_cast<double>(g_shSwCalls) : 0.0, g_shUsMax, g_shTotUsSum, g_shTotUsMax, g_shTrunc,
        g_shReason);
    SHADOW_LOG("S|swing=%ld|class=%s|game=%d|blade=%d|both=%d|gameOnly=%d|bladeOnly=%d", g_totFires, swingshadow::ClsName(t.cls),
               g_shNGame, g_shNBlade, t.both, t.gameOnly, t.bladeOnly);

    // The miss probe's verdict for this swing (spec: HIT / INSIDE NOT HIT / NEAR MISS / NOTHING NEAR).
    int inside = 0, nearMiss = 0, nearHit = 0;
    float missMin = 1e9f, insideMin = 1e9f;
    char list[4096] = {};   // was 256 (~5 objects) - every touched object listed (trace 20261119_1730)
    size_t at = 0;
    for (int i = 0; i < g_nearN; ++i) {
        bool hit = false;
        for (int j = 0; j < g_shNGame; ++j) { if (g_shGame[j].obj == g_near[i].obj) { hit = true; break; } }
        const float d = g_near[i].dist;
        if (hit) { ++nearHit; }
        else if (d <= kInsideM) { ++inside; insideMin = std::fmin(insideMin, d); }
        else { ++nearMiss; missMin = std::fmin(missMin, d); }
        if (at < sizeof(list) - 40) {
            const int w = std::snprintf(list + at, sizeof(list) - at, "%s%s %.2f@%ld", at ? ", " : "",
                                        hit ? "hit" : (d <= kInsideM ? "INSIDE" : "miss"), static_cast<double>(d), g_near[i].call);
            if (w > 0) { at += static_cast<size_t>(w); }
        }
    }
    const char* verdict = g_shNGame > 0 ? "HIT" : (inside > 0 ? "INSIDE NOT HIT" : (nearMiss > 0 ? "NEAR MISS" : "NOTHING NEAR"));
    ++g_missCls[g_shNGame > 0 ? 0 : (inside > 0 ? 1 : (nearMiss > 0 ? 2 : 3))];
    // [PROBE: SWINGMISS] gate=diag_swing_shadow  site=HEADSET  log-only=yes
    LOG("swing: [SWINGMISS] #%ld %s | game hit %d object(s) | near shape touched %d: hit %d, inside the real shape but NOT hit %d "
        "(closest %.3f m), outside %d (closest %.3f m) | per object (verdict dist m @ shadow call): %s | near calls %ld",
        g_totFires, verdict, g_shNGame, g_nearN, nearHit, inside, inside ? static_cast<double>(insideMin) : -1.0, nearMiss,
        nearMiss ? static_cast<double>(missMin) : -1.0, g_nearN ? list : "none", g_shSwCalls);
    SHADOW_LOG("N|swing=%ld|verdict=%s|game=%d|near=%d|nearHit=%d|inside=%d|insideMin=%.4f|outside=%d|outsideMin=%.4f",
               g_totFires, verdict, g_shNGame, g_nearN, nearHit, inside, inside ? static_cast<double>(insideMin) : -1.0,
               nearMiss, nearMiss ? static_cast<double>(missMin) : -1.0);
    SwingAiEnd(verdict);   // [SWINGAI] (trace 20261119_1730)
}

// Once per DriverFrame: the ARMED / REFUSED line (first time, unconditional) and the 5 s totals. The 5 s stamp sits above
// every early return but the switch itself, with its own key (INV-9 / INV-10). COST: one bool test, one tick read.
void ShadowTick()
{
    if (!DiagSwingShadow) { return; }
    if (!g_shArmedSaid) {
        g_shArmedSaid = 1;
        // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        LOG("swing: [SWINGSHADOW] ARMED (logged unconditionally) | melee_swing_mode %d | search %p quadInit %p realloc %p "
            "free %p | debug-draw byte %s | per-frame rows -> dlvr_swingshadow.log",
            MeleeSwingMode, reinterpret_cast<void*>(g_search), reinterpret_cast<void*>(g_quadInit),
            reinterpret_cast<void*>(g_realloc), reinterpret_cast<void*>(g_free),
            g_dbgByte ? (*g_dbgByte ? "SET - every shadow call REFUSED" : "clear") : "NOT RESOLVED (builder not installed yet)");
    }
    if (!g_shLiveSaid && g_builderLive) {
        // Review: the ARMED line goes out before the 1 Hz install, so it shows nulls - say the resolved state once it is live.
        g_shLiveSaid = 1;
        // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        LOG("swing: [SWINGSHADOW] builder LIVE | search %p quadInit %p realloc %p free %p | debug-draw byte %s",
            reinterpret_cast<void*>(g_search), reinterpret_cast<void*>(g_quadInit), reinterpret_cast<void*>(g_realloc),
            reinterpret_cast<void*>(g_free),
            g_dbgByte ? (*g_dbgByte ? "SET - every shadow call REFUSED" : "clear") : "NOT RESOLVED");
    }
    if (MeleeSwingMode != 2 && !g_shModeSaid) {
        g_shModeSaid = 1;
        // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
        LOG("swing: [SWINGSHADOW] REFUSED - melee_swing_mode=%d, the probe needs 2 (B1). Nothing is shadowed.", MeleeSwingMode);
    }
    const ULONGLONG now = GetTickCount64();
    if (g_shLast5s == 0) { g_shLast5s = now; }
    if (now - g_shLast5s < 5000) { return; }
    g_shLast5s = now;
    using swingshadow::Cls;
    // [PROBE: SWINGSHADOW] gate=diag_swing_shadow  site=HEADSET  log-only=yes
    LOG("swing: [SWINGSHADOW] 5 s | swings BOTH %ld GAME ONLY %ld BLADE ONLY %ld NEITHER %ld SPLIT %ld INCOMPLETE %ld "
                 "NO B1 %ld | shadow calls %ld | frames %ld | refused: no export %ld, no player %ld, no weapon model %ld, no "
                 "Holder %ld, no DTrailStart1 %ld, read %ld, debug byte %ld, no ok pair %ld, alloc %ld, no melee item %ld | "
                 "target sets full %ld | geometry skipped (newest frame not ok) %ld | miss probe swings: HIT %ld, INSIDE NOT "
                 "HIT %ld, NEAR MISS %ld, NOTHING NEAR %ld | near calls %ld, near sets full %ld",
                 g_shCls[static_cast<int>(Cls::Both)], g_shCls[static_cast<int>(Cls::GameOnly)],
                 g_shCls[static_cast<int>(Cls::BladeOnly)], g_shCls[static_cast<int>(Cls::Neither)],
                 g_shCls[static_cast<int>(Cls::Split)], g_shCls[static_cast<int>(Cls::Incomplete)],
                 g_shCls[static_cast<int>(Cls::NoB1)], g_shCalls, g_shFrames, g_shRefNoExport, g_shRefNoPlayer,
                 g_shRefNoModel, g_shRefNoHold, g_shRefNoTip, g_shRefRead, g_shRefDebug, g_shRefNoPair, g_shRefMem,
                 g_shRefNoItem, g_shOverflow, g_shSkipGeo, g_missCls[0], g_missCls[1], g_missCls[2], g_missCls[3],
                 g_nearCalls, g_nearOverflow);
}

#else   // DLVR_BETA_LEAN: the probe is compiled out - no-op stubs keep the call sites identical to DEV
bool g_shOpen = false;
bool g_shComplete = false;
double g_shSpeed = 0.0;
double g_shTMs = 0.0;
inline void ShadowReadFrame(void*, double) {}
inline void ShadowFireStart() {}
inline void ShadowAfterSearch(void*, const HitParams&, void*, bool) {}
inline void ShadowEndSwing(bool) {}
inline void ShadowTick() {}
#endif   // !DLVR_BETA_LEAN

// A port of sub_180DC2000:1074-1210 with the animation removed (spec §2.3). One sub-step per game frame, from last
// frame's controller segment to this frame's - EXCEPT on the swing's first B1Build run, which BACKFILLS: one sub-step
// per consecutive pair of ok ring frames from the frame at/just before the candidate start up to this frame, so the
// sweep covers the whole stroke although FIRE now waits for minTravelM of path. All the quads go into ONE HitParams,
// then DB63A0 + DCA810 once, then free - exactly the original's loop shape.
char B1Build(void* self, void* targets, void* item, float* record)
{
    HitParams hp{};
    hp.tag = 59;
    hp.player = VCall<void*>(self, 32);
    float tmp[4] = {};
    const float* pv3 = VCall<const float*>(hp.player, 1864, static_cast<void*>(tmp));
    hp.v[0] = pv3[0]; hp.v[1] = pv3[1]; hp.v[2] = pv3[2];
    hp.list1928 = static_cast<char*>(self) + 1928;
    hp.one = 1;
    hp.quads = nullptr;                   // :1075 - count 0, cap 0; the first sub-step's growth allocates (cap 16)
    hp.count = 0;
    hp.cap = 0;

    // The backfill: the swing's first B1Build run only (set at FIRE by DriverFrame, consumed here whatever happens).
    const bool backfill = g_backfillPending;
    g_backfillPending = false;
    int span = 0;                         // ring entries from the start frame to the newest, both included
    if (backfill && g_wrN > 0) {
        // Newest-first: the newest ring frame at or before candStartMs. The newest entry is THIS frame's
        // (ReadWorldGrip ran before B1Frame in this DriverFrame), i.e. g_cur.
        int k = 0;
        for (; k < g_wrN; ++k) {
            if (g_wr[(g_wrHead - k + kWorldRing) % kWorldRing].tMs <= g_candStartMs) { break; }
        }
        if (k == g_wrN) {                 // the candidate began before the whole ring - capped at the ring size
            k = g_wrN - 1;
            g_swBackfillTrunc = 1;
            ++g_backfillTrunc;
        }
        span = k + 1;
        g_swBackfillFrames = span;
    }

    long steps = 0;
    SubStepResult r = kSubStepOk;
    // Oldest pair first. A pair is two ADJACENT ring frames, both ok - never bridged across a stale/missing snapshot.
    for (int k = span - 1; k >= 1 && r == kSubStepOk; --k) {
        const WorldFrame& a = g_wr[(g_wrHead - k + kWorldRing) % kWorldRing];
        const WorldFrame& b = g_wr[(g_wrHead - k + 1 + kWorldRing) % kWorldRing];
        if (!a.ok || !b.ok) { continue; }
        bladeline::Seg sa{}, sb{};
        BlPair(a.seg, a.pos, a.blade, b.seg, b.pos, b.blade, &sa, &sb);
        r = B1SubStep(self, record, hp, sa, sb, a.box, b.box);
        ++steps;
    }
    if (steps == 0) {
        // Not the first run, or no ok pair to backfill: build 2's single sub-step. g_prev is last frame's snapshot
        // (ReadWorldGrip runs every DriverFrame, swing or not), so the quad normally spans a real frame of motion.
        // prev = cur - a degenerate quad - only when last frame's rig snapshot was stale or missing (g_prev.ok
        // false; counted as stalegrip/nogrip). B1Frame guarantees g_cur.ok.
        const GripFrame& pv = g_prev.ok ? g_prev : g_cur;
        bladeline::Seg sa{}, sb{};
        BlPair(pv.seg, pv.pos, pv.blade, g_cur.seg, g_cur.pos, g_cur.blade, &sa, &sb);
        r = B1SubStep(self, record, hp, sa, sb, pv.box, g_cur.box);
        ++steps;
    }
    if (backfill) { g_swBackfillQuads = static_cast<long>(hp.count); }

    if (r != kSubStepOk) {
        // kSubStepBlocked: the original frees and returns 0 (:1190-1194, LABEL_58). kSubStepNoMem: fail closed.
        g_free(hp.quads);
        ++g_buildZero;
        ++g_swBuildZero;
        return 0;
    }
    if (hp.count) {                       // :1200-1205
        g_prep(self, &hp, item, record);
        // Revision 2 (spec §7.6): the search's own time, MEASURED - the box quads grow its triangle tests. COST: two QPC reads
        // per B1Build run.
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        g_search(self, &hp, targets, F(self, 1892));
        QueryPerformanceCounter(&q1);
        const double sMs = static_cast<double>(q1.QuadPart - q0.QuadPart) * g_qpcToMs;
        g_swSearchMs += sMs;
        if (sMs > g_swSearchMaxMs) { g_swSearchMaxMs = sMs; }
        if (sMs > g_winSearchMaxMs) { g_winSearchMaxMs = sMs; }
        // diag_swing_shadow: after the game's own search, before B1's free - reads `targets`, writes nothing B1 uses.
        if (DiagSwingShadow) { ShadowAfterSearch(self, hp, targets, backfill); }
        else if (g_shOpen) { g_shComplete = false; }   // switched off mid-swing: this run was not shadowed -> INCOMPLETE
        // §9.3 the factor restore - AFTER the shadow read the game's own value. The targets vector: { data, count, cap },
        // 64-byte entries, +24 contact, +56 the factor (INV-630). Only the factor is written.
        // [LOADBEARING: melee_hit_factor_restore] COST: one pass over <= 64 entries (a sqrt each) per B1Build with targets.
        if (MeleeHitFactorRestore && g_cur.ok && targets) {
            char* data = *reinterpret_cast<char**>(targets);
            const unsigned count = *reinterpret_cast<const unsigned*>(static_cast<char*>(targets) + 8);
            for (unsigned i = 0; data && i < count && i < 64; ++i) {
                char* en = data + 64 * static_cast<size_t>(i);
                float* f = reinterpret_cast<float*>(en + 56);
                const float before = *f;
                const float after = bladeline::RestoredFactor(before, reinterpret_cast<const float*>(en + 24), g_cur.pos, g_reachM);
                if (after != before) {
                    *f = after;
                    ++g_swFacN;
                    g_swFacBefore += before;
                    g_swFacAfter += after;
                }
            }
        }
    }
    g_free(hp.quads);                     // the widening may have realloc'd it - free what it handed back (F9)
    ++g_buildOk;
    ++g_swBuildOk;
    return 1;
}

// The detour on gamedll+0xDC2000. Exact prototype, so the 5th argument (hand, on the stack) is read correctly and
// calling the trampoline is an ordinary call - no thunk needed.
char __fastcall SwingBuildHook(void* self, void* targets, void* item, float* record, unsigned hand)
{
    if (!(g_b1Active && self == g_b1Self && GetCurrentThreadId() == g_b1Tid)) {
        return g_origBuild(self, targets, item, record, hand);
    }
    return B1Build(self, targets, item, record);
}

// Called only by TryBuilderHook (mode 2, slot 16 already live, not live yet, not refused for good). Install path only,
// on camhook's 1 Hz thread - never per frame.
bool InstallBuilderHook()
{
    ++g_builderTries;
    // Every rva below is the loaded build's (common/buildrva.h: Steam unchanged, the EGL table on the Epic build) - they
    // are CALLED from the builder hook, so none may stay a Steam rva once that hook can install on EGL.
    using ::dlvr::buildrva::Gd;
    g_hit       = reinterpret_cast<HitFn>(g_gd + Gd(kHitRva));
    g_quadInit  = reinterpret_cast<QuadInitFn>(g_gd + Gd(kQuadInitRva));
    g_phys      = reinterpret_cast<PhysFn>(g_gd + Gd(kPhysRva));
    g_geo       = reinterpret_cast<GeoFn>(g_gd + Gd(kGeoRva));
    g_prep      = reinterpret_cast<PrepFn>(g_gd + Gd(kPrepRva));
    g_search    = reinterpret_cast<SearchFn>(g_gd + Gd(kSearchRva));
    g_clear1928 = reinterpret_cast<ClearFn>(g_gd + Gd(kClear1928));
    g_clear1960 = reinterpret_cast<ClearFn>(g_gd + Gd(kClear1960));
    g_realloc   = *reinterpret_cast<ReallocFn*>(g_gd + Gd(kIatRealloc));
    g_free      = *reinterpret_cast<FreeFn*>(g_gd + Gd(kIatFree));
#if !defined(DLVR_BETA_LEAN) || defined(DLVR_SWING_PROBE)   // BETA lean: compiled out; back in the probe variant
    g_dbgByte   = reinterpret_cast<const volatile unsigned char*>(g_gd + Gd(kDebugDrawByteRva));   // diag_swing_shadow only
#endif
    if (!g_realloc || !g_free) {
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] builder REFUSED for the session - gamedll IAT realloc=%p free=%p (RVA 0x%zX / 0x%zX) not "
            "resolved. Mode 2 will REFUSE every hit (nobuilder).",
            reinterpret_cast<void*>(g_realloc), reinterpret_cast<void*>(g_free), static_cast<size_t>(kIatRealloc),
            static_cast<size_t>(kIatFree));
        g_builderRefused = true;
        return false;
    }
    int attempts = 0;
    // [LOADBEARING: B1 sweep builder detour] gamedll+0xDC2000 - melee21_dc2000_prologue_ours.txt (SAFE TO STEAL, 14)
    const bool live = InstallSingleGameDetour(g_gd + kBuildRva, kBuildBytes, sizeof(kBuildBytes),
                                              reinterpret_cast<void*>(&SwingBuildHook),
                                              reinterpret_cast<void**>(&g_origBuild), "swingbuild", &attempts);
    if (!live) {
        // attempts == 0: InstallSingleGameDetour never reached a commit - InstallDetour refused (prologue mismatch,
        // logged just above by boneblend, or its VirtualAlloc failed). Not retried. attempts > 0: the other threads
        // could not be got clear of the range (or VirtualProtect failed) - retried next second, up to the cap.
        if (attempts == 0 || g_builderTries >= kMaxBuilderTries) { g_builderRefused = true; }
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] builder hook gamedll+0x%zX NOT LIVE (try %d of %d, %d commit attempt(s)) - %s. Mode 2 "
            "REFUSES every hit (nobuilder) while it is not live.",
            static_cast<size_t>(kBuildRva), g_builderTries, kMaxBuilderTries, attempts,
            attempts == 0 ? "InstallDetour refused (a prologue mismatch is logged just above by boneblend if that was "
                            "the cause) - REFUSED for the session"
                          : (g_builderRefused ? "commit failed - try cap reached, REFUSED for the session"
                                              : "commit failed - retried next second"));
        return false;
    }
    g_builderLive = true;
    return true;
}

// I1: the ONLY caller of InstallBuilderHook. Mode 2 only, only once slot 16 is live (SwingInstall checks g_slot), and
// never again once live or refused for good. Returns true when this call made it live.
bool TryBuilderHook()
{
    if (MeleeSwingMode != 2 || g_builderLive || g_builderRefused) { return false; }
    return InstallBuilderHook();
}

// The game's own blade axis for comparison (spec §6): the player's R_HandHolder world Y column (F12).
bool GameBladeAxis(void* self, swing::Vec3& out)
{
    if (!g_getWorld || !g_handElem) { return false; }
    void* player = VCall<void*>(self, 32);
    if (!player) { return false; }
    const unsigned elem = g_handElem(self, 0);
    const float* m = g_getWorld(player, static_cast<int>(elem));
    if (!m) { return false; }
    out = swing::Normalised({ m[1], m[5], m[9] });
    return true;
}

// Angle between two directions, degrees; -1 when either is shorter than 1e-6 or non-finite. Cheap (2 sqrt, 1 acos).
float AngleDeg(const swing::Vec3& a, const swing::Vec3& b)
{
    if (!swing::Finite(a) || !swing::Finite(b)) { return -1.0f; }
    const float la = swing::Len(a);
    const float lb = swing::Len(b);
    if (!(la > 1e-6f) || !(lb > 1e-6f)) { return -1.0f; }
    float d = swing::Dot(a, b) / (la * lb);
    d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
    return std::acos(d) * 57.29578f;
}

// The net stroke so far - candidate path start -> p - in the head-yaw frame of the latest valid head. False when either
// end or the head is unknown, or the stroke is shorter than 0.1 mm. Events only (FIRE / END / drop), never per frame.
bool NetStrokeHy(const swing::Vec3& p, swing::Vec3& dirOut, float& lenOut)
{
    lenOut = 0.0f;
    if (!g_candStartOk || !g_lastHeadOk) { return false; }
    const swing::Vec3 d = swing::Sub(p, g_candStartP);
    lenOut = swing::Len(d);
    if (!std::isfinite(lenOut) || !(lenOut > 1e-4f)) { return false; }
    dirOut = swing::Normalised(swing::InHeadYawFrame(g_lastHeadQ[0], g_lastHeadQ[1], g_lastHeadQ[2], g_lastHeadQ[3], d));
    return true;
}

// " | net stroke head-yaw (x y z) over L m | vs previous swing's net A deg | T ms after the previous END" - every field
// NAMES why it is missing rather than going silent. The "previous swing" is the last END's net stroke.
void FormatMotion(char* buf, std::size_t n, const swing::Vec3& p, double nowMs)
{
    swing::Vec3 net{ 0.0f, 0.0f, 0.0f };
    float len = 0.0f;
    const bool haveNet = NetStrokeHy(p, net, len);
    char netText[80];
    if (haveNet) {
        std::snprintf(netText, sizeof(netText), "(%.2f %.2f %.2f) over %.3f m", net.x, net.y, net.z,
                      static_cast<double>(len));
    } else {
        std::snprintf(netText, sizeof(netText), "NOT MEASURED (%s)",
                      !g_candStartOk ? "no start position" : (!g_lastHeadOk ? "no head pose" : "under 0.1 mm"));
    }
    char vsText[64];
    const float vs = (haveNet && g_prevNetOk) ? AngleDeg(net, g_prevNetHy) : -1.0f;
    if (vs >= 0.0f) {
        std::snprintf(vsText, sizeof(vsText), "%.0f deg", static_cast<double>(vs));
    } else {
        std::snprintf(vsText, sizeof(vsText), "n/a (%s)", g_prevNetOk ? "no net stroke" : "no previous swing");
    }
    char sinceText[48];
    if (g_lastEndMs >= 0.0 && std::isfinite(nowMs)) {
        std::snprintf(sinceText, sizeof(sinceText), "%.0f ms", nowMs - g_lastEndMs);
    } else {
        std::snprintf(sinceText, sizeof(sinceText), "n/a (no END yet)");
    }
    std::snprintf(buf, n, " | net stroke head-yaw %s | vs previous swing's net %s | after the previous END %s",
                  netText, vsText, sinceText);
}

void AppendPathSeg(float lenM, double dtMs, long flags)
{
    if (g_pathSegN < kPathSegMax) {
        g_pathSeg[g_pathSegN++] = { lenM, static_cast<float>(dtMs), flags };
    }
    ++g_pathSegTotal;
}

// "[SWING] <what> candidate path | N segment(s) | largest L m = share S of the path, dt D ms, TRACKED/NOT | not
// position-tracked on K of N | segments len mm/dt ms: a/b b/c! ..." ("!" = position NOT tracked, "?" = flags unknown).
// Says so by name when nothing was recorded. Closes the path.
void LogPath(const char* what, long idx)
{
    if (g_pathSegTotal == 0) {
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] %s #%ld candidate path | NO SEGMENTS RECORDED (the candidate had no previous sample)",
            what, idx);
        g_pathOpen = false;
        return;
    }
    float sum = 0.0f;
    int big = 0;
    int notTracked = 0;
    for (int i = 0; i < g_pathSegN; ++i) {
        sum += g_pathSeg[i].lenM;
        if (g_pathSeg[i].lenM > g_pathSeg[big].lenM) { big = i; }
        if (g_pathSeg[i].flags < 0 || !(g_pathSeg[i].flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) { ++notTracked; }
    }
#if defined(DLVR_SWING_PROBE)
    static char segs[8192];   // static: game thread only; 512 segments x ~14 chars
#else
    char segs[400];
#endif
    int off = 0;
    segs[0] = '\0';
    for (int i = 0; i < g_pathSegN && off < static_cast<int>(sizeof(segs)) - 24; ++i) {
        const long fl = g_pathSeg[i].flags;
        const char* mark = fl < 0 ? "?" : ((fl & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "" : "!");
        const int w = std::snprintf(segs + off, sizeof(segs) - static_cast<std::size_t>(off), " %.0f/%.0f%s",
                                    static_cast<double>(g_pathSeg[i].lenM * 1000.0f),
                                    static_cast<double>(g_pathSeg[i].dtMs), mark);
        if (w <= 0) { break; }
        off += w;
    }
    const PathSeg& b = g_pathSeg[big];
    char keptText[48] = {};
    if (g_pathSegTotal > g_pathSegN) { std::snprintf(keptText, sizeof(keptText), " (only the first %d kept)", kPathSegMax); }
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] %s #%ld candidate path | %d segment(s)%s | largest %.3f m = share %.2f of the path, dt %.1f "
        "ms, position %s | not position-tracked on %d of %d | segments len mm/dt ms (! = not tracked, ? = unknown):%s",
        what, idx, g_pathSegTotal, keptText,
        static_cast<double>(b.lenM), sum > 0.0f ? static_cast<double>(b.lenM / sum) : 0.0, static_cast<double>(b.dtMs),
        b.flags < 0 ? "flags UNKNOWN" : ((b.flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "TRACKED" : "NOT TRACKED"),
        notTracked, g_pathSegN, segs);
    g_pathOpen = false;
}

// The game's own attack-end reset (spec §2.4, melee20:273-280 / 323-331): both hit lists cleared, +1924 (hits),
// +2000, +2016, +1625 (durability latch), +808 zeroed. Run by B1Frame at a swing's first B1 frame and (I3) by
// DriverFrame at the END of a swing that made >= 1 B1 call. Callers guarantee the clears resolved and this+0x110 == -1.
void ResetAttackState(void* self)
{
    g_clear1928(static_cast<char*>(self) + 1928);
    g_clear1960(static_cast<char*>(self) + 1960);
    *I(self, 1924) = 0;
    *I(self, 2000) = 0;
    *B(self, 2016) = 0;
    *B(self, 1625) = 0;
    *I(self, 808) = 0;
}

}  // namespace

namespace {

void B1Frame(void* self, const swing::Step& st, int state);   // Task 4 fills this; mode 1 never reaches it

// This frame's grip, in both spaces the feature needs: app space for the detector (stick locomotion and turning do
// not move it), game world for the sweep (the rig's publish - the numbers the drawn arm was solved to, F16).
swing::Sample SampleGrip(XrPosef& gripOut, XrPosef& headOut, bool& gripOk, bool& headOk)
{
    swing::Sample s{};
    s.tMs = NowMs();
    gripOk = GetControllerGripPose(kRightHand, gripOut);
    headOk = GetHeadPose(headOut);
    s.poseOk = gripOk && headOk;
    s.p = { gripOut.position.x, gripOut.position.y, gripOut.position.z };
    // Fix 5 (trace 20261119_1735): the right grip for the shoulder-zone exit rule, every sample. COST: one call, two array reads.
    float squeeze = 0.0f;
    s.gripKnown = GetControllerSqueeze(kRightHand, squeeze) && std::isfinite(squeeze);
    s.gripDown = s.gripKnown && squeeze >= holster::kGripThreshold;
    if (s.poseOk) {
        // Return-stroke rule: "toward the body" in app space (one atan2 + sin + cos). Left zero without a pose.
        s.back = swing::BackInApp(headOut.orientation.x, headOut.orientation.y, headOut.orientation.z,
                                  headOut.orientation.w);
        // The holster zone, evaluated here even with holster_enable=0: a reach over the shoulder is not a swing. Body-locked when
        // the holster body-lock has the shoulder latched (HolsterZoneLocal), else head-relative as before.
        const float hp[3] = { headOut.position.x, headOut.position.y, headOut.position.z };
        const float hq[4] = { headOut.orientation.x, headOut.orientation.y, headOut.orientation.z, headOut.orientation.w };
        const float gp[3] = { gripOut.position.x, gripOut.position.y, gripOut.position.z };
        float lv[3];
        HolsterZoneLocal(kHolsterShoulder, kHolsterCallerSwing, hp, hq, gp, lv);
        const float dx = lv[0] - HolsterCenterX;
        const float dy = lv[1] - HolsterCenterY;
        const float dz = lv[2] - HolsterCenterZ;
        const float d2 = dx * dx + dy * dy + dz * dz;
        // M6: hysteresis, same shape as holster::Machine - enter inside r, leave only past r * kExitScale.
        if (g_inHolster) {
            if (d2 > (HolsterRadius * holster::kExitScale) * (HolsterRadius * holster::kExitScale)) {
                g_inHolster = false;
            }
        } else if (d2 < HolsterRadius * HolsterRadius) {
            g_inHolster = true;
        }
        s.inHolster = g_inHolster;
    } else {
        g_inHolster = false;   // no pose - clear the latch rather than hold a stale zone state
    }
    return s;
}

// Returns the rig snapshot's QPC (0 = none) for the backfill ring.
// ===== THE BLADE-LENGTH HIT LINE (spec docs/superpowers/specs/2026-10-26-blade-length-hit-line-design.md, Jay: "Yes") =====
// Mode 2 only. Per DriverFrame, the drawn weapon's segment: from the hand (or its Holder, whichever is further up the shaft) to
// the mesh's real tip + melee_hit_margin_m (bladeline_table.inc, tools/bladeline_table.py). B1SubStep sweeps it; a frame with no
// row / no model / a failed read keeps today's line (PickPair), and every reason is named once. Game thread only.
// bladeline_table.inc (kBladeRows / kBladeBoxes) is included above B1SubStep, which reads the boxes (revision 2).

void* ItemOf(void* self, void* player);   // defined beside RayLenOf - B1Frame's own "melee item in the hand" check

const void* g_blModel = nullptr;   // the weapon model the cached row belongs to (resolved on a change only)
int         g_blRow = -1;          // its kBladeRows index (-1 = none: today's line)
int         g_blHold = -1;         // its "Holder" element id
long        g_blNoModel = 0, g_blNoRow = 0, g_blRead = 0;   // frames on today's line, by reason (the [SWING STATS] window)
char        g_blSaid[64][64] = {}; // mesh names already logged (one line per distinct mesh)
int         g_blSaidN = 0;
bool        g_blResolve = false;   // re-resolve the row on the next frame (set at FIRE: a freed model can come back at the
                                   // same address with another mesh - final review I3, the shadow probe's own defence)

// The weapon model RayLenOf reaches: player+816 -> its holder object (vfunc +8) -> +4128 + 8 x slot.
const void* BlWeaponModel(void* self, void* player)
{
    void* holder = VCall<void*>(static_cast<char*>(player) + 816, 8);
    return holder ? *reinterpret_cast<void**>(static_cast<char*>(holder) + 8 * (g_slotSel(self) ? 1 : 0) + 4128) : nullptr;
}

void BlSayMesh(const char* name, const char* what)
{
    for (int i = 0; i < g_blSaidN; ++i) { if (std::strcmp(g_blSaid[i], name) == 0) { return; } }
    if (g_blSaidN < 64) { strncpy_s(g_blSaid[g_blSaidN++], name, _TRUNCATE); }
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] blade line: mesh '%s' -> %s", name, what);
}

// This frame's drawn segment into g_cur.seg (after ReadWorldGripInner, which reset it). COST: one compare + one g_getWorld
// + ~40 flops per DriverFrame with a melee item in hand; the name / row lookup and 1 g_getId on a weapon-model change only.
void BlFrame(void* self)
{
    if (MeleeSwingMode != 2 || !g_cur.ok || !g_getId || !g_getWorld || !g_slotSel) { return; }
    void* player = VCall<void*>(self, 32);
    // No item / no model: forget the cached model, so a weapon drawn later at a recycled address is resolved afresh (I3).
    if (!player || !ItemOf(self, player)) { g_blModel = nullptr; return; }
    const void* wm = BlWeaponModel(self, player);
    if (!wm) { g_blModel = nullptr; ++g_blNoModel; return; }
    if (wm != g_blModel || g_blResolve) {
        g_blResolve = false;
        g_blModel = wm;
        char raw[96] = {}, nm[96] = {};
        const bool named = ItemMeshName(reinterpret_cast<uintptr_t>(wm), raw, sizeof(raw)) && twohand::NormMesh(raw, nm, sizeof(nm));
        g_blRow = named ? bladeline::FindRow(nm, kBladeRows, kBladeRowsN) : -1;
        g_blHold = g_getId(wm, "Holder");
        char what[160];
        if (!named) {
            BlSayMesh("(unreadable)", "mesh name UNREADABLE - today's line (controller, game reach)");
        } else if (g_blRow < 0) {
            BlSayMesh(nm, "NO ROW in the blade table - today's line (controller, game reach)");
        } else if (g_blHold < 0) {
            BlSayMesh(nm, "a row but NO \"Holder\" element on the model - today's line (controller, game reach)");
        } else {
            const unsigned nb = kBladeRows[g_blRow].boxN;
            char boxes[48];
            if (nb) { std::snprintf(boxes, sizeof(boxes), ", %u box(es)", nb); }
            else { std::snprintf(boxes, sizeof(boxes), ", NO boxes - line only"); }
            std::snprintf(what, sizeof(what), "row %d, tip %.3f m + margin %.2f m, min reach %.2f m%s", g_blRow,
                          static_cast<double>(kBladeRows[g_blRow].tipM), static_cast<double>(MeleeHitMarginM),
                          static_cast<double>(MeleeMinReachM), boxes);
            BlSayMesh(nm, what);
        }
    }
    if (g_blRow < 0 || g_blHold < 0) { ++g_blNoRow; return; }
    const float* m = g_getWorld(wm, g_blHold);
    float along = 0.0f;
    // [LOADBEARING: melee_min_reach_m] the primary line's END = max(tip + margin, the hand's along + min reach), spec §7.3.
    if (!m || !bladeline::Segment(m, kBladeRows[g_blRow], g_cur.pos, MeleeHitMarginM, MeleeMinReachM, g_cur.seg.s, g_cur.seg.e,
                                  &along)) {
        ++g_blRead;
        return;
    }
    g_cur.seg.blade = true;
    // Revision 2: the Holder 3x4 for this frame's box edges (B1SubStep builds them). COST: one 12-float copy, no game call.
    g_cur.box.ok = true;
    g_cur.box.row = g_blRow;
    for (int i = 0; i < 12; ++i) { g_cur.box.m[i] = m[i]; }
}

long long ReadWorldGripInner()
{
    g_prev = g_cur;
    g_cur = GripFrame{};
    float cWorld[9];
    long long qpc = 0;
    if (!SwingGripRead(g_cur.pos, cWorld, &qpc)) {
        ++g_refNoGrip;
        return 0;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_cur.ageMs = static_cast<float>(static_cast<double>(now.QuadPart - qpc) * g_qpcToMs);
    if (MeleePoseMaxAgeMs > 0.0f && g_cur.ageMs > MeleePoseMaxAgeMs) {
        ++g_refStaleGrip;
        return qpc;
    }
    g_cur.blade = swing::BladeDirWorld(cWorld, MeleeBladePitchDeg);
    g_cur.ok = swing::Finite(g_cur.blade) && swing::Finite({ g_cur.pos[0], g_cur.pos[1], g_cur.pos[2] });
    return qpc;
}

// tMs = this frame's detector sample time. Every call pushes one ring entry (ok or not) - a struct copy.
void ReadWorldGrip(void* self, double tMs)
{
    const long long qpc = ReadWorldGripInner();
    BlFrame(self);   // the blade-length hit line - this frame's drawn segment into g_cur.seg (cost at its definition)
    g_wrHead = (g_wrHead + 1) % kWorldRing;
    WorldFrame& w = g_wr[g_wrHead];
    w.ok = g_cur.ok;
    w.pos[0] = g_cur.pos[0];
    w.pos[1] = g_cur.pos[1];
    w.pos[2] = g_cur.pos[2];
    w.blade = g_cur.blade;
    w.qpc = qpc;
    w.tMs = tMs;
    w.seg = g_cur.seg;
    w.box = g_cur.box;   // revision 2 - the drawn Holder 3x4 + row (a 56-byte copy)
    if (g_wrN < kWorldRing) { ++g_wrN; }
}

void LogFire(void* self, const swing::Step& st, int state, const XrPosef& grip, const XrPosef& head, bool poseOk,
             double nowMs, const swing::Vec3& p)
{
    // The controller's blade axis in the head-YAW frame - "neutral hand points ahead" reads (0, 0, -1).
    swing::Vec3 hf{ 0.0f, 0.0f, 0.0f };
    // Redo 3: the detector's velocity at FIRE, same frame - which way the hand is MOVING, not where the blade points.
    char velText[64];
    std::snprintf(velText, sizeof(velText), "NOT MEASURED (%s)", poseOk ? "no velocity verdict" : "no pose");
    if (poseOk) {
        const swing::Vec3 bXr = swing::Rotate(grip.orientation.x, grip.orientation.y, grip.orientation.z,
                                              grip.orientation.w, swing::BladeLocal(MeleeBladePitchDeg));
        hf = swing::InHeadYawFrame(head.orientation.x, head.orientation.y, head.orientation.z,
                                   head.orientation.w, bXr);
        if (st.speedValid && swing::Len(st.vel) > 1e-6f) {
            const swing::Vec3 vHy = swing::Normalised(swing::InHeadYawFrame(
                head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w, st.vel));
            std::snprintf(velText, sizeof(velText), "(%.2f %.2f %.2f)", vHy.x, vHy.y, vHy.z);
        }
    }
    char motion[256];
    FormatMotion(motion, sizeof(motion), p, nowMs);
    swing::Vec3 game{ 0.0f, 0.0f, 0.0f };
    const bool haveGame = GameBladeAxis(self, game);
    float angDeg = -1.0f;
    if (haveGame && g_cur.ok) {
        float d = swing::Dot(game, g_cur.blade);
        d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
        angDeg = std::acos(d) * 57.29578f;
    }
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] FIRE #%ld | speed %.2f m/s over an actual window of %.1f ms across %d samples (configured "
        "minimum %.0f ms) | travel %.2f m at fire (candidate %.0f ms before; min travel %.2f m) | state(this+0x110) "
        "%d | blade head-yaw frame "
        "(%.2f %.2f %.2f) %s | blade world (%.3f %.3f %.3f) %s | game hand-holder Y (%.3f %.3f %.3f) %s, angle "
        "%.1f deg (-1 = not measurable) | grip snapshot age %.1f ms | mode %d | hand velocity head-yaw %s%s",
        g_totFires, st.speed, static_cast<double>(st.spanMs), st.spanSamples, static_cast<double>(MeleeSwingWindowMs),
        static_cast<double>(st.travelM), nowMs - st.candStartMs, static_cast<double>(MeleeSwingMinTravelM),
        state, hf.x, hf.y, hf.z,
        poseOk ? "" : "(NO POSE)", g_cur.blade.x, g_cur.blade.y, g_cur.blade.z,
        g_cur.ok ? "" : "(NO FRESH RIG SNAPSHOT - see stalegrip/nogrip)", game.x, game.y, game.z,
        haveGame ? "" : "(NOT RESOLVED)", angDeg, g_cur.ageMs, MeleeSwingMode, velText, motion);
}

// Fix round 1 (Minor 8): one line per DROPPED candidate - how close the rebound came to melee_swing_min_travel_m.
// Budget 120 per session; past it the drops are still counted (candidates dropped) and the unlogged ones are counted
// as "drop lines suppressed" in the 1 s STATS line. Game thread (DriverFrame / SwingUpdateThunk's relatch).
long g_totDrops = 0;   // session count of dropped candidates - numbers the DROP path lines

void LogDrop(const swing::Step& st)
{
    ++g_candDropped;
    ++g_totDrops;
    if (st.dropWhy == swing::Drop::Return || st.dropWhy == swing::Drop::TowardBody) { ++g_candReturn; }
    if (g_dropBudget <= 0) {
        ++g_dropLinesSuppressed;
        g_pathOpen = false;
        return;
    }
    --g_dropBudget;
    // Redo 3: the net stroke to the LAST good sample (g_lastP - on a pose-loss drop that is the one before the loss),
    // timed at the drop (candStartMs + durMs; durMs is -1 when the drop sample's time was not finite).
    char motion[256];
    FormatMotion(motion, sizeof(motion), g_lastP,
                 st.durMs >= 0.0f ? st.candStartMs + static_cast<double>(st.durMs) : std::nan(""));
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] candidate dropped (%s) | travel %.3f m of %.2f | peak %.2f m/s | %.0f ms%s",
        swing::DropName(st.dropWhy), static_cast<double>(st.travelM), static_cast<double>(MeleeSwingMinTravelM),
        static_cast<double>(st.peak), static_cast<double>(st.durMs), motion);
    LogPath("DROP", g_totDrops);   // same budget as the drop line above
}

// anim_suppress_melee_idle (INV-319): keep the melee ready state on "Stand". Game thread, inside the driver gate, after the
// game's melee update. Cost with the setting on: one 4-byte load, one int load; writes only when the game set something.
// Offsets are OUR gamedll's (97046bba, idle5_ours.c / idle6_ours.c): availability bytes +1561..+1564, variant +304.
long g_idleSupBytes = 0;      // frames on which at least one availability byte had to be cleared
long g_idleSupCatch[6] = {};  // caught variants by value (index 5 = anything outside 1..4)
bool g_idleSupFirstSaid = false;

void MeleeIdleSuppressFrame(void* self)
{
    if (!AnimSuppressMeleeIdle || !self) { return; }
    char* const c = static_cast<char*>(self);
    volatile unsigned char* const av = reinterpret_cast<volatile unsigned char*>(c + 1561);
    volatile int* const var = reinterpret_cast<volatile int*>(c + 304);
    const int a0 = av[0], a1 = av[1], a2 = av[2], a3 = av[3];
    const int v = *var;
    if (!g_idleSupFirstSaid) {
        g_idleSupFirstSaid = true;
        // [PROBE: IDLESUP] gate=anim_suppress_melee_idle  site=HEADSET  log-only=no
        LOG("swing: [IDLESUP] FIRST FRAME (logged unconditionally) | availability Exhausted %d Vigilent %d VigilentLeft %d "
            "Trick %d | variant +304 = %d (0 Stand, 1 Exhausted_Idle, 2 Vigilent, 3 VigilentLeft, 4 Trick) | +0x110 %d",
            a0, a1, a2, a3, v, *reinterpret_cast<const int*>(c + 0x110));
    }
    if (a0 | a1 | a2 | a3) {
        av[0] = 0;
        av[1] = 0;
        av[2] = 0;
        av[3] = 0;
        ++g_idleSupBytes;
        // [PROBE: IDLESUP] gate=anim_suppress_melee_idle  site=HEADSET  log-only=no
        LOG("swing: [IDLESUP] CLEARED availability (set by the game on a weapon-type change) | Exhausted %d Vigilent %d "
            "VigilentLeft %d Trick %d | clears so far %ld",
            a0, a1, a2, a3, g_idleSupBytes);
    }
    if (v != 0) {
        *var = 0;
        const int k = (v >= 1 && v <= 4) ? v : 5;
        ++g_idleSupCatch[k];
        // [PROBE: IDLESUP] gate=anim_suppress_melee_idle  site=HEADSET  log-only=no
        LOG("swing: [IDLESUP] CAUGHT idle variant %d (%s) - put back to 0 (Stand) | caught so far: Exhausted %ld Vigilent %ld "
            "VigilentLeft %ld Trick %ld other %ld",
            v, v == 1 ? "Exhausted_Idle" : v == 2 ? "Vigilent" : v == 3 ? "VigilentLeft" : v == 4 ? "Trick" : "UNKNOWN value",
            g_idleSupCatch[1], g_idleSupCatch[2], g_idleSupCatch[3], g_idleSupCatch[4], g_idleSupCatch[5]);
    }
}

}  // namespace

// 1 Hz from camhook's install loop, unconditionally: say ONCE per change when the setting is on but cannot act.
void MeleeIdleSuppressTick()
{
    static int s_said = -1;
    const int refused = (AnimSuppressMeleeIdle && MeleeSwingMode == 0) ? 1 : 0;
    if (refused != s_said) {
        s_said = refused;
        if (refused) {
            // [PROBE: IDLESUP] gate=anim_suppress_melee_idle  site=HEADSET  log-only=no
            LOG("swing: [IDLESUP] REFUSED - anim_suppress_melee_idle=1 but melee_swing_mode=0: the clear runs in the swing "
                "driver's slot-16 hook, which is not installed. Melee idle animations are NOT suppressed.");
        }
    }
}

namespace {

void DriverFrame(void* self)
{
    ++g_calls;
    MeleeIdleSuppressFrame(self);
    const int state = *reinterpret_cast<const int*>(static_cast<const char*>(self) + 0x110);
    g_primaryState = state;
    if (state == -1) { ++g_idle; } else { ++g_notIdleFrames; }

    XrPosef grip{};
    XrPosef head{};
    bool gripOk = false;
    bool headOk = false;
    const swing::Sample s = SampleGrip(grip, head, gripOk, headOk);
    ReadWorldGrip(self, s.tMs);
    ShadowReadFrame(self, s.tMs);   // diag_swing_shadow - the drawn weapon into the ring beside g_wr (cost at its definition)
    ShadowTick();

    // C1: this frame's controller blade from the XR grip quaternion - APP space for U-d, and the head-yaw frame
    // (exactly as LogFire computes it) for [SWING STATS]. Per frame: one Rotate + one yaw extraction, nothing else.
    swing::Vec3 bladeXrApp{ 0.0f, 0.0f, 0.0f };
    g_bladeXrOk = s.poseOk;
    g_bladeXrHy = { 0.0f, 0.0f, 0.0f };
    if (s.poseOk) {
        bladeXrApp = swing::Rotate(grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w,
                                   swing::BladeLocal(MeleeBladePitchDeg));
        g_bladeXrHy = swing::InHeadYawFrame(head.orientation.x, head.orientation.y, head.orientation.z,
                                            head.orientation.w, bladeXrApp);
    }

    const swing::Params p{ MeleeSwingArmSpeed, MeleeSwingFireSpeed, MeleeSwingEndSpeed, MeleeSwingWindowMs,
                           MeleeSwingMaxMs, MeleeSwingMinTravelM, MeleeSwingReturnMs,
                           std::cos(MeleeSwingReturnDeg * 0.017453292519943f), MeleeSwingRefuseTowardBody,
                           MeleeSwingReturnFastMps, std::cos(MeleeSwingReversalDeg * 0.017453292519943f),
                           MeleeSwingReversalMs };
    const swing::Vec3 prevP = g_lastP;   // redo 3: the detector's prevP_ BEFORE this sample - a candidate's path start
    const bool prevPOk = g_lastPOk;
    const double prevT = g_lastT;
    const swing::Step st = g_det.Update(s, p);
    g_shSpeed = st.speed;   // diag_swing_shadow's F rows (review I6) - two stores per frame, read only by the probe
    g_shTMs = s.tMs;
    if (st.repeat) { ++g_repeats; } else { ++g_samples; }
    if (!s.poseOk) { ++g_refNoPose; }
    if (st.nonFinite) { ++g_refNonFinite; }
    if (st.holsterRefused) { ++g_refHolster; }
    // Fix 5: one line per shoulder-zone exit (a reach, not a per-frame event), 300 per session, then one STOPPED line.
    if (st.zoneExit != swing::ZoneExit::None) {
        static int s_zoneExitLines = 0;
        static long s_zoneExitArmed = 0;
        static long s_zoneExitIdle = 0;
        if (st.zoneExit == swing::ZoneExit::Armed) { ++s_zoneExitArmed; } else { ++s_zoneExitIdle; }
        if (s_zoneExitLines < 300) {
            ++s_zoneExitLines;
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] shoulder zone exit -> %s | grip %s | session: armed %ld idle %ld",
                swing::ZoneExitName(st.zoneExit), s.gripKnown ? (s.gripDown ? "down" : "up") : "UNKNOWN",
                s_zoneExitArmed, s_zoneExitIdle);
        } else if (s_zoneExitLines == 300) {
            ++s_zoneExitLines;
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] shoulder zone exit lines STOPPED at 300 (exits still change the state; armed %ld idle %ld)",
                s_zoneExitArmed, s_zoneExitIdle);
        }
    }
    // Redo 3 motion tracking - a few float copies per frame, no calls. Mirrors the detector: a pose loss or a
    // non-finite sample clears its history (Clear()), a repeat leaves it, anything else becomes the new prevP_.
    if (!s.poseOk || st.nonFinite) {
        g_lastPOk = false;
    } else if (!st.repeat) {
        g_lastP = s.p;
        g_lastPOk = true;
        g_lastT = s.tMs;
    }
    // Redo 4 raise instrument. The flags are read once per non-repeat pose-valid sample (a 32-bit load) for the STATS
    // tally; the path records only while a candidate is open - its first segment is the detector's (prevP_ -> here).
    if (s.poseOk && !st.nonFinite && !st.repeat) {
        const long gflags = ControllerGripFlags(kRightHand);
        if (gflags < 0) {
            ++g_flagsUnknown;
        } else if (!(gflags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) {
            ++g_notTracked;
        }
        if (st.candStarted) {
            g_pathSegN = 0;
            g_pathSegTotal = 0;
            g_pathOpen = true;
            if (prevPOk) { AppendPathSeg(swing::Len(swing::Sub(s.p, prevP)), s.tMs - prevT, gflags); }
        } else if (g_pathOpen && prevPOk) {
            AppendPathSeg(swing::Len(swing::Sub(s.p, prevP)), s.tMs - prevT, gflags);
        }
    }
    if (s.poseOk) {
        g_lastHeadQ[0] = head.orientation.x;
        g_lastHeadQ[1] = head.orientation.y;
        g_lastHeadQ[2] = head.orientation.z;
        g_lastHeadQ[3] = head.orientation.w;
        g_lastHeadOk = true;
    }
    if (st.candStarted) {   // the detector's path starts at the previous non-repeat sample, or here with no history
        g_candStartP = prevPOk ? prevP : s.p;
        g_candStartOk = true;
    }
    if (st.dropped) {
        LogDrop(st);   // counts g_candDropped
        g_candStartOk = false;
    }

    if (st.candStarted) {
        // U-d counts the candidate's frames (the same stroke), so its accumulator starts HERE, not at FIRE.
        g_udN = 0;
        g_udSum = 0.0;
        g_udMax = 0.0f;
    }
    if (st.fired) {
        ++g_fires;
        ++g_totFires;
        // Minor: in EVERY mode, so a 1 -> 2 switch mid-swing never reuses the previous swing's reach / lists.
        g_swingReady = false;
        // I4: per-swing counters start at FIRE (U-d started at the candidate, above).
        g_swB1Calls = g_swBuildOk = g_swBuildZero = g_swSamples = 0;
        g_swRefusal = "none";
        g_swBlocked = 0;
        ShadowFireStart();   // diag_swing_shadow - opens the swing only while the probe is on
        g_swBlSteps = g_swFbSteps = 0;   // the blade-length hit line's per-swing sub-step counts (the END line)
        g_swBoxQuads = 0;                // revision 2: box quads and the search's time, per swing (the END line)
        g_swCapQuads = 0;                // §9: capsule quads, restored factors, per swing (the END line)
        g_swFacN = 0;
        g_swFacBefore = g_swFacAfter = 0.0;
        g_swSearchMs = g_swSearchMaxMs = 0.0;
        g_blResolve = true;              // and its row re-resolved next frame (one name read per swing - final review I3)
        // The backfill: B1Build's first run this swing sweeps from the candidate start (mode 2 only reads it).
        g_candStartMs = st.candStartMs;
        g_backfillPending = true;
        g_swBackfillQuads = -1;
        g_swBackfillFrames = 0;
        g_swBackfillTrunc = 0;
        LogFire(self, st, state, grip, head, s.poseOk, s.tMs, g_lastP);
        LogPath("FIRE", g_totFires);   // redo 4: the candidate's per-sample path, then closed
    }
    if ((st.state == swing::State::Active || st.ended) && !st.repeat) { ++g_swSamples; }

    // C1(2) U-d, no axis convention needed: the blade-to-velocity angle is frame-independent, so it must agree between
    // XR app space and game world if cWorld follows the same mapping as the positions. Candidate and Active frames
    // with a detector velocity, both XR poses, two consecutive fresh rig snapshots and > 1 mm of world displacement.
    if ((st.state == swing::State::Active || st.state == swing::State::Candidate) && st.speedValid && s.poseOk &&
        g_cur.ok && g_prev.ok) {
        const swing::Vec3 dW{ g_cur.pos[0] - g_prev.pos[0], g_cur.pos[1] - g_prev.pos[1],
                              g_cur.pos[2] - g_prev.pos[2] };
        if (swing::Len(dW) > 0.001f) {
            const float aXr = AngleDeg(bladeXrApp, st.vel);
            const float aW = AngleDeg(g_cur.blade, dW);
            if (aXr >= 0.0f && aW >= 0.0f) {
                const float dd = std::fabs(aXr - aW);
                ++g_udN;
                g_udSum += dd;
                if (dd > g_udMax) { g_udMax = dd; }
            }
        }
    }

    // Melee throw (spec 2026-09-30-melee-throw-design §3a): after the detector (it reads this frame's velocity), before
    // B1 - a READYING / READY / just-thrown frame never calls the hit. throw_enable=0 -> false at its first line.
    const float throwGrip[3] = { grip.position.x, grip.position.y, grip.position.z };
    const float throwHeadP[3] = { head.position.x, head.position.y, head.position.z };
    const float throwHeadQ[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
    const bool throwMute = ThrowFrame(self, throwGrip, throwHeadP, throwHeadQ, s.poseOk, st.speedValid, st.vel.x,
                                      st.vel.y, st.vel.z, s.tMs);
    // Ruling R24: while a throwable is readied / in flight / being cancelled the VR swing's hit is not called - one gesture
    // must not throw a grenade AND land a melee hit. COST: one volatile load.
    const bool throwableBusy = MeleeSwingMode == 2 && ThrowableBusy();
    {
        static long s_nSup = 0;
        static bool s_swingSaid = false;   // one line per suppressed swing (its FIRE, or the first frame of one in flight)
        if (throwableBusy && !s_swingSaid && (st.fired || st.state == swing::State::Active)) {
            s_swingSaid = true;
            ++s_nSup;
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=no
            LOG("swing: [SWING] SUPPRESSED #%ld - a throwable is readied / in flight (ThrowableBusy, ruling R24): %s - "
                "the hit (B1) is not called",
                s_nSup, st.fired ? "this FIRE" : "the swing already in progress");
        }
        if (st.ended || st.fired) {
            if (!(throwableBusy && st.fired)) { s_swingSaid = false; }   // re-arm per swing
        }
    }
    // Body grab (plan 2026-10-23-body-grab Task 4): the right hand holds a body - its weapon is hidden, so the swing's hit is not
    // called (a fast drag reads as a swing). The detector and its FIRE / END lines run as before. COST: one volatile load.
    // Physical breakout (plan 2026-10-24-physical-breakout, Build constants (e)): no hit while a zombie holds the player - a
    // two-handed shove reads as a swing - but only from 500 ms into the grab (breakout_logic.h MeleeGateClosed; Jay 2026-10-27 / 11-16:
    // an attack as the zombie grabs should count, "change the timing on the gate"). breakout_physical=0 -> always false.
    // COST: one volatile load + GetTickCount64.
    const bool grabbing = BodyGrabHandActive(1) || BreakoutMeleeGateClosed();
    // A swing under way while either grab was on keeps its hit off to its END (swing_logic.h HoldLatch; Jay 2026-10-27: a thrown
    // body dismembered on release, "gate the grab too") - the throw / shove still moving after the release is not a new swing.
    // COST: a few compares.
    static swing::HoldLatch s_hold;
    static bool s_holdSaid = false;   // one line per swing kept off past its release
    static long s_nHoldOff = 0;
    const bool holdAllows = swing::HoldLatchStep(s_hold, grabbing, st);
    if (!grabbing && !holdAllows && st.state == swing::State::Active && !s_holdSaid) {
        s_holdSaid = true;
        ++s_nHoldOff;
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=no
        LOG("swing: [SWING] HELD-SWING OFF #%ld - swing #%ld was under way while a body grab (right hand) or a zombie grab past "
            "its first 500 ms was on, and is still running after it: no hit (B1) until its END", s_nHoldOff, g_totFires);
    }
    if (st.ended || holdAllows) { s_holdSaid = false; }
    if (MeleeSwingMode == 2 && !throwMute && !throwableBusy && holdAllows) {   // g_disabled is already refused by SwingUpdateThunk
        B1Frame(self, st, state);
    }
    if (st.ended) {
        ++g_ends;
        // I4: hits only when THIS swing called the hit - otherwise +1924 belongs to someone else (a native attack).
        const bool madeB1 = g_swB1Calls > 0;
        char hitsText[64];
        if (madeB1) {
            std::snprintf(hitsText, sizeof(hitsText), "hits %d (this+1924)", *I(self, 1924));
        } else {
            std::snprintf(hitsText, sizeof(hitsText), "hits n/a (no B1 call this swing)");
        }
        // I3: the game's own attack-end reset, AFTER +1924 was read above - only for a swing that called the hit, only
        // on the primary self, and only while this+0x110 == -1 NOW (never touch a native attack's state mid-flight).
        const char* endReset = "not needed (no B1 call this swing)";
        if (madeB1) {
            const int stateNow = *reinterpret_cast<const int*>(static_cast<const char*>(self) + 0x110);
            if (self != g_primarySelf) {
                endReset = "SKIPPED - not the primary WeaponMeleeController";
            } else if (stateNow != -1) {
                endReset = "SKIPPED - this+0x110 != -1 (a native attack is in flight, left untouched)";
            } else if (!g_clear1928 || !g_clear1960) {
                endReset = "SKIPPED - the list clears are not resolved";
            } else {
                ResetAttackState(self);
                g_swingReady = false;
                endReset = "ran (hit lists cleared, +1924 +2000 +2016 +1625 +808 zeroed)";
            }
        }
        // Rebound suppression: travel = the swing's TOTAL path from the candidate start (the detector's). backfill =
        // the quads B1Build's first run emitted and the ring frames it spanned (-1 = no B1Build run this swing).
        // Redo 3: the whole stroke's net direction (candidate start -> the last good sample) against the previous
        // swing's, then this one becomes "the previous swing". Net length vs travel = how curved the stroke was.
        char motion[256];
        FormatMotion(motion, sizeof(motion), g_lastP, s.tMs);
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] END #%ld reason=%s | peak %.2f m/s | %.0f ms | travel %.2f m | mode %d | B1 calls %ld "
            "builder ret1 %ld ret0 %ld blocked %d | backfill %ld quad(s) over %ld ring frame(s)%s (-1 = no builder "
            "run) | samples %ld | %s | last B1 refusal %s | attack-end reset %s | line blade %ld / today's %ld sub-step(s) "
            "(frames on today's line this session: no model %ld, no row %ld, read %ld) | box quads %ld | capsule quads %ld | "
            "factor restored %ld (mean %.3f -> %.3f) | search %.2f ms (max %.2f ms per call) | max turn %.0f deg over the "
            "%.0f ms look-back (reversal above %.0f deg; -1 = not measured)%s",
            g_totFires, swing::EndName(st.reason), st.peak, st.durMs, static_cast<double>(st.travelM),
            MeleeSwingMode, g_swB1Calls, g_swBuildOk, g_swBuildZero, g_swBlocked, g_swBackfillQuads,
            g_swBackfillFrames, g_swBackfillTrunc ? " TRUNCATED at the ring size" : "", g_swSamples, hitsText,
            g_swRefusal, endReset, g_swBlSteps, g_swFbSteps, g_blNoModel, g_blNoRow, g_blRead, g_swBoxQuads, g_swCapQuads,
            g_swFacN, g_swFacN ? g_swFacBefore / static_cast<double>(g_swFacN) : -1.0,
            g_swFacN ? g_swFacAfter / static_cast<double>(g_swFacN) : -1.0, g_swSearchMs, g_swSearchMaxMs,
            static_cast<double>(st.maxTurnDeg), static_cast<double>(MeleeSwingReversalMs),
            static_cast<double>(MeleeSwingReversalDeg), motion);
        {
            swing::Vec3 net{ 0.0f, 0.0f, 0.0f };
            float netLen = 0.0f;
            g_prevNetOk = NetStrokeHy(g_lastP, net, netLen);
            if (g_prevNetOk) { g_prevNetHy = net; }
            if (std::isfinite(s.tMs)) { g_lastEndMs = s.tMs; }
            g_candStartOk = false;
        }
        if (g_udN > 0) {
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] END #%ld U-d blade/velocity angle agreement: n %d mean |dXR-dWorld| %.1f deg max "
                "%.1f deg (small = cWorld consistent with the position mapping; large = convention mismatch)",
                g_totFires, g_udN, g_udSum / static_cast<double>(g_udN), static_cast<double>(g_udMax));
        } else {
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] END #%ld U-d blade/velocity angle agreement: NOT MEASURED (no valid frames)",
                g_totFires);
        }
        ShadowEndSwing(madeB1);   // diag_swing_shadow - the SUMMARY (only for a swing it opened)
    }
    if (DiagMeleeSwingProbe && st.state == swing::State::Active && g_detailBudget > 0) {
        --g_detailBudget;
        // [PROBE: SWING] gate=diag_melee_swing_probe  site=HEADSET  log-only=yes
        LOG("swing: [SWING] frame | speed %.2f valid %d | state %d | grip world (%.3f %.3f %.3f) ok %d | blade "
            "(%.3f %.3f %.3f) | %ld detail line(s) left",
            st.speed, st.speedValid ? 1 : 0, state, g_cur.pos[0], g_cur.pos[1], g_cur.pos[2], g_cur.ok ? 1 : 0,
            g_cur.blade.x, g_cur.blade.y, g_cur.blade.z, g_detailBudget);
    }

    const ULONGLONG nowTick = GetTickCount64();
    if (g_statsStart == 0) { g_statsStart = nowTick; }
    if (nowTick - g_statsStart >= 1000) {
        const LONG selvesRaw  = g_seenSelvesCount;    // one read each - another thread may still be incrementing
        const LONG threadsRaw = g_seenThreadsCount;
        const LONG otherRaw   = g_otherSelfCalls;
        const LONG remoteRaw  = g_remoteSelfCalls;
        const long overlapTot = g_gate.Overlaps();   // Gate: frames skipped because another thread held the gate
        // C1(1): the game's hand-holder Y against the rig's world blade - a vcall + GetElementWorldMatrix, so at most
        // ONCE per stats window, here, on the game thread. -1 = not measurable (no fresh rig snapshot or unresolved).
        float hhAngDeg = -1.0f;
        swing::Vec3 hhY{ 0.0f, 0.0f, 0.0f };
        if (g_cur.ok && GameBladeAxis(self, hhY)) { hhAngDeg = AngleDeg(hhY, g_cur.blade); }
        // [PROBE: SWING STATS] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG_PERIODIC("swing: [SWING STATS] 1 s | update calls %ld (idle %ld, in an attack state %ld) | samples %ld repeats "
            "%ld | refused: holster %ld nopose %ld nonfinite %ld stalegrip %ld nogrip %ld notidle %ld nowindow "
            "%ld multiself %ld noexport %ld noitem %ld nobuilder %ld | fires %ld ends %ld candidates dropped %ld "
            "(drop lines suppressed %ld) | "
            "B1 calls %ld builder ret1 %ld ret0 %ld hit early-outs %ld backfill truncated %ld | faults %ld | selves "
            "%d%s (other-self calls %ld, remote-player calls %ld excluded) | threads %d%s | overlap skipped %ld (session %ld) | "
            "blade XR head-yaw (%.2f %.2f %.2f)%s | blade world (%.3f %.3f %.3f) %s | hand-holder Y angle %.1f deg "
            "(-1 = not measurable) | return/toward-body refused %ld | grip position NOT tracked on %ld sample(s), flags unknown "
            "on %ld | B1 search max %.2f ms per call",
            g_calls, g_idle, g_notIdleFrames, g_samples, g_repeats, g_refHolster, g_refNoPose, g_refNonFinite,
            g_refStaleGrip, g_refNoGrip, g_refNotIdle, g_refNoWindow, g_refMultiSelf, g_refNoExport, g_refNoItem,
            g_refNoBuilder, g_fires, g_ends, g_candDropped, g_dropLinesSuppressed, g_b1Calls, g_buildOk, g_buildZero,
            g_b1Calls - (g_buildOk + g_buildZero), g_backfillTrunc, g_faults,
            SeenCount(selvesRaw), SeenSat(selvesRaw) ? "+" : "", static_cast<long>(otherRaw),
            static_cast<long>(remoteRaw),
            SeenCount(threadsRaw), SeenSat(threadsRaw) ? "+" : "", overlapTot - g_overlapAtStats, overlapTot,
            g_bladeXrHy.x, g_bladeXrHy.y, g_bladeXrHy.z, g_bladeXrOk ? "" : " (NO POSE)",
            g_cur.blade.x, g_cur.blade.y, g_cur.blade.z,
            g_cur.ok ? "" : "(NO FRESH RIG SNAPSHOT - see stalegrip/nogrip)", hhAngDeg, g_candReturn, g_notTracked,
            g_flagsUnknown, g_winSearchMaxMs);
        g_winSearchMaxMs = 0.0;
        // R1: B1Frame refuses while this is > 1. A saturated count clamps to kMaxTrack, still > 1.
        g_lastWindowSelves = SeenCount(selvesRaw);
        g_calls = g_idle = g_notIdleFrames = g_samples = g_repeats = 0;
        g_refHolster = g_refNoPose = g_refNonFinite = g_refStaleGrip = g_refNoGrip = 0;
        g_refNotIdle = g_refNoItem = g_refNoBuilder = g_fires = g_ends = g_faults = 0;
        g_refMultiSelf = g_refNoWindow = g_refNoExport = 0;
        g_b1Calls = g_buildOk = g_buildZero = 0;
        g_candDropped = g_backfillTrunc = g_dropLinesSuppressed = 0;
        g_candReturn = g_notTracked = g_flagsUnknown = 0;
        InterlockedExchange(&g_otherSelfCalls, 0);
        InterlockedExchange(&g_remoteSelfCalls, 0);
        g_overlapAtStats = overlapTot;
        InterlockedExchange(&g_seenSelvesCount, 0);
        InterlockedExchange(&g_seenThreadsCount, 0);
        g_statsStart = nowTick;
        g_lastStatsEmit = nowTick;   // F2: SwingStatsTick's silence baseline
    }
}

// Per swing, at FIRE: resolve the item, reach and ray length (the original computes them per call; the weapon cannot
// change mid-swing), and run the game's own attack-end reset (spec §2.4, F13-F15). Per active frame: zero the stale
// hit-direction override (F5) and call the whole hit. Only while the controller is IDLE (this+0x110 == -1).
// (g_swingReady is declared with the per-swing state above; DriverFrame clears it on every FIRE.)

// R3: the primary self was re-latched - drop any swing in progress, and force the per-swing reset to run again on
// the new self. A plain function so SwingUpdateThunk's __try body builds no temporary.
// Fix round 1 (Minor 8): the relatch's pending-candidate drop, logged like any other. A plain function for the same
// reason as ResetSwingState (no Step temporary in SwingUpdateThunk's __try body).
void AbandonCandidate()
{
    const swing::Step ab = g_det.Abandon(NowMs());
    if (ab.dropped) { LogDrop(ab); }
}

void ResetSwingState()
{
    g_det = swing::Detector{};
    g_swingReady = false;
    g_lastPOk = false;       // redo 3: the fresh detector has no prevP_ either
    g_candStartOk = false;
    g_pathOpen = false;      // redo 4: no candidate, no path
}

void* ItemOf(void* self, void* player)
{
    void* base = VCall<void*>(player, 1944, static_cast<unsigned>(g_slotSel(self) ? 1 : 0));
    return base ? static_cast<char*>(base) + 88 : nullptr;
}

// 1.2 x sub_180DC2000's v31 (melee3:805-863): the length the world-geometry check casts (sub_180DC84B0's xmm2).
float RayLenOf(void* self, void* player)
{
    const unsigned elem = g_handElem(self, 0);
    const float* hh = g_getWorld(player, static_cast<int>(elem));
    float v[3] = { hh[1], hh[5], hh[9] };                     // no DTrailStart1: the hand holder's Y column
    if (g_classGate(self)) {
        float tmp[4] = {};
        const float* d = VCall<const float*>(player, 1864, static_cast<void*>(tmp));
        v[0] = d[0]; v[1] = d[1]; v[2] = d[2];
    } else {
        void* holder = VCall<void*>(static_cast<char*>(player) + 816, 8);
        void* wmodel = holder ? *reinterpret_cast<void**>(static_cast<char*>(holder) + 8 * (g_slotSel(self) ? 1 : 0) + 4128)
                              : nullptr;
        const int trail = (wmodel && g_getId) ? g_getId(wmodel, "DTrailStart1") : -1;
        if (trail != -1) {
            const float* tm = g_getWorld(wmodel, trail);
            v[0] = tm[3] - hh[3]; v[1] = tm[7] - hh[7]; v[2] = tm[11] - hh[11];
        }
    }
    return 1.2f * std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void B1Frame(void* self, const swing::Step& st, int state)
{
    if (st.state != swing::State::Active) { return; }   // g_swingReady was cleared on FIRE by DriverFrame
    if (state != -1) { ++g_refNotIdle; g_swRefusal = "notidle"; return; }   // a native R2 attack is in flight - never touch it
    // R1 + m4: B1 calls only when the last STATS window saw EXACTLY one WeaponMeleeController - fail CLOSED both ways.
    if (g_lastWindowSelves == 0) { ++g_refNoWindow; g_swRefusal = "nowindow"; return; }   // no window completed yet: unknown is not "one"
    if (g_lastWindowSelves > 1) { ++g_refMultiSelf; g_swRefusal = "multiself"; return; }  // > 1 melee controller seen
    if (!g_builderLive) { ++g_refNoBuilder; g_swRefusal = "nobuilder"; return; }
    // m1: RayLenOf dereferences these; an unresolved engine export is a named refusal, not a fault.
    if (!g_getWorld || !g_getId || !g_handElem) { ++g_refNoExport; g_swRefusal = "noexport"; return; }
    if (!g_cur.ok) { g_swRefusal = "nogrip/stalegrip"; return; }   // counted as stalegrip/nogrip in ReadWorldGrip
    void* player = VCall<void*>(self, 32);
    void* item = player ? ItemOf(self, player) : nullptr;
    if (!item) { ++g_refNoItem; g_swRefusal = "noitem"; return; }

    if (!g_swingReady) {
        g_reachM = g_reach(self, item);
        g_rayLen = RayLenOf(self, player);
        ResetAttackState(self);   // g_builderLive (checked above) means the clears resolved
        g_swingReady = true;
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] B1 swing #%ld armed in the game | reach %.3f m (sub_180DBEC70) | geometry ray %.3f m | "
            "hit lists cleared, +1924 +2000 +2016 +1625 +808 zeroed",
            g_totFires, g_reachM, g_rayLen);
    }
    F(self, 0x29C)[0] = 0.0f;                                // F5: never inherit an attack-type hit direction
    F(self, 0x29C)[1] = 0.0f;
    F(self, 0x29C)[2] = 0.0f;
    g_b1Tid = GetCurrentThreadId();
    g_b1Self = self;
    g_b1Active = true;
    g_hit(self, static_cast<char*>(self) + 0x6F0, 0);
    g_b1Active = false;
    g_b1Self = nullptr;
    ++g_b1Calls;
    ++g_swB1Calls;
    if (*B(self, 772)) {                                     // one wall strike ends this swing (F1 clears the game's flag)
        g_det.Block();
        g_swBlocked = 1;
    }
}

// Gate: a refused entry. The first one is logged with both threads (the holder's id/self are written right after it
// entered, so they are the holder's or, at worst, the previous holder's - said in the line); every one is counted by
// swing::Gate itself. A plain function so SwingUpdateThunk's __try body builds no temporary.
void NoteOverlap(void* self)
{
    if (InterlockedCompareExchange(&g_overlapSaid, 1, 0) != 0) { return; }
    const DWORD holderTid = g_gateHolderTid;
    void* const holderSelf = g_gateHolderSelf;
    void* const primary = g_primarySelf;
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] OVERLAP (first of the session) - thread %lu (self %p, %s) found the driver gate HELD by thread "
        "%lu (self %p, %s; the last holder to enter) - this frame SKIPPED. Every later one is counted as `overlap "
        "skipped` in [SWING STATS].",
        GetCurrentThreadId(), self, self == primary ? "the primary" : "NOT the primary", holderTid, holderSelf,
        holderSelf == primary ? "the primary" : "NOT the primary");
}

// The primary has been quiet >= 500 ms. `last` AHEAD of `now` (another thread stamped it after `now` was read) is
// NOT quiet - the unsigned difference would otherwise wrap to a huge value and hand the primary to another self.
bool PrimaryQuiet(ULONGLONG now, ULONGLONG last) { return now > last && now - last >= 500; }

// Everything the gate protects: the primary latch, then DriverFrame. Runs only while THIS thread holds g_gate. The
// tick is read HERE, inside the gate, so no other thread can stamp g_primaryLastTick between the read and the test.
void GatedFrame(void* self)
{
    const ULONGLONG nowTick = GetTickCount64();
    g_gateHolderTid = GetCurrentThreadId();
    g_gateHolderSelf = self;
    // F1: latch the PRIMARY WeaponMeleeController. A brand-new self only takes over once the current
    // primary has gone quiet for >= 500 ms - inside that window a second self calling is SHARING the
    // slot, not replacing the player's instance.
    if (g_primarySelf == nullptr || (self != g_primarySelf && PrimaryQuiet(nowTick, g_primaryLastTick))) {
        void* prev = g_primarySelf;
        g_primarySelf = self;
        if (g_det.state() == swing::State::Active) {
            ++g_ends;
            // m5: the swing is dropped by the reset below - say so, or its FIRE has no END.
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] END #%ld reason=relatch (primary WeaponMeleeController changed) | mode %d",
                g_totFires, MeleeSwingMode);
        } else if (g_det.state() == swing::State::Candidate) {
            // A pending candidate is dropped by the reset below - nothing fired, so no END line; the drop line
            // instead (reason relatch). Relatch only - NowMs is one QPC read.
            AbandonCandidate();
        }
        ResetSwingState();   // R3: a swing (and its per-swing reset) never carries over to another self
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] primary WeaponMeleeController = %p (previous %p)", self, prev);
    }
    if (self != g_primarySelf) {
        InterlockedIncrement(&g_otherSelfCalls);
        return;   // not the primary - do not feed the detector
    }
    g_primaryLastTick = nowTick;
    DriverFrame(self);
}

// The light stagger window's close: the melee controller's slot 16 ran again, so the reaction handed back (INV-639). One line per
// window (capped - INV-9). COST: one compare when no window is open.
void StaggerWindowClose(void* self)
{
    if (!g_staggerSelf || self != g_staggerSelf) { return; }
    g_staggerSelf = nullptr;
#if defined(DLVR_SWING_PROBE)   // the probe variant: uncapped (trace 20261119_1730)
    if (true) {
#else
    if (g_staggerLines < 300) {
#endif
        ++g_staggerLines;
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] LIGHT STAGGER #%ld END after %llu ms - the melee controller is updated again | swing frames fed from "
            "the inactive update %ld | swing FIREs in it %ld | B1 (hit) calls in it %ld%s",
            g_staggerN, GetTickCount64() - g_staggerT0, g_staggerFrames, g_totFires - g_staggerFiresAt, g_b1Calls - g_staggerB1At,
            g_staggerLines == 300 ? " | LAST LIGHT STAGGER line" : "");
    }
}

// The player's active controller group from the melee controller (vfunc +32 = its player, INV-639's +3324). False = unreadable.
bool ReadPlayerGroup(void* self, int* group)
{
    __try {
        void* player = VCall<void*>(self, 32);
        if (!player) { return false; }
        *group = *reinterpret_cast<const int*>(static_cast<const char*>(player) + kPlayerGroupOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The swing driver's frame, shared by slot 16 (the melee controller active) and slot 17 (the light stagger). A fault in our part
// is caught, counted, logged once, and disables the WHOLE swing driver for the session (fail CLOSED) rather than killing the game.
void SwingFeed(void* self)
{
    if (MeleeSwingMode == 0 || !self) { return; }
    // M3: a fault disables the WHOLE driver, not just B1 - skip DriverFrame (and everything below) entirely
    // once it has happened, rather than re-entering the __try every frame for nothing.
    if (g_disabled) { return; }
    volatile bool entered = false;   // read by the __except below - volatile so it is not cached in a register
    __try {
        const ULONGLONG nowTick = GetTickCount64();
        // U-60 (b): another player's controller (co-op) never reaches R1's count or the primary latch.
        const int cls = ClassifySelf(self);
        if (cls == kSelfRemote) {
            InterlockedIncrement(&g_remoteSelfCalls);
            return;
        }
        TrackSeen(self, g_seenSelves, g_seenSelvesCount);
        TrackSeen(GetCurrentThreadId(), g_seenThreads, g_seenThreadsCount);

        // Gate: an unrelated controller while the primary is fresh never touches the gate - otherwise its call
        // could hold the gate and make the PRIMARY's frame skip. Racy reads, both aligned single loads; a stale
        // read only sends the call into the gate, where the latch below re-decides it exactly as before.
        void* const primary = g_primarySelf;
        if (primary != nullptr && self != primary && !PrimaryQuiet(nowTick, g_primaryLastTick)) {
            InterlockedIncrement(&g_otherSelfCalls);
            return;
        }
        if (!g_gate.TryEnter()) {   // counted inside swing::Gate
            NoteOverlap(self);
            return;
        }
        entered = true;
        GatedFrame(self);
        entered = false;
        g_gate.Leave();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A fault INSIDE g_hit unwinds past B1Frame's `g_b1Active = false`. Left set, every later sweep-builder
        // call on this thread - every native R2 attack - would be answered by B1Build. Clear it first.
        g_b1Active = false;
        g_b1Self = nullptr;   // m2
        if (entered) { g_gate.Leave(); }   // the driver is disabled below either way - never leave the gate held
        ++g_faults;
        g_lastFaultCode = GetExceptionCode();
        if (InterlockedIncrement(&g_totFaults) == 1) {
            g_disabled = true;
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] *** FAULT 0x%08lX inside the swing driver - SWING DRIVER DISABLED for this session "
                "(the game's own update already ran; no further [SWING] lines will print - see [SWING STATS]). ***",
                g_lastFaultCode);
        }
    }
}

// The slot-16 replacement. The ORIGINAL runs first and untouched; ours runs after, on the same thread, with the
// live `this` - so INV-131's stale-pointer hazard cannot arise.
void __fastcall SwingUpdateThunk(void* self)
{
    g_origUpdate(self);
    StaggerWindowClose(self);
    SwingFeed(self);
}

// The slot-17 replacement (INV-639; Jay 2026-10-27: "It should count" -> "Just light"). The game's damage reaction (group 30)
// stops the melee controller, so slot 16 - our swing - does not run; the inactive update does. The ORIGINAL runs first and
// untouched; then, ONLY while the player's group is 30, the same swing frame as slot 16 (every gate unchanged). Every other
// group (knockdown 16, grabs, ladders, dialogue ...) is left alone. COST per call: the original, a mode test, one SEH-guarded
// virtual call + int read; in group 30 the swing frame slot 16 would have run.
void __fastcall SwingInactiveThunk(void* self, float dt)
{
    g_origInactive(self, dt);
    if (MeleeSwingMode != 2 || !self || g_disabled) { return; }
    int group = -1;
    const bool read = ReadPlayerGroup(self, &group);
    if (!read || group < -1 || group > 33) {
        if (!g_staggerGroupBadSaid) {
            g_staggerGroupBadSaid = true;
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=no
            LOG("swing: [SWING] LIGHT STAGGER feed REFUSED - the player's controller group (PlayerDI +%zu, Steam build) is %s%d - "
                "not a group 0-33 / -1; swings during a stagger are not fed (said once)",
                static_cast<size_t>(kPlayerGroupOff), read ? "" : "unreadable, last ", group);
        }
        return;
    }
    if (!swing::FeedFromInactive(group)) { return; }
    if (g_staggerSelf != self) {
        g_staggerSelf = self;
        g_staggerT0 = GetTickCount64();
        ++g_staggerN;
        g_staggerFrames = 0;
        g_staggerB1At = g_b1Calls;
        g_staggerFiresAt = g_totFires;
    }
    ++g_staggerFrames;
    SwingFeed(self);
}

}  // namespace

bool SwingInstall()
{
    // M5: g_installed alone does not mean "installed" - a REFUSAL also sets it (so it does not re-log every
    // second). Only g_slot >= 0 means the detour is actually live.
    if (g_installed) {
        // I1: slot 16 is live - the builder is (re)tried here, once a second, so a live 1 -> 2 switch installs it
        // within a second. TryBuilderHook does nothing outside mode 2, once live, or once refused for good.
        if (g_slot >= 0 && TryBuilderHook()) {
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] builder hook gamedll+0x%zX LIVE (installed after ARMED, try %d) | mode %d",
                static_cast<size_t>(kBuildRva), g_builderTries, MeleeSwingMode);
        }
        return g_slot >= 0;
    }
    if (MeleeSwingMode == 0) {
        if (InterlockedExchange(&g_notArmedLogged, 1) == 0) {
            // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING] NOT ARMED - melee_swing_mode=0, nothing patched.");
        }
        return false;
    }
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !eng) {
        return false;   // retried next second by camhook's install loop; not a finding yet
    }
    const uintptr_t vt = RttiVtableOf("WeaponMeleeController");   // install path only - never per frame
    if (!vt) {
        return false;   // no melee controller constructed yet; retried
    }
    g_gd = reinterpret_cast<uintptr_t>(gd);
    const uintptr_t want = g_gd + ::dlvr::buildrva::Gd(kUpdateRva);   // the EGL rva on the Epic build (buildrva table)
    int found = -1;
    int readable = 0;
    for (int i = 0; i < kScanSlots; ++i) {
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
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] REFUSED - gamedll+0x%zX (the melee per-frame update) is NOT in the first %d readable "
            "slot(s) of vtable %p. Nothing written. (INV-49: never hook a guessed index.)",
            static_cast<size_t>(kUpdateRva), readable, reinterpret_cast<void*>(vt));
        g_installed = true;   // refused for good - do not re-log every second
        return false;
    }

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
    g_handElem  = reinterpret_cast<HandElemFn>(g_gd + ::dlvr::buildrva::Gd(kHandElemRva));
    g_reach     = reinterpret_cast<ReachFn>(g_gd + ::dlvr::buildrva::Gd(kReachRva));
    g_classGate = reinterpret_cast<BoolSelfFn>(g_gd + ::dlvr::buildrva::Gd(kClassGate));
    g_slotSel   = reinterpret_cast<BoolSelfFn>(g_gd + ::dlvr::buildrva::Gd(kSlotSelRva));
    g_getId     = reinterpret_cast<GetIdFn>(GetProcAddress(eng, "?GetElementID@IModelObject@@QEBAHPEBD@Z"));
    g_getWorld  = reinterpret_cast<GetWorldFn>(
        GetProcAddress(eng, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));

    void** slot = reinterpret_cast<void**>(vt) + found;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG("swing: [SWING] REFUSED - VirtualProtect failed on vtable %p slot %d, GetLastError=%lu.",
            reinterpret_cast<void*>(vt), found, GetLastError());
        g_installed = true;
        return false;
    }
    g_origUpdate = reinterpret_cast<UpdateFn>(want);
    *slot = reinterpret_cast<void*>(&SwingUpdateThunk);
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_slot = found;
    g_installed = true;
    g_installTick = GetTickCount64();   // F2: SwingStatsTick's baseline before the first STATS line
    // The light stagger feed (INV-639): the inactive update, found by its address like slot 16 (INV-49) - mode 2 only. A refusal
    // leaves slot 16 exactly as above. The EGL rva comes from the buildrva table (tools/egl_allowlist.tsv, melee family).
    if (MeleeSwingMode == 2) {
        const uintptr_t want17 = g_gd + ::dlvr::buildrva::Gd(kInactiveRva);
        int found17 = -1;
        for (int i = 0; i < kScanSlots; ++i) {
            uintptr_t v = 0;
            __try {
                v = reinterpret_cast<uintptr_t*>(vt)[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (v == want17) { found17 = i; break; }
        }
        DWORD old17 = 0;
        if (found17 >= 0 && VirtualProtect(reinterpret_cast<void**>(vt) + found17, sizeof(void*), PAGE_READWRITE, &old17)) {
            g_origInactive = reinterpret_cast<InactiveFn>(want17);
            reinterpret_cast<void**>(vt)[found17] = reinterpret_cast<void*>(&SwingInactiveThunk);
            DWORD dummy17 = 0;
            VirtualProtect(reinterpret_cast<void**>(vt) + found17, sizeof(void*), old17, &dummy17);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            g_slot17 = found17;
        }
        // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=no
        LOG("swing: [SWING] light stagger feed %s (logged unconditionally in mode 2) | inactive update gamedll+0x%zX: %s, "
            "vtable %p slot %d | fed only while the player's controller group (PlayerDI +%zu) is %d (INV-639)",
            g_slot17 >= 0 ? "LIVE" : "REFUSED - swings during a light stagger are not seen", static_cast<size_t>(kInactiveRva),
            found17 < 0 ? "NOT FOUND in the readable slots" : (g_slot17 >= 0 ? "patched" : "found, VirtualProtect failed"),
            reinterpret_cast<void*>(vt), found17, static_cast<size_t>(kPlayerGroupOff), swing::kLightStaggerGroup);
    }
    // I1: the gamedll+0xDC2000 sweep-builder detour (spec §2.3) - mode 2 only, and only now that slot 16 is live.
    // Mode 1 patches slot 16 and nothing else.
    TryBuilderHook();
    const char* builderState =
        MeleeSwingMode != 2
            ? (MeleeSwingMode == 1 ? "not installed (mode 1 - slot 16 only)"
                                   : "not installed (mode is not 2 - slot 16 only)")
            : (g_builderLive ? "LIVE"
                             : (g_builderRefused ? "NOT LIVE - REFUSED for the session, mode 2 will REFUSE every hit"
                                                 : "NOT LIVE - retried each second, mode 2 REFUSES every hit until it is"));
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] ARMED (logged unconditionally) | mode %d | update gamedll+0x%zX found at vtable %p SLOT %d | "
        "builder hook gamedll+0x%zX %s | GetElementID %p GetElementWorldMatrix %p | fire %.2f arm %.2f end %.2f m/s, "
        "window %.0f ms, max %.0f ms, blade pitch %.1f deg | holster zone refusal uses centre (%.3f %.3f %.3f) r %.3f "
        "| min travel %.2f m | toward-body refusal %s | return-stroke rule %s (window %.0f ms, cone %.0f deg around "
        "toward-the-body)",
        MeleeSwingMode, static_cast<size_t>(kUpdateRva), reinterpret_cast<void*>(vt), found,
        static_cast<size_t>(kBuildRva), builderState,
        reinterpret_cast<void*>(g_getId), reinterpret_cast<void*>(g_getWorld), MeleeSwingFireSpeed,
        MeleeSwingArmSpeed, MeleeSwingEndSpeed, static_cast<double>(MeleeSwingWindowMs),
        static_cast<double>(MeleeSwingMaxMs), MeleeBladePitchDeg, HolsterCenterX, HolsterCenterY, HolsterCenterZ,
        HolsterRadius, static_cast<double>(MeleeSwingMinTravelM),
        MeleeSwingRefuseTowardBody ? "ON (any stroke in the cone never fires)" : "OFF",
        MeleeSwingRefuseTowardBody ? "UNUSED (toward-body refusal is on)" : (MeleeSwingReturnMs > 0.0f ? "ON" : "OFF"),
        static_cast<double>(MeleeSwingReturnMs), static_cast<double>(MeleeSwingReturnDeg));
    // [PROBE: SWING] gate=melee_swing_mode  site=HEADSET  log-only=yes
    LOG("swing: [SWING] driver gate ARMED (logged unconditionally) | non-blocking: a slot-16 call that finds another "
        "thread inside the driver SKIPS its frame and is counted (`overlap skipped` in [SWING STATS]; the first one "
        "logs both threads) | overlaps so far %ld",
        g_gate.Overlaps());
    return true;
}

// F2: zero calls per second is silent otherwise - a driver that never gets called looks identical, in the log,
// to one that has nothing to report. Called once a second from camhook.cpp's install loop, independent of
// whether DriverFrame itself ever runs (that is the point). Single-writer: only DriverFrame writes
// g_lastStatsEmit; this function only reads it.
void SwingStatsTick()
{
    if (g_slot < 0) { return; }   // never installed, or refused - nothing armed to report on
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_lastReported = 0;
    if (g_disabled) {
        if (now - s_lastReported >= 1000) {
            s_lastReported = now;
            // [PROBE: SWING STATS] gate=melee_swing_mode  site=HEADSET  log-only=yes
            LOG("swing: [SWING STATS] *** DISABLED after fault 0x%08lX *** - faults %ld", g_lastFaultCode,
                g_totFaults);
        }
        return;
    }
    // R4: the primary's most recent call counts too - between install and the first STATS line (which needs 1 s of
    // calls) the driver is being called, and without it that gap printed one false "NO update calls" per session.
    const ULONGLONG lastEmit = g_lastStatsEmit;
    const ULONGLONG lastCall = g_primaryLastTick;
    ULONGLONG baseline = lastEmit > g_installTick ? lastEmit : g_installTick;
    if (lastCall > baseline) { baseline = lastCall; }
    // `now` was read before the game thread's latest write: baseline may be a few ms AHEAD of it - never underflow.
    if (now > baseline && now - baseline > 1500 && now - s_lastReported >= 1000) {
        s_lastReported = now;
        // [PROBE: SWING STATS] gate=melee_swing_mode  site=HEADSET  log-only=yes
        LOG_PERIODIC("swing: [SWING STATS] NO update calls for %llu ms - the melee update was not reached (no melee weapon "
            "out, or not in a level)",
            static_cast<unsigned long long>(now - baseline));
    }
}

unsigned long long SwingPrimaryLastCallTick()
{
    return g_primaryLastTick;   // volatile, aligned 64-bit load on x64 (R4) - no lock needed
}

bool SwingPrimaryMeleeState(int* state)
{
    const ULONGLONG last = g_primaryLastTick;
    const ULONGLONG now  = GetTickCount64();
    if (MeleeSwingMode == 0 || last == 0 || now < last || now - last > 250) { return false; }   // not called recently = not known
    *state = static_cast<int>(g_primaryState);
    return true;
}

}  // namespace dlvr
