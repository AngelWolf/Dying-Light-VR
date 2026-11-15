// VR swing-triggered melee attack (B1) - docs/SPEC-VR-SWING-ATTACK-2026-09-29.md. Settings live in swing.cpp
// (each with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp after the holster rows.
#pragma once

namespace dlvr {

extern int   MeleeSwingMode;       // melee_swing_mode       - 0 off (nothing installed), 1 detect only, 2 hit. DEFAULT 2.
extern float MeleeSwingFireSpeed;  // melee_swing_fire_speed - m/s, FIRE at or above
extern float MeleeSwingArmSpeed;   // melee_swing_arm_speed  - m/s, arm below
extern float MeleeSwingEndSpeed;   // melee_swing_end_speed  - m/s, END below
extern float MeleeSwingWindowMs;   // melee_swing_window_ms  - velocity window minimum span, ms
extern float MeleeSwingMaxMs;      // melee_swing_max_ms     - hard END, ms
extern float MeleeSwingMinTravelM; // melee_swing_min_travel_m - path a swing must travel before it FIREs, m
extern float MeleeSwingReturnMs;   // melee_swing_return_ms  - return-stroke rule window after an END, ms; 0 = off
extern float MeleeSwingReturnDeg;  // melee_swing_return_deg - cone around "toward the body" that counts as a return, deg
extern bool  MeleeSwingRefuseTowardBody; // melee_swing_refuse_toward_body - 1 = any stroke in the cone never fires
extern float MeleeSwingReturnFastMps;    // melee_swing_return_fast_mps - a return-window stroke at least this fast fires; 0 = off
extern float MeleeSwingReversalDeg;      // melee_swing_reversal_deg - turn within the look-back that ENDs a swing, deg
extern float MeleeSwingReversalMs;       // melee_swing_reversal_ms  - the reversal look-back, ms; 0 = melee_swing_window_ms
extern float MeleeBladePitchDeg;   // melee_blade_pitch_deg  - blade tilt about grip +X, degrees
extern float MeleeHitMarginM;      // melee_hit_margin_m     - the B1 line's reach past the drawn weapon's tip, m (0..0.30)
extern float MeleeMinReachM;       // melee_min_reach_m      - the B1 line's minimum reach from the hand along the shaft, m (0..1.20)
extern float MeleeHitThicknessM;   // melee_hit_thickness_m  - the hit capsule's radius (across the swing plane + past the end), m
extern float MeleeHitBackM;        // melee_hit_back_m       - the hit capsule's reach back behind the hand, m
extern bool  MeleeHitFactorRestore; // melee_hit_factor_restore - rewrite each hit's distance factor to the game's own line's
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagMeleeSwingProbe = false;
#else
extern bool  DiagMeleeSwingProbe;  // diag_melee_swing_probe - per-frame detail lines while a swing is active
#endif
extern bool  AnimSuppressMeleeIdle; // anim_suppress_melee_idle - keep the melee ready state on "Stand" (INV-319). DEFAULT ON.
// diag_swing_shadow - the swing shadow probe (blade vs hit line), log only. DEFAULT OFF. DEV only:
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): constant false, the gated code is dropped
inline constexpr bool DiagSwingShadow = false;
#else
extern bool  DiagSwingShadow;
#endif

// Idempotent. Called once a second from camhook.cpp's install loop while MeleeSwingMode != 0. Resolves the
// WeaponMeleeController vtable, swaps slot 16 (found by scanning for gamedll+0xDB1D10, never by index), and
// in MODE 2 ONLY, once slot 16 is live, installs the gamedll+0xDC2000 detour (retried each call until live or
// refused for good). Every refusal names itself in the log. Returns true once slot 16 is installed.
bool SwingInstall();

// Fix round 1 (F2). Called once a second from camhook.cpp's install loop, right after SwingInstall(), while
// MeleeSwingMode != 0. Reports "zero calls" and "disabled after a fault" by NAME - a driver that never runs
// is otherwise indistinguishable, in the log, from one with nothing to report.
void SwingStatsTick();

// anim_suppress_melee_idle: 1 Hz, unconditional - logs [IDLESUP] REFUSED once per change when it cannot act (mode 0).
void MeleeIdleSuppressTick();

// GetTickCount64 of the primary WeaponMeleeController's most recent slot-16 call; 0 = never (or mode 0, nothing
// installed). A cross-check for melee_r2_suppress's log only - U-c: slot 16 is not called with a firearm or the bow.
unsigned long long SwingPrimaryLastCallTick();

// The primary WeaponMeleeController's EMeleeControllerState (+0x110; -1 = ready, anything else = an attack / throw / charge
// state - mac UpdateStateAttack / UpdateStateThrowMelee / UpdateChargeAttack), as of its last slot-16 call. false = NOT KNOWN
// (mode 0, or no call in the last 250 ms - no melee weapon out). For item_rel_hold mode 3's melee hold (2026-10-28).
bool SwingPrimaryMeleeState(int* state);

}  // namespace dlvr
