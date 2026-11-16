// Manual reload - spec docs/superpowers/specs/2026-11-01-manual-reload-design.md, plan
// docs/superpowers/plans/2026-11-01-manual-reload-pistol.md. Task 4: the [RELOAD] probe (GUN / ELEMENTS / REST / STATE). Task 5: the
// steps driven - the game's own reload START refused for a manual pistol, every add-bullet but ours refused for it, R2 / L1 withheld
// while a step is live, the gun's own bone_magazine drawn at the left hand (shrunk to nothing once let go - Jay: "Yes, go with A"),
// and the rounds added by the game's own add-bullet at the seat / rack. The hand steps are reload_logic.h (unit-tested).
//
// HOW (OUR gamedll 97046bba = deployed; RESEARCH-MANUAL-RELOAD-2026-10-31.md section 18, IDA mr1-mr4; prologues read from the file):
//   - HookTail's argument is PlayerFppVis; *(vis + 0x50) = PlayerDI (breakout.cpp kVisPlayerOff, HookTail's own first read).
//   - The firearm controller = *(PlayerDI + 3096) (paired with +3104 in the X handler +D9F890, mr4:202-251). Its +344 = the held
//     item's slot; its vfunc +32 = the player; PlayerDI vfunc +1944(slot) = the selected InventoryItem (+DAF580 / +DAF5C0 do the same).
//     +600 = the reload list index (-1 = not reloading, section 6 field table). RUN RL0 / RL0B: +3096 slot 0 held the RIGHT hand's
//     item on every line.
//   - InventoryItem + 64 = rounds in the clip (+DAF580); item + 88 = its desc: desc vfunc +448 = item type (18 = the pistol class,
//     G6), desc vfunc +1624 -> { char* +0, u32 len +8 } = the anim prefix (rl2_out.txt:2026-2053; the game returns 'beretta' - RL0),
//     desc vfunc +608 = clip max.
//   - Spare = +BD0270(player, +6F2F50(item + 64), 1) = the inventory count of the clip's own ammo id MINUS the clip (G3, mr2:136-185).
//   - The held gun's MODEL = *(captured entity + 0x1020) (hand 0 = right; HeldItemElementWorld, INV-277). Its elements by name
//     through the engine's IModelObject exports, resolved here by mangled name. RL0B: the pistol is ONE skinned mesh 'body'; the
//     magazine is the part of it bone_magazine moves (no separate mesh element) - so "the mag vanishes" = bone_magazine's world
//     written with its basis scaled to kHideScale (Jay: "Yes, go with A").
//   - The parts' frames (G7, the game's own clips; RL0B in play): bone_magazine leaves along its PARENT's +Z; bone_chamber moves
//     along its parent's +X, 0.1182 at rest, 0.1550 locked back while the clip is 0. world = parentWorld x local (row-major 3x4,
//     basis = COLUMNS).
//   - START (+DA6E00, void(ctrl, a2, char)): returning without the body leaves state 0 at every caller (G1; the auto-reload re-calls
//     it every update while the clip is 0 - the refusal is two vfunc calls + a compare, no log after the capped lines).
//   - ADD-BULLET (+DAF160, void(ctrl, item)): its first 9 bytes are `test rdx,rdx; je rel32` (not relocatable), so the detour sits at
//     +DAF169 - 14 position-independent bytes (push rbp; push r15; sub rsp,38h; mov rax,[rdx+0D8h]) that no branch in the function
//     targets (capstone over all 221 instructions; control: 35 branches into the body found). Nothing is pushed before +9, so the
//     hook is entered with the caller's own stack and returns to the caller. OUR seat call goes to the trampoline (skips the hook).
//
// SMG (plan docs/superpowers/plans/2026-11-01-manual-reload-smg.md, spec 2026-11-01-manual-reload-smg-design): the five SMG meshes
// are classified by reload_guns.h (type 18 + prefix submachinegun / _alt + both bones + the MESH NAME, read once per model through
// ItemMeshName). They run the pistol's steps; the grab distances are measured to Jay's hand spots carried on bone_magazine /
// bone_chamber (reload_guns_table.inc - on A / Rust / Eagle the mag is ~15 cm from its joint), the rack completes at the same share
// of the gun's own bolt travel as reload_rack_pull_m is of the pistol slide's, and the BOLT is drawn during the rack (BoltLocalX:
// never forward of where it was grabbed, never past the game's travel) and settled after it. The pistol is unchanged (RUN RL2C).
//
// THREADS: ReloadTick (game thread) runs the step machine and every game call; ReloadPadPoll / ReloadWithholdsR2 (pad thread) read
// the published words below; the two detours run on whichever thread calls the game function (only guarded reads + the item list).
// Every game read and call is SEH-guarded; a fault at resolve turns the feature off for the launch, named.
#include "reload.h"

#include "boneblend.h"      // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "wristquad.h"      // wristquad::OnThisLaunch - gates the wrist HUD's gun-family read (stage B)
#include "pumpsound.h"      // PumpSoundInstall - the pump shotgun's rack-sound detour (+110D890)
#include "bownock.h"        // BowNockCatchM / HoldM / SeatM / CatchDeg / CatchLenM - the crossbow's lay / seat tolerances (no new key)
#include "camhook.h"        // BoneProbeCapturedEntity
#include "controllers.h"    // GetControllerSqueeze / PulseController
#include "handcarry.h"      // the crossbow's bolt on the rendered l_hand (HandCarryTakeAs - crossbow plan Task 3 / 5)
#include "handcarry_logic.h"   // handcarry::Decide / SameObject - the bow nock's single-owner carry rule
#include "gunshape.h"       // GunShapeMagClearM - the held-mag well rail's clear distance per gun (Jay 2026-11-02 (4))
#include "gunsound.h"       // GunSoundPlay / GunSoundTick - the game's reload sounds at the player's steps (solid guns Task 5)
#include "magrail_logic.h"  // the held mag on the well's line: Decide / Measure / Draw (Jay 2026-11-02 (4))
#include "holster.h"        // HolsterLeftPouchHeld / HolsterHaptic
#include "reload_fx.h"      // per gun: racks + the game's reload sounds per step (solid guns plan Task 3)
#include "reload_guns.h"    // Classify / RackPullFor / BoltLocalX + the SMG rows
#include "reload_hands.h"   // the left hand's poses: ActiveHandPose / RackRowFor / MagRowFor / LockedHand / HandBlendT
#include "reload_logic.h"
#include "pump_logic.h"     // the pump shotgun's steps (PMachine - pump plan Task 1)
#include "dbarrel_logic.h"  // the double-barrel's steps (DMachine) + its generated hand / shell data (dbarrel_hands.h)
#include "break6_logic.h"   // 2026-11-09: the Schofield's steps (SMachine)
#include "gate_logic.h"     // 2026-11-09: the Sixshooter / Walker's steps (GMachine)
#include "twist_logic.h"    // RG5: the gate flick = the right controller's twist
#include "cock_logic.h"     // 2026-11-09: the fan-cock after a revolver reload (CMachine)
#include "fuel_logic.h"     // 2026-11-09: the chainsaw gun's pour + cord (FMachine)
#include "twohand.h"        // TwoHandGripping - the pump's rack hold IS the two-hand grip (pump plan ruling P5)
#include "twohand_logic.h"  // StandardCWorld / SlerpM3 / Ortho3 - the lock's rotation, as the bow's (INV-506)
#include "xbow_logic.h"     // the crossbow's steps (XMachine - crossbow plan Task 1)

#include <windows.h>
#include <intrin.h>   // _ReturnAddress - the refused caller in the START / ADD-BULLET lines

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs (the crossbow's 5 s counts line only)

// The Epic (EGL) build: every gamedll rva below is on tools/egl_allowlist.tsv (family 15, translated exactly by
// tools/egl_rva_table.py). The DEV tree has no buildrva.h: Steam rvas only. The two DETOUR targets are handed to
// InstallSingleGameDetour as Steam addresses - it moves an allowlisted one itself.
// The crossbow's bolt-visibility detour (+755370) is NOT on the allowlist (crossbow plan Task 5, carry ruling T0-a): on the Epic
// gamedll it is refused by name, never guessed (DLVR_RELOAD_GD_IS_EGL).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_RELOAD_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_RELOAD_GD_IS_EGL() (::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll) == ::dlvr::buildrva::kEgl)
#else
#define DLVR_RELOAD_GD_RVA(rva) (rva)
#define DLVR_RELOAD_GD_IS_EGL() false
#endif

namespace dlvr {

// [DEP: manual_reload] required-for=reloading a pistol or an SMG by hand (ManualReload; the SMG: plan 2026-11-01-manual-reload-smg,
//      its bolt drawn during the rack) - spec 2026-11-01-manual-reload-design; plan Task 4
//      = the [RELOAD] probe, Task 5 = the steps: the game's reload START (+DA6E00) and every add-bullet but ours (+DAF160) refused
//      for a held-and-classified pistol, R2 withheld and the left grip's L1 withheld while a step is live, bone_magazine drawn at the
//      left hand / shrunk once let go, the rounds added through the game's own +DAF160 at the seat / rack.
//      Also makes the left hip the ammo pouch (holster.cpp HolsterLeftPoll: the hip zone is live only while this is on).
//      tree=INERT at 0: ReloadTick / ReloadPadPoll return at their first test, both detours call straight through, nothing is
//      withheld; the hip zone is off; X and the auto-reload are the game's. LOADBEARING at 1 for every pistol reload.
//      HEADSET: the steps need the LEFT grip pose (the rig's published grip) and the left squeeze; the gamedll calls / detours need
//      the Steam gamedll 97046bba or the Epic build (buildrva family 15) - each function's first bytes are compared before use; a
//      mismatch REFUSES by name. FLAT: READY / GUN / REFUSED / ELEMENT lines and the START refusal (X does nothing on the pistol) are
//      structural; every distance and every drawn part is HEADSET-only. Was compiled default 0 (Tier C - Jay: "Decide after the pistol").
//      LEFT HAND POSES (plan 2026-11-01-reload-hand-poses, no new key): while a mag is held the left fingers take the gun's mag-hold
//      row and the mag sits at that row in the hand AT ONCE from the grab (2026-11-02, Jay after HP2: "In hand at the grip", no
//      100 ms ease) - the pistol's PULL row (kPullRows, Jay's E1F) for the pulled mag, its mag row for the fresh one; while Racking
//      the left hand is LOCKED (position + rotation) to bone_chamber in
//      the rack row's pose (boneblend.cpp left solve: ReloadPinRot / ReloadPinPos; the two-hand pin skipped) and the pull is read
//      from the COMMANDED hand. Fingers need rig_subtree_rest=1 + rig_subtree_fingers=1 (ReloadFingersApply); the lock does not.
//      THE CROSSBOW (plan 2026-11-03-manual-reload-crossbow Task 5; spec 2026-11-03-manual-reload-crossbow-design, INV-676) rides this
//      switch, no new key: type 37 classified by mesh (reload_guns.h ClassifyCrossbow, the three bones checked); X / the auto-reload
//      and every add-bullet but ours refused through the same two detours; a THIRD detour on CrossbowVis's slot-58 bolt predicate
//      (+755370) shows the carried / laid bolt; the rack (bone_stringreload) pulled back by the left hand to the catch (Jay's rack
//      pose locked), then a bolt taken over the LEFT shoulder (holster.cpp's mirrored zone, live only with a crossbow out), carried on
//      the rendered l_hand (handcarry.cpp), laid on the groove and slid to the seat (the game's own add-bullet). It also reads
//      reload_grab_m, reload_rack_pull_m and the five bow_nock_* values (bownock.cpp). HEADSET (the left grip pose + squeeze); FLAT:
//      the [XBOW] ARMED / DETOUR / DRAW / REFUSED lines are structural. Fix round 1 (rulings T5-a / T5-b / T5-c): the parts' rests are
//      the row's mesh data (no live latch), bolt_pose (the aim point) is put back at rest on every exit from Laid that is not a seat
//      and in ReloadOffTick, and the string is drawn forward while Empty.
//      THE PUMP SHOTGUN (plan 2026-11-04-manual-reload-pump-shotgun Task 4; spec 2026-11-04-manual-reload-pump-shotgun-design, INV-677)
//      rides this switch, no new key: type 24 classified by mesh (reload_guns.h ClassifyPump - wn_shotgun_b / f, prefix shotgun_pa;
//      2026-11-07 also the semi-auto wn_shotgun_c, prefix shotgun_sa, its row racks=false - Jay "No rack"; the double-barrel keeps the
//      game's reload); shells from the left hip, pushed one per port entry (reload_snap_m), the game's own
//      add-bullet per shell, racked by the two-hand grip (reload_rack_pull_m's share of the pump travel), R2 withheld while not
//      chambered, bone_reload_shotgun + ammo_bullet drawn, the game's per-shot rack sound silenced (pumpsound.cpp), the left fingers in
//      Jay's C13 animation (boneblend.cpp ReloadFingersApply) and the two-hand pin slid with the stroke (twohand.cpp TwoHandLeftPin).
//      FLAT: [PUMP] ARMED / DRAW / REFUSED / SHOT are structural; every hand step is HEADSET.
//      SHIPS ON since 2026-11-03 (Jay: "New tester package, with manual reload on"; was "Ship OFF for now", 11-01).
bool ManualReload = true;
// [DEP: reload_snap_m] required-for=the seat (reload_logic.h FreshInHand -> Seat / Done). Claude's start 0.05 m (spec goal 4
//      "generous"), Tier D. tree=INERT at manual_reload=0.
float ReloadSnapM = 0.05f;
// [DEP: reload_align_deg] required-for=the seat's "roughly pointed the right way". Claude's start 45 deg, Tier D. INERT at 0 master.
float ReloadAlignDeg = 45.0f;
// [DEP: reload_pull_out_m] required-for=Pulling -> MagInHand (the mag is out): the left grip's distance from where it grabbed, in
//      ANY direction (2026-11-02; was along the well). Claude's start 0.06 m (the game's own clip moves it 0.103 m out, G7), Tier D.
//      INERT at manual_reload=0.
float ReloadPullOutM = 0.06f;
// [DEP: reload_rack_pull_m] required-for=Racking -> RackDone - as soon as the pull reaches it, the grip still held (2026-11-02, Jay
//      after HP2; was on the release). Claude's start 0.03 m (the slide's own travel is 0.0368, G7), Tier D. Since 2026-11-02 (Jay
//      after HP4, "Pistol only") the PISTOL ignores it and needs its full 0.0368 travel; it sets the SMG's share of its bolt travel.
//      The CROSSBOW (crossbow plan Task 5, ledger ruling P2) catches at the same share of its rack travel: 0.2176 x key / 0.0368.
float ReloadRackPullM = 0.03f;
// [DEP: reload_grab_m] required-for=the grab of the mag (Idle -> Pulling) and of the SMG's handle (NeedsRack -> Racking). Claude's
//      start 0.08 m, Tier D. INERT at manual_reload=0. The PISTOL's slide grab is 0.12 m (reload_guns.h kPistolRackGrabM - Jay after
//      HP4: "Too small" -> "12 cm, pistol slide only").
//      The CROSSBOW's rack grab does NOT use it since 2026-11-04: 0.10 m, reload_guns.h kXbowRackGrabM (Jay after XB1b: "Rack needs
//      slightly more radius" -> "10 cm") - the commanded left hand bone within that of Jay's rack pose on the forward rack (ruling T2-a).
float ReloadGrabM = 0.08f;
// [DEP: reload_flick_dps] required-for=the double-barrel's FLICK close (dbarrel_logic.h: the muzzle's upward turn rate, deg/s, for 2
//      updates in a row, while the gun is open - Jay 2026-11-07 FLICK "A" = the muzzle snapped up; "Yes, that's the one" for the close).
//      400 = Claude's start (Jay "Sure", 2026-11-07); Tier D - the shipped value follows Jay's disk once tuned (the [DBARREL] CLOSED line
//      prints the fastest muzzle-up turn seen while open, the tuning data). tree=INERT at manual_reload=0 or off the double-barrel.
//      HEADSET (bone_root's turn between updates).
float ReloadFlickDps = 400.0f;

namespace {

// ---- OUR gamedll 97046bba ----
constexpr size_t    kVisPlayerOff = 0x50;        // PlayerFppVis + 80 -> PlayerDI (a pointer)
constexpr size_t    kFireCtlOff = 3096;          // PlayerDI + 3096 -> WeaponFireController (G6)
constexpr size_t    kFireCtl2Off = 3104;         // PlayerDI + 3104 -> the other one (logged, not used)
constexpr size_t    kCtlSlotOff = 344;           // controller + 344 = the held item's slot id
constexpr size_t    kCtlReloadIdxOff = 600;      // controller + 600 = the reload list index (-1 = not reloading)
constexpr size_t    kCtlPlayerVfn = 32;          // controller vfunc +32 -> the player
constexpr size_t    kSelItemVfn = 1944;          // PlayerDI vfunc +1944(slot) -> InventoryItem
constexpr size_t    kItemClipOff = 64;           // InventoryItem + 64 = rounds in the clip
constexpr size_t    kItemDescOff = 88;           // InventoryItem + 88 = the item desc
constexpr size_t    kDescTypeVfn = 448;          // desc vfunc +448 -> item type
constexpr size_t    kDescPrefixVfn = 1624;       // desc vfunc +1624 -> { char*, u32 len } anim prefix
// The family test (type 18 + prefix + both bones; an SMG also by mesh) is reload::Classify (reload_guns.h, unit-tested).
constexpr size_t    kHeldItemOff = 0x1020;       // captured entity + 0x1020 + hand*8 -> the held model (hand 0 = right)
constexpr uintptr_t kRoundsRva = 0xDAF580;       // int(ctrl)
constexpr uintptr_t kMaxRva = 0xDAF5C0;          // int(ctrl)
constexpr uintptr_t kAmmoIdRva = 0x6F2F50;       // int*(item + 64)
constexpr uintptr_t kSpareRva = 0xBD0270;        // int(player, int* ammoId, char 1)
constexpr uintptr_t kStartRva = 0xDA6E00;        // void(ctrl, a2, char) - the reload start, DETOURED
constexpr uintptr_t kAddBulletBodyRva = 0xDAF169;   // +DAF160 + 9 - the add-bullet DETOUR site (see the header)
// First bytes, read from OUR gamedll file (no RIP-relative operand in any of them).
const unsigned char kRoundsHead[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x50, 0x20,
                                      0x8B, 0x93, 0x58, 0x01, 0x00, 0x00 };
const unsigned char kAmmoIdHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x18, 0x48,
                                      0x8B, 0xF9, 0x48, 0x83, 0xC1, 0x18 };
const unsigned char kSpareHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                     0x48, 0x8B, 0x81, 0x38, 0x09, 0x00, 0x00 };
// +DA6E00: mov [rsp+8],rbx; push rdi; sub rsp,20h; xor edx,edx; movzx edi,r8b = 16 bytes (the next is a call rel32 - not stolen).
const unsigned char kStartHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xD2, 0x41, 0x0F, 0xB6, 0xF8 };
// +DAF160's first 9 (test rdx,rdx; je +307h) - compared, not stolen - then the 14 stolen bytes at +DAF169.
const unsigned char kAddBulletEntry[] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x07, 0x03, 0x00, 0x00 };
const unsigned char kAddBulletBody[] = { 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x82, 0xD8, 0x00, 0x00, 0x00 };
// +DAF5C0's first 21 bytes are byte-identical to +DAF580's (both: vfunc +32, then the slot at +344).
constexpr float     kChamberClipRest = 0.1182f;  // G7: m_fpp_beretta_shootempty frame 0
constexpr float     kHideScale = 0.001f;         // option A: the let-go mag's basis scale (not 0 - a degenerate basis)
// The held-mag well rail (Jay 2026-11-02 (4)) - MY RULINGS, not Jay's numbers, no key: past the clear point the mag eases from the
// well's line into the fingers over 3 cm; a fresh mag's centre within 5 cm of the line locks on (lined up) / stops at the lip.
// RUN MR2 (Jay: "a little restrictive on putting the mag back in" -> "Had to line it up too exactly" -> "Yes please" to 3 -> 5 cm).
constexpr float     kRailEaseM = 0.03f;
constexpr float     kRailCaptureM = 0.05f;
constexpr ULONGLONG kStillMs = 300;              // the parts still this long before their rest latches (the bow's rule)
constexpr float     kStillEps = 1e-5f;
constexpr ULONGLONG kStateEveryMs = 1000;
constexpr ULONGLONG kPublishFreshMs = 250;       // a pad-thread read of a tail publish older than this = nothing live
constexpr long      kMaxEventLines = 200;        // GUN / REFUSED / REST / EVENT / SEAT lines a run (INV-9)
constexpr long      kMaxRefuseLines = 40;        // START / ADD-BULLET REFUSED lines a run
constexpr long      kMaxElemLines = 192;         // ELEMENT lines a run
constexpr int       kMaxElemsPerModel = 64;
constexpr int       kMaxElemModels = 3;          // pistol models whose element list is logged
constexpr int       kMaxItems = 8;               // per-item step state kept (plan Task 5 step 5), the oldest dropped
constexpr float     kGripThreshold = 0.5f;       // the vr_grip block's own (ovr2xr.cpp)
constexpr int       kPulseMs = 40;
// ---- THE CROSSBOW (plan 2026-11-03-manual-reload-crossbow Task 5; spec §6a / §6b, INV-676) ----
// CrossbowVis vtable slot 58 (IsArrowRenderingEnabled, Mac name): its only caller is slot 27 +754D10, which turns the +1480 object's
// rendering on / off on its edges. DETOURED (ledger ruling R2) - true while WE show the bolt, else the game's answer. First 17 bytes
// = 4 whole position-independent instructions (push rdi; sub rsp,20h; cmp qword [rcx+0C8h],0; mov rdi,rcx - spec §6b, ruling T0-a).
constexpr uintptr_t kXbowVisRva = 0x755370;
const unsigned char kXbowVisHead[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B,
                                       0xF9 };
constexpr size_t    kXbowBoltObjOff = 1480;      // CrossbowVis + 1480 = the bolt ModelObject (the bow's BowVis + 1480 arrow, INV-402)
constexpr long      kMaxXbowLines = 200;         // [XBOW] event / draw lines a run (INV-9)
constexpr ULONGLONG kXbowPeriodMs = 5000;        // the counts line (diag_periodic_logs)

// The engine's IModelObject exports (OUR engine e9fbfc2c), by mangled name.
const char* const kExpElemId = "?GetElementID@IModelObject@@QEBAHPEBD@Z";
const char* const kExpLocal = "?GetElementLocalMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z";
const char* const kExpWorld = "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z";
const char* const kExpSetWorld = "?SetElementWorldMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z";
const char* const kExpParent = "?GetElementParent@IModelObject@@QEBAHH@Z";
const char* const kExpCount = "?GetElementsNumber@IModelObject@@QEBAHXZ";
const char* const kExpName = "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z";
const char* const kExpIsBone = "?IsElementABone@IModelObject@@QEAA_NH@Z";
const char* const kExpFlags = "?GetElementFlags@IModelObject@@QEBAIH@Z";
const char* const kExpHidden = "?IsElementHidden@IModelObject@@QEBA_NH@Z";

using ElemIdFn = int(__fastcall*)(const void* model, const char* name);
using ElemMtxFn = const float*(__fastcall*)(const void* model, int i);
using SetWorldFn = void(__fastcall*)(void* model, int i, const float* m34);
using ElemIntFn = int(__fastcall*)(const void* model, int i);
using ElemCountFn = int(__fastcall*)(const void* model);
using ElemNameFn = const char*(__fastcall*)(const void* model, int i);
using ElemBoolFn = bool(__fastcall*)(const void* model, int i);
using ElemFlagsFn = unsigned(__fastcall*)(const void* model, int i);
using CtlIntFn = int(__fastcall*)(uintptr_t ctrl);
using AmmoIdFn = const int*(__fastcall*)(uintptr_t itemClip);
using SpareFn = int(__fastcall*)(uintptr_t player, const int* ammoId, char sub);
using StartFn = void(__fastcall*)(uintptr_t ctrl, uintptr_t a2, char a3);
using AddBulletFn = void(__fastcall*)(uintptr_t ctrl, uintptr_t item);

ElemIdFn    g_elemId = nullptr;
ElemMtxFn   g_local = nullptr;
ElemMtxFn   g_world = nullptr;
SetWorldFn  g_setWorld = nullptr;
ElemIntFn   g_parent = nullptr;
ElemCountFn g_count = nullptr;
ElemNameFn  g_name = nullptr;
ElemBoolFn  g_isBone = nullptr;
ElemFlagsFn g_flags = nullptr;
ElemBoolFn  g_hidden = nullptr;
CtlIntFn    g_rounds = nullptr;
CtlIntFn    g_max = nullptr;
AmmoIdFn    g_ammoId = nullptr;
SpareFn     g_spare = nullptr;
StartFn     g_startTrue = nullptr;       // the trampolines (written by the install before the jump goes live)
AddBulletFn g_addBulletTrue = nullptr;   // = +DAF160 from +9: OUR seat call goes here (never through the hook)
using XbowVisFn = bool(__fastcall*)(uintptr_t vis);
XbowVisFn   g_xbowVisTrue = nullptr;     // the +755370 trampoline
int         g_xbowDetour = 0;            // 0 not tried, 1 live, -1 refused (the crossbow then reloads as today; the guns are unaffected)
const char* g_xbowDetourWhy = "not tried yet";
volatile uintptr_t g_xbowShowVis = 0;    // game thread writes: the CrossbowVis whose bolt WE show (carried / laid / settling); 0 = none
volatile LONG64    g_xbowShowCalls = 0;  // predicate calls answered true (the counts line)

int  g_resolved = 0;              // 0 not yet, 1 ready, -1 refused / faulted (off for the launch)
long g_nEvent = 0, g_nElem = 0;   // game thread only
int  g_elemModels = 0;
volatile LONG g_nRefuse = 0;      // detours (any thread)
volatile LONG64 g_startRefused = 0, g_addRefused = 0;

// The items classified as manual pistols (game thread writes, the detours read - a word per slot, benign races).
volatile uintptr_t g_manualItems[kMaxItems] = {};
bool IsManualItem(uintptr_t item)
{
    if (!item) { return false; }
    for (int i = 0; i < kMaxItems; ++i) {
        if (g_manualItems[i] == item) { return true; }
    }
    return false;
}

// Pad thread -> game thread.
volatile LONG g_gripDown = 0;
// Game thread -> pad thread (stamped; stale = false).
volatile LONG g_withholdR2 = 0;
volatile LONG g_ownsLeft = 0;     // a left-grip press starting now belongs to a reload step (at the mag / slide, or mid-reload)
volatile LONG64 g_publishMs = 0;
// Pad thread only.
bool g_padPressWas = false, g_padPressOwned = false;

// Per-item step state (game thread).
struct ItemState {
    uintptr_t item = 0;
    ULONGLONG lastUsedMs = 0;
    reload::Machine machine;
    bool  haveRel = false;       // the mag's pose in the hand, latched at the Grab
    float rel[12] = {};
    bool  haveMagRest = false;   // bone_magazine's rest local (identical on every model of the pistol - RL0B)
    float magRest[12] = {};
    float grabHand[3] = {};      // the hand at the Grab (Pulling's travel) / at the rack grab (Racking's travel)
    int   spare = -1;
    bool  haveBoltGrab = false;  // SMG: bone_chamber's LOCAL at the rack grab (its x = where the bolt was - BoltLocalX's floor)
    float boltGrab[12] = {};
    bool  rackSecond = false;    // the Commando's +x handle (the gun's LEFT, spec §4) was grabbed (latched once per rack grab)
    // The mag's place in the hand IS the row's from the Grab (the pull row / mag row - Jay after HP2: "In hand at the grip", no 100 ms
    // ease) and the fresh mag's from PouchTake. relRow = the row rel was set from; null = the grab's own latch (no row / no achieved
    // hand at the grab).
    const reload::HandRow* relRow = nullptr;
    // The held mag on the well's line (magrail_logic.h, Jay 2026-11-02 (4)): on from the Grab while Pulling, and from the moment a
    // fresh mag locks on at the well's open end; off once the mag is clear (or the step ends).
    reload::rail::State rail;
    // RUN MR1 (Jay: "Hand isn't locking to the mag"): on the rail the drawn hand is LOCKED to the mag, so the rail's travel is read
    // from the CONTROLLER (the rack's rule): s = railS0 + (controller - railCmd0) . out, anchored where the rail began.
    float railS0 = 0.0f;
    float railCmd0[3] = {};
    // THE CROSSBOW (crossbow plan Task 5): its own steps (xbow_logic.h - never the gun Machine above, carry ruling T2-d) and the
    // commanded hand at the rack grab (the pull is read from the controller while the lock holds the drawn hand on the rack).
    reload::XMachine xm;
    float xGrabCmd[3] = {};
    // THE PUMP SHOTGUN (pump plan Task 4): its own steps (pump_logic.h - never the gun Machine). Its chambered flag lives here, so a
    // switch keeps it per item until the next draw (freshDraw re-derives it from the round count, plan ruling P1).
    reload::PMachine pm;
    // THE DOUBLE-BARREL (2026-11-07): its own steps (dbarrel_logic.h) - which chamber holds what, the barrels' angle, the hand.
    reload::DMachine dm;
    // 2026-11-09: the Schofield / the gate revolvers / the fan-cock / the chainsaw - per item, like dm (a switch keeps them)
    reload::SMachine sm;
    reload::GMachine gm;
    reload::CMachine cm;
    reload::FMachine fm;
};
ItemState g_items[kMaxItems];

ItemState* StateFor(uintptr_t item, ULONGLONG now)
{
    int freeAt = -1, oldest = 0;
    for (int i = 0; i < kMaxItems; ++i) {
        if (g_items[i].item == item) { g_items[i].lastUsedMs = now; return &g_items[i]; }
        if (!g_items[i].item && freeAt < 0) { freeAt = i; }
        if (g_items[i].lastUsedMs < g_items[oldest].lastUsedMs) { oldest = i; }
    }
    const int at = freeAt >= 0 ? freeAt : oldest;
    g_items[at] = ItemState{};
    g_items[at].item = item;
    g_items[at].lastUsedMs = now;
    g_manualItems[at] = item;
    return &g_items[at];
}
// A crossbow refused this draw (crossbow plan Task 5: no bolt object / no detour / a missing row): an item registered by an earlier
// draw is dropped from the two detours' list, so the game reloads it as today. COST: kMaxItems compares, once per model change.
void ForgetManualItem(uintptr_t item)
{
    if (!item) { return; }
    for (int i = 0; i < kMaxItems; ++i) {
        if (g_items[i].item == item) { g_items[i] = ItemState{}; }
        if (g_manualItems[i] == item) { g_manualItems[i] = 0; }
    }
}

// The held model, classified once per model pointer.
struct Gun {
    uintptr_t model = 0;
    uintptr_t modelInv = 0;                         // model's InventoryItem at the classification - a reused address is another item
    bool      built = false;                        // a built family (Pistol / Smg) - else the game reloads it
    reload::Family        family = reload::Family::None;
    const reload::GunRow* row = nullptr;            // Smg: Jay's spots + the bolt travel; null for the pistol (its joints)
    const reload::HandRow* rackRow = nullptr;       // the left hand LOCKED on the slide / bolt (pistol by mesh, SMG by prefix)
    const reload::HandRow* rackRow2 = nullptr;      // the Commando's +x handle pose (gun's LEFT; "<mesh>+x"); null elsewhere
    const char* magBone = "bone_magazine";          // the mag bone resolved at the classification: the row's (rifles bone_clip), else
                                                    // bone_magazine (pistol / no row) - resolved once per draw, never per update
    const reload::HandRow* magRow = nullptr;        // the left hand's mag hold (by mesh); null = today's open hand
    const reload::HandRow* pullRow = nullptr;       // PISTOL: the pull-out hand (by mesh) for Pulling / MagInHand; null = magRow
    const reload::FxRow*   fx = nullptr;            // racks + the game's reload sounds (reload_fx.h, by mesh); null = no rack, no sound
    bool      haveClear = false;                    // the well rail: this mesh's mag clear distance (gunshape.h GunShapeMagClearM)
    float     clearM = 0.0f;                        //   false = no well row -> the mag is free in the hand from the grab, as before
    uintptr_t item = 0;
    int       magId = -1, chamberId = -1, magParent = -1, chamberParent = -1;
    bool      haveRest = false;      // the chamber's rest (logged only) - the mag's lives per item
    float     restChamber[12] = {};
    float     lastMag[12] = {}, lastChamber[12] = {};
    bool      haveLast = false;
    ULONGLONG stillSince = 0;
    ULONGLONG lastStateMs = 0;
    bool      saidFirstState = false;
};
Gun g_gun;
// Game thread only (ReloadTick and the tail solve run on it): the PlayerDI of this tick (the sound call's object) and the held mag's
// pose in the left handholder frame (solid guns plan Task 5).
uintptr_t g_curPlayer = 0;
// RUN SG3: the held-mag stop now runs on the PAD-POLL thread too, so the mag-in-hand pose is PUBLISHED under a lock: the game thread
// stages it (g_stage*) during ReloadTick and a scope guard publishes it once, on every return path (no half-written or momentarily
// cleared state); ReloadMagInHand reads the published copy under the shared lock.
bool      g_stageOn = false;
float     g_stageRel[12] = {};
float     g_stageDrawn[12] = {};
SRWLOCK   g_magLock = SRWLOCK_INIT;
bool      g_magRelOn = false;      // published (g_magLock)
float     g_magRel[12] = {};       // published
float     g_magDrawn[12] = {};     // published: the mag's world as drawn this tick (review finding 3: the solve re-expresses it)
struct MagPublish {
    ~MagPublish()
    {
        AcquireSRWLockExclusive(&g_magLock);
        g_magRelOn = g_stageOn;
        if (g_stageOn) {
            memcpy(g_magRel, g_stageRel, sizeof(g_magRel));
            memcpy(g_magDrawn, g_stageDrawn, sizeof(g_magDrawn));
        }
        ReleaseSRWLockExclusive(&g_magLock);
    }
};

bool EventLine() { return ++g_nEvent <= kMaxEventLines; }
bool RefuseLine() { return InterlockedIncrement(&g_nRefuse) <= kMaxRefuseLines; }

// ---- SEH-guarded reads and calls (no C++ objects with destructors in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadInt(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HeadMatches(uintptr_t a, const unsigned char* want, size_t n)
{
    __try { return memcmp(reinterpret_cast<const void*>(a), want, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// obj's vfunc at byte offset `slot`, no argument / one u32 argument. Returns the full rax.
bool CallV0(uintptr_t obj, size_t slot, uintptr_t* out)
{
    using Fn = uintptr_t(__fastcall*)(uintptr_t);
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        *out = (*reinterpret_cast<Fn const*>(vt + slot))(obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallV1(uintptr_t obj, size_t slot, unsigned a, uintptr_t* out)
{
    using Fn = uintptr_t(__fastcall*)(uintptr_t, unsigned);
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        *out = (*reinterpret_cast<Fn const*>(vt + slot))(obj, a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// F1 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): in co-op this machine also runs the OTHER player's
// WeaponFireController (INV-594), so the two refusals below must act for the LOCAL player's controller only. The controller's
// player (vfunc +32, `return *(this+8)`) IS the game's local PlayerDI when it is ours (INV-594, measured equal on both machines).
// Unreadable chain or vcall -> true = today's behaviour (single player is never changed). COST: one guarded vcall + 4 guarded reads,
// only for a manual gun's call.
volatile LONG64 g_startRemote = 0, g_addRemote = 0;
volatile LONG   g_saidRemote = 0;
bool CtlIsLocal(uintptr_t ctrl)
{
    int why = -1;
    const uintptr_t local = RigGamePlayerObject(&why);
    if (why != 0 || !local) { return true; }
    uintptr_t player = 0;
    if (!CallV0(ctrl, kCtlPlayerVfn, &player) || !player) { return true; }
    return player == local;
}
void SayRemote(const char* what, uintptr_t ctrl, uintptr_t item)
{
    if (InterlockedExchange(&g_saidRemote, 1) == 0) {
        // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [RELOAD] %s of a REMOTE player's controller %p item %p - passed to the game (co-op: only the local player's "
            "manual gun is refused; logged once)", what, reinterpret_cast<void*>(ctrl), reinterpret_cast<void*>(item));
    }
}

// The controller's selected InventoryItem (controller vfunc +32 -> player vfunc +1944(ctrl + 344)). 0 = none / fault.
uintptr_t SelItemOf(uintptr_t ctrl)
{
    int slot = -1;
    uintptr_t player = 0, item = 0;
    if (!ctrl || !ReadInt(ctrl + kCtlSlotOff, &slot) || slot < 0 || !CallV0(ctrl, kCtlPlayerVfn, &player) || !player ||
        !CallV1(player, kSelItemVfn, static_cast<unsigned>(slot), &item)) {
        return 0;
    }
    return item;
}
// The anim prefix: desc vfunc +1624 -> { char* +0, u32 +8 }. Copied into out (NUL-terminated, truncated to n - 1).
bool ReadPrefix(uintptr_t desc, char* out, size_t n)
{
    out[0] = 0;
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(desc);
        using Fn = uintptr_t(__fastcall*)(uintptr_t);
        const uintptr_t s = (*reinterpret_cast<Fn const*>(vt + kDescPrefixVfn))(desc);
        if (!s) { return false; }
        const char* p = *reinterpret_cast<const char* const*>(s);
        unsigned len = *reinterpret_cast<const unsigned*>(s + 8);
        if (!p && len) { return false; }
        if (len > n - 1) { len = static_cast<unsigned>(n - 1); }
        for (unsigned i = 0; i < len; ++i) { out[i] = p[i]; }
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
bool ElemId(uintptr_t model, const char* name, int* out)
{
    __try { *out = g_elemId(reinterpret_cast<const void*>(model), name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = -1; return false; }
}
bool ElemMtx(ElemMtxFn fn, uintptr_t model, int i, float out12[12])
{
    if (i < 0) { return false; }   // rifle plan Task 5: the Railgun has no bone_chamber (id -1) - never index the engine with it
    __try {
        const float* m = fn(reinterpret_cast<const void*>(model), i);
        if (!m) { return false; }
        for (int k = 0; k < 12; ++k) { out12[k] = m[k]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SetWorld(uintptr_t model, int i, const float m12[12])
{
    if (i < 0) { return false; }   // as ElemMtx: an absent element is never written
    __try { g_setWorld(reinterpret_cast<void*>(model), i, m12); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ElemParent(uintptr_t model, int i, int* out)
{
    if (i < 0) { *out = -1; return false; }   // as ElemMtx: an absent element has no parent
    __try { *out = g_parent(reinterpret_cast<const void*>(model), i); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = -1; return false; }
}
// One element's structural facts for the ELEMENT list (G5: RL0B - no separate magazine mesh).
bool ElemFacts(uintptr_t model, int i, const char** name, int* parent, int* bone, unsigned* flags, int* hidden)
{
    __try {
        const void* m = reinterpret_cast<const void*>(model);
        *name = g_name(m, i);
        *parent = g_parent(m, i);
        *bone = g_isBone(m, i) ? 1 : 0;
        *flags = g_flags(m, i);
        *hidden = g_hidden(m, i) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ElemCount(uintptr_t model, int* out)
{
    __try { *out = g_count(reinterpret_cast<const void*>(model)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}
bool CallCtlInt(CtlIntFn fn, uintptr_t ctrl, int* out)
{
    __try { *out = fn(ctrl); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSpare(uintptr_t player, uintptr_t item, int* out)
{
    __try {
        const int* id = g_ammoId(item + kItemClipOff);
        if (!id) { return false; }
        *out = g_spare(player, id, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallAddBullet(uintptr_t ctrl, uintptr_t item)
{
    __try { g_addBulletTrue(ctrl, item); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The double-barrel's ONE-round add (INV-699): a byte of the fire controller, read / written guarded (game thread only).
bool ReadU8(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const volatile unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteU8(uintptr_t a, unsigned char v)
{
    __try { *reinterpret_cast<volatile unsigned char*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int SpareOf(uintptr_t ctrl, uintptr_t item)
{
    uintptr_t player = 0;
    int spare = -1;
    if (CallV0(ctrl, kCtlPlayerVfn, &player) && player) { CallSpare(player, item, &spare); }
    return spare;
}

// ---- the two detours ----
// [LOADBEARING: manual reload start refusal] (manual_reload) X, the auto-reload on an empty clip and every other caller of the game's
//      reload start do nothing for a classified pistol - else the game's timed reload runs under the hands (spec goals 5 / 6 / 9).
//      COST per call: one bool test off; on, two guarded vfunc calls + a list compare (the auto-reload calls it every update while
//      the clip is 0 - no log past the capped lines).
void __fastcall StartHook(uintptr_t ctrl, uintptr_t a2, char a3)
{
    if (ManualReload) {
        const uintptr_t item = SelItemOf(ctrl);
        if (IsManualItem(item) && !CtlIsLocal(ctrl)) {
            InterlockedIncrement64(&g_startRemote);
            SayRemote("START", ctrl, item);
        } else if (IsManualItem(item)) {
            InterlockedIncrement64(&g_startRefused);
            if (RefuseLine()) {
                // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=no
                LOG("reload: [RELOAD] START REFUSED ctl %p item %p caller %p (the game's reload of a manual gun; x%lld so far)",
                    reinterpret_cast<void*>(ctrl), reinterpret_cast<void*>(item), _ReturnAddress(),
                    static_cast<long long>(g_startRefused));
            }
            return;
        }
    }
    g_startTrue(ctrl, a2, a3);
}
// [LOADBEARING: manual reload add-bullet refusal] (manual_reload) a classified pistol gets rounds ONLY from our seat / rack call - the
//      inventory paths +BD76D0 / +BD1A10 fill an EMPTY gun on a slot change (Task 1 G3), which would refill a pulled-empty pistol on a
//      weapon switch (spec goal 7). COST per call: one bool test off; on, a list compare.
void __fastcall AddBulletHook(uintptr_t ctrl, uintptr_t item)
{
    if (ManualReload && IsManualItem(item) && !CtlIsLocal(ctrl)) {
        InterlockedIncrement64(&g_addRemote);
        SayRemote("ADD-BULLET", ctrl, item);
    } else if (ManualReload && IsManualItem(item)) {
        InterlockedIncrement64(&g_addRefused);
        if (RefuseLine()) {
            // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [RELOAD] ADD-BULLET REFUSED ctl %p item %p caller %p (not our seat - a manual gun is filled by hand; x%lld)",
                reinterpret_cast<void*>(ctrl), reinterpret_cast<void*>(item), _ReturnAddress(), static_cast<long long>(g_addRefused));
        }
        return;
    }
    g_addBulletTrue(ctrl, item);
}
// [LOADBEARING: manual reload crossbow bolt visibility] (manual_reload) the crossbow's predicate +755370 hides the bolt every update
//      while the clip is 0 (INV-676) - without this the bolt carried in the fist and laid on the groove is invisible. True for the
//      CrossbowVis the tick publishes (carry / laid / the settle after the seat), else the game's own answer.
//      COST per call (~2 per animation update, crossbow only - its one caller is CrossbowVis slot 27): one bool test + one volatile
//      load + a compare; + one interlocked increment when it answers true.
bool __fastcall XbowVisHook(uintptr_t vis)
{
    if (ManualReload && vis && vis == g_xbowShowVis) {
        InterlockedIncrement64(&g_xbowShowCalls);
        return true;
    }
    return g_xbowVisTrue(vis);
}

void Refuse(const char* what)
{
    g_resolved = -1;
    // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
    LOG("reload: [RELOAD] REFUSED - %s; manual reload is OFF this launch (X and the auto-reload stay the game's).", what);
}

bool Resolve()
{
    if (g_resolved != 0) { return g_resolved > 0; }
    HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!en || !gd) { Refuse(!en ? "engine_x64_rwdi.dll is not loaded" : "gamedll_x64_rwdi.dll is not loaded"); return false; }
    g_elemId = reinterpret_cast<ElemIdFn>(GetProcAddress(en, kExpElemId));
    g_local = reinterpret_cast<ElemMtxFn>(GetProcAddress(en, kExpLocal));
    g_world = reinterpret_cast<ElemMtxFn>(GetProcAddress(en, kExpWorld));
    g_setWorld = reinterpret_cast<SetWorldFn>(GetProcAddress(en, kExpSetWorld));
    g_parent = reinterpret_cast<ElemIntFn>(GetProcAddress(en, kExpParent));
    g_count = reinterpret_cast<ElemCountFn>(GetProcAddress(en, kExpCount));
    g_name = reinterpret_cast<ElemNameFn>(GetProcAddress(en, kExpName));
    g_isBone = reinterpret_cast<ElemBoolFn>(GetProcAddress(en, kExpIsBone));
    g_flags = reinterpret_cast<ElemFlagsFn>(GetProcAddress(en, kExpFlags));
    g_hidden = reinterpret_cast<ElemBoolFn>(GetProcAddress(en, kExpHidden));
    const char* missing = !g_elemId ? kExpElemId : !g_local ? kExpLocal : !g_world ? kExpWorld : !g_setWorld ? kExpSetWorld
                        : !g_parent ? kExpParent : !g_count ? kExpCount : !g_name ? kExpName : !g_isBone ? kExpIsBone
                        : !g_flags ? kExpFlags : !g_hidden ? kExpHidden : nullptr;
    if (missing) {
        char why[256];
        wsprintfA(why, "the engine does not export %s", missing);
        Refuse(why);
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gd);
    struct Site { uintptr_t rva; const unsigned char* head; size_t n; const char* name; uintptr_t* out; };
    uintptr_t aRounds = 0, aMax = 0, aAmmo = 0, aSpare = 0, aEntry = 0;
    const Site sites[] = {
        { kRoundsRva, kRoundsHead, sizeof(kRoundsHead), "+DAF580 (rounds in clip)", &aRounds },
        { kMaxRva, kRoundsHead, sizeof(kRoundsHead), "+DAF5C0 (clip max)", &aMax },
        { kAmmoIdRva, kAmmoIdHead, sizeof(kAmmoIdHead), "+6F2F50 (item ammo id)", &aAmmo },
        { kSpareRva, kSpareHead, sizeof(kSpareHead), "+BD0270 (spare ammo)", &aSpare },
        { kAddBulletBodyRva - 9, kAddBulletEntry, sizeof(kAddBulletEntry), "+DAF160 (add-bullet entry)", &aEntry },
    };
    for (const Site& s : sites) {
        const uintptr_t a = base + DLVR_RELOAD_GD_RVA(s.rva);
        if (!HeadMatches(a, s.head, s.n)) {
            char why[256];
            wsprintfA(why, "gamedll %s at +%X: its first bytes do not match the Steam 97046bba / Epic build this was read from", s.name,
                      static_cast<unsigned>(a - base));
            Refuse(why);
            return false;
        }
        *s.out = a;
    }
    g_rounds = reinterpret_cast<CtlIntFn>(aRounds);
    g_max = reinterpret_cast<CtlIntFn>(aMax);
    g_ammoId = reinterpret_cast<AmmoIdFn>(aAmmo);
    g_spare = reinterpret_cast<SpareFn>(aSpare);
    // The two detours (Steam addresses - InstallSingleGameDetour moves an allowlisted one on the Epic build and checks the bytes).
    int attS = 0, attA = 0;
    if (!InstallSingleGameDetour(base + kStartRva, kStartHead, sizeof(kStartHead), reinterpret_cast<void*>(&StartHook),
                                 reinterpret_cast<void**>(&g_startTrue), "reload start", &attS)) {
        Refuse("the reload-start detour (+DA6E00) was not installed - its first 16 bytes differ, or the write was refused");
        return false;
    }
    if (!InstallSingleGameDetour(base + kAddBulletBodyRva, kAddBulletBody, sizeof(kAddBulletBody), reinterpret_cast<void*>(&AddBulletHook),
                                 reinterpret_cast<void**>(&g_addBulletTrue), "reload add-bullet", &attA)) {
        // The start detour stays installed; with g_resolved = -1 nothing is classified, so it always calls through.
        Refuse("the add-bullet detour (+DAF169) was not installed - its 14 bytes differ, or the write was refused");
        return false;
    }
    // THE CROSSBOW's bolt-visibility detour (crossbow plan Task 5, ruling R2): its failure turns only the crossbow off (named), never the
    // guns. The Epic gamedll is refused BY NAME before any byte is compared: +755370 is not on the EGL allowlist (ruling T0-a).
    int attX = 0;
    if (DLVR_RELOAD_GD_IS_EGL()) {
        g_xbowDetourWhy = "the Epic (EGL) gamedll is loaded and +755370 is not on tools/egl_allowlist.tsv - no address is guessed";
    } else if (!InstallSingleGameDetour(base + kXbowVisRva, kXbowVisHead, sizeof(kXbowVisHead), reinterpret_cast<void*>(&XbowVisHook),
                                        reinterpret_cast<void**>(&g_xbowVisTrue), "reload crossbow bolt visibility", &attX)) {
        g_xbowDetourWhy = "the bolt-visibility detour (+755370) was not installed - its first 17 bytes differ, or the write was refused";
    } else {
        g_xbowDetourWhy = nullptr;
    }
    g_xbowDetour = g_xbowDetourWhy ? -1 : 1;
    if (g_xbowDetour > 0) {
        // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=no
        LOG("reload: [XBOW] DETOUR LIVE - CrossbowVis bolt predicate +755370 (17 bytes, %d attempt(s)): the bolt shows while the left hand "
            "carries / lays it and through the seat's settle.", attX);
    } else {
        // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [XBOW] REFUSED - %s; the CROSSBOW reloads as today this launch (the pistol / SMG / rifle reloads are unaffected).",
            g_xbowDetourWhy);
    }
    // THE PUMP SHOTGUN's rack-sound silencer (pump plan Task 3): best-effort, a refusal logs its own line and leaves the game's sound.
    PumpSoundInstall();
    g_resolved = 1;
    // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=no
    LOG("reload: [RELOAD] READY - 10 engine element exports, 4 gamedll functions (+DAF580 +DAF5C0 +6F2F50 +BD0270 at +%X +%X +%X +%X) "
        "checked; detours LIVE: reload start +DA6E00 (%d attempt(s)), add-bullet +DAF169 (%d). Steps: snap %.3f m, align %.0f deg, "
        "pull-out %.3f m, rack %.3f m, grab %.3f m; let-go mag shrunk to x%.3f.",
        static_cast<unsigned>(aRounds - base), static_cast<unsigned>(aMax - base), static_cast<unsigned>(aAmmo - base),
        static_cast<unsigned>(aSpare - base), attS, attA, ReloadSnapM, ReloadAlignDeg, ReloadPullOutM, ReloadRackPullM, ReloadGrabM,
        kHideScale);
    return true;
}

// ---- small maths (row-major 3x4, basis = columns, t = [3] [7] [11]) ----
float Len3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }
// out = a x b (both 3x4 affine)
void Mul34(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c];
            if (c == 3) { v += a[4 * r + 3]; }
            out[4 * r + c] = v;
        }
    }
}
// out = m x v (a point in m's local frame -> m's space). A zero v = m's origin (the pistol's joints).
void XformPt(const float* m, const reload::Vec3& v, float out[3])
{
    for (int r = 0; r < 3; ++r) { out[r] = m[4 * r] * v.x + m[4 * r + 1] * v.y + m[4 * r + 2] * v.z + m[4 * r + 3]; }
}
// The hand's 3x4 from a position + a row-major 3x3 (axes as columns), each column normalised. False = degenerate.
bool Hand34(const float p[3], const float r9[9], float out[12])
{
    for (int c = 0; c < 3; ++c) {
        const float l = Len3(r9[c], r9[3 + c], r9[6 + c]);
        if (!(l > 1e-6f) || !std::isfinite(l)) { return false; }
        for (int r = 0; r < 3; ++r) { out[4 * r + c] = r9[3 * r + c] / l; }
    }
    for (int r = 0; r < 3; ++r) { out[4 * r + 3] = p[r]; }
    return true;
}
// The inverse of a rigid 3x4 (rotation columns orthonormal).
void RigidInv(const float* m, float* out)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out[4 * r + c] = m[4 * c + r]; }
    }
    for (int r = 0; r < 3; ++r) {
        out[4 * r + 3] = -(out[4 * r] * m[3] + out[4 * r + 1] * m[7] + out[4 * r + 2] * m[11]);
    }
}
// The angle between two 3x4s' rotations, each column normalised first (flat worlds are not unit - INV-11). -1 = degenerate.
float RotAngleDeg(const float* a, const float* b)
{
    float tr = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float la = Len3(a[c], a[4 + c], a[8 + c]);
        const float lb = Len3(b[c], b[4 + c], b[8 + c]);
        if (la < 1e-6f || lb < 1e-6f) { return -1.0f; }
        tr += (a[c] * b[c] + a[4 + c] * b[4 + c] + a[8 + c] * b[8 + c]) / (la * lb);
    }
    float ct = (tr - 1.0f) * 0.5f;
    if (ct > 1.0f) { ct = 1.0f; }
    if (ct < -1.0f) { ct = -1.0f; }
    return std::acos(ct) * 57.2957795f;
}
bool SameMtx(const float* a, const float* b)
{
    for (int k = 0; k < 12; ++k) {
        if (std::fabs(a[k] - b[k]) > kStillEps) { return false; }
    }
    return true;
}
// A column of a 3x4, normalised. False = degenerate.
bool Axis(const float* m, int c, float out[3])
{
    const float l = Len3(m[c], m[4 + c], m[8 + c]);
    if (!(l > 1e-6f)) { return false; }
    out[0] = m[c] / l; out[1] = m[4 + c] / l; out[2] = m[8 + c] / l;
    return true;
}
// Final fix wave (C1 / C2): the unit direction of m's rotation x v (v in m's local frame - a row's axis in its bone's PARENT frame).
// Normalised exactly as Axis(): with v a basis vector (the SMG rows' (0 0 1) / (1 0 0)) the products are column c bit for bit (x*1
// and + 0 are exact), so the SMG's rail / rack directions are unchanged. False = degenerate (a zero axis - the Railgun's rack).
// A world direction in m's axes (each column normalised - bone_root may carry scale). False = degenerate. COST: 9 mul-adds + 3 sqrt.
bool DirToLocal(const float* m, const float w[3], float out[3])
{
    for (int c = 0; c < 3; ++c) {
        const float l = Len3(m[c], m[4 + c], m[8 + c]);
        if (!(l > 1e-6f)) { return false; }
        out[c] = (m[c] * w[0] + m[4 + c] * w[1] + m[8 + c] * w[2]) / l;
    }
    return true;
}

bool AxisOf(const float* m, const reload::Vec3& v, float out[3])
{
    const float a = m[0] * v.x + m[1] * v.y + m[2] * v.z;
    const float b = m[4] * v.x + m[5] * v.y + m[6] * v.z;
    const float c = m[8] * v.x + m[9] * v.y + m[10] * v.z;
    const float l = Len3(a, b, c);
    if (!(l > 1e-6f)) { return false; }
    out[0] = a / l; out[1] = b / l; out[2] = c / l;
    return true;
}

// ---- the LEFT hand's poses (plan docs/superpowers/plans/2026-11-01-reload-hand-poses.md Task 3) ----
// Published by the tail (game thread) once per update while a built gun is held AND a pose is on or still blending out; read by
// the left arm's solve (ReloadPinRot / ReloadPinPos - the tick thread and the tail solve) and the finger-pin pass
// (ReloadFingerPose). Seqlock: odd = writing. Stale after 150 ms (the bow's guard): a gun put away or a tail that stopped - the
// readers fall back to today's hand at once.
//   part   = bone_chamber's world as drawn this update (the SMG's bolt as WE draw it during the rack)
//   rack   = the row the lock uses (kept through the blend-out); lockOn / lockTick = the lock's blend clock
//   finger = the row the fingers use (rack or mag, kept through the blend-out); fingerOn / fingerTick = its clock
volatile LONG          g_hpSeq = 0;
volatile LONG          g_hpAny = 0;   // 1 while anything is on or blending out - the per-pass readers' one-load gate
float                  g_hpPart[12] = {};
const reload::HandRow* g_hpRack = nullptr;
const reload::HandRow* g_hpFinger = nullptr;
LONG                   g_hpLockOn = 0, g_hpFingerOn = 0;
LONG64                 g_hpLockTick = 0, g_hpFingerTick = 0, g_hpStamp = 0;
// The CONSUMERS' counts since the last [RELOAD] STATE line (read and zeroed there): the solve's lock applications (rotation /
// position), the finger pass's passes and pins written, and finger reads whose row was not in this file's tables (must stay 0).
volatile LONG          g_cntLockRot = 0, g_cntLockPos = 0, g_cntFingerPasses = 0, g_cntFingerPins = 0, g_cntFingerNoRow = 0;

struct HpFrame {
    float part[12];
    const reload::HandRow* rack;
    const reload::HandRow* finger;
    LONG   lockOn, fingerOn;
    LONG64 lockTick, fingerTick;
};
bool HpRead(HpFrame* f, LONG64 now)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s0 = g_hpSeq;
        if (s0 & 1) { continue; }
        MemoryBarrier();
        for (int k = 0; k < 12; ++k) { f->part[k] = g_hpPart[k]; }
        f->rack = g_hpRack;
        f->finger = g_hpFinger;
        f->lockOn = g_hpLockOn;
        f->fingerOn = g_hpFingerOn;
        f->lockTick = g_hpLockTick;
        f->fingerTick = g_hpFingerTick;
        const LONG64 stamp = g_hpStamp;
        MemoryBarrier();
        if (g_hpSeq == s0) { return stamp != 0 && now - stamp <= 150; }
    }
    return false;
}

// Game thread: a blend clock that never jumps - a switch mid-blend carries on from where the blend IS (the tick is moved back by
// the share already covered).
struct BlendClock { bool on = false; bool have = false; ULONGLONG tick = 0; };
float ClockT(const BlendClock& c, ULONGLONG now)
{
    return c.have ? reload::HandBlendT(c.on, static_cast<long long>(now - c.tick)) : 0.0f;
}
void ClockSet(BlendClock& c, bool on, ULONGLONG now, bool eases = true)
{
    if (c.have && c.on == on) { return; }
    if (!eases) {   // switch at once: the reader's HandBlendT(on, >= kHandBlendMs) is 1 (on) / 0 (off)
        c.tick = now - static_cast<ULONGLONG>(reload::kHandBlendMs);
        c.on = on;
        c.have = true;
        return;
    }
    const float cur = ClockT(c, now);
    const float covered = on ? cur : 1.0f - cur;
    c.tick = now - static_cast<ULONGLONG>(covered * static_cast<float>(reload::kHandBlendMs) + 0.5f);
    c.on = on;
    c.have = true;
}
BlendClock             g_lockClock, g_fingerClock;   // game thread
const reload::HandRow* g_lockRow = nullptr;          // game thread: the last row each clock served (kept for the blend-out)
const reload::HandRow* g_fingerRow = nullptr;
float                  g_partLast[12] = {};
bool                   g_havePart = false;
bool                   g_lockIsMag = false;          // game thread: the last lock was the MAG on the well rail (RUN MR1), not the rack
long                   g_nHandsLines = 0;

// Game thread, once per DriveSteps. lockOn = Racking on a gun with a rack row and a part read; fingerRow = the active pose's row
// (null = no pose); fingerEases = that pose blends (reload::PoseEases: the rack does, the mag hold switches at once - Jay after HP2).
// The off switch follows the pose that was on. A row change while the fingers are on carries the clock over (the pull row -> the
// mag row passes through Empty, no pose). COST: a few compares; the seqlock write (~20 words) only while something is on or blending
// out.
bool g_fingerEases = true;   // game thread: the last ON pose's (for its off switch)
void HandsPublish(bool lockOn, const reload::HandRow* rackRow, const float* part12, const reload::HandRow* fingerRow,
                  bool fingerEases, ULONGLONG now)
{
    if (part12) { memcpy(g_partLast, part12, sizeof(g_partLast)); g_havePart = true; }
    if (lockOn && rackRow) { g_lockRow = rackRow; }
    ClockSet(g_lockClock, lockOn && rackRow && g_havePart, now);
    if (fingerRow) { g_fingerRow = fingerRow; g_fingerEases = fingerEases; }
    ClockSet(g_fingerClock, fingerRow != nullptr, now, g_fingerEases);
    const bool any = g_lockClock.on || ClockT(g_lockClock, now) > 0.0f || g_fingerClock.on || ClockT(g_fingerClock, now) > 0.0f;
    if (!any && !g_hpAny) { return; }
    InterlockedIncrement(&g_hpSeq);   // odd = writing
    MemoryBarrier();
    for (int k = 0; k < 12; ++k) { g_hpPart[k] = g_partLast[k]; }
    g_hpRack = g_havePart ? g_lockRow : nullptr;
    g_hpFinger = g_fingerRow;
    g_hpLockOn = g_lockClock.on ? 1 : 0;
    g_hpFingerOn = g_fingerClock.on ? 1 : 0;
    g_hpLockTick = static_cast<LONG64>(g_lockClock.tick);
    g_hpFingerTick = static_cast<LONG64>(g_fingerClock.tick);
    g_hpStamp = static_cast<LONG64>(now);
    MemoryBarrier();
    InterlockedIncrement(&g_hpSeq);   // even = stable
    InterlockedExchange(&g_hpAny, any ? 1 : 0);
}

// Game thread: the held model changed or went away - the poses end at once (no blend-out onto another gun, no stale clock left
// "on" for when this gun comes back). COST: one bool test unless something was on.
void HandsReset()
{
    if (!g_lockClock.have && !g_fingerClock.have && !g_hpAny) { return; }
    g_lockClock = BlendClock{};
    g_fingerClock = BlendClock{};
    g_lockRow = nullptr;
    g_fingerRow = nullptr;
    g_fingerEases = true;
    g_havePart = false;
    g_lockIsMag = false;
    InterlockedExchange(&g_hpAny, 0);
}

// The lock's blend 0..1 and the frame it reads. False = off / stale / nothing to lock to. COST: off = one bool test + one volatile
// load; on = one seqlock read (~20 words) + GetTickCount64.
bool LockT(HpFrame* f, float* t)
{
    if (!ManualReload || !g_hpAny) { return false; }
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    if (!HpRead(f, now) || !f->rack) { return false; }
    const float tt = reload::HandBlendT(f->lockOn != 0, static_cast<long long>(now - f->lockTick));
    if (!(tt > 0.0f)) { return false; }
    *t = tt;
    return true;
}

// The mag's place in the hand for the gun's MAG ROW. H (DriveSteps) = the DRAWN hand basis at the ACHIEVED handholder, and the
// handholder sits at holderT in the hand bone (boneblend.cpp g_handHolderLocal), so the hand bone = H x [I | -holderT]. Jay's row
// is that hand bone in bone_magazine's REST frame, so the mag = hand bone x inv(row): rel = [I | -holderT] x inv(row). The basis
// columns are scaled to the seat's (the game may draw the mag with scale - INV-11). False = a degenerate row / seat.
bool MagRowRel(const reload::HandRow& row, const float holderT[3], const float seat[12], float out[12])
{
    float R[9];
    if (!twohand::Ortho3(row.handR, R)) { return false; }
    float m[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { m[4 * r + c] = R[3 * c + r]; }   // R^T
    }
    for (int r = 0; r < 3; ++r) {
        m[4 * r + 3] = -(m[4 * r] * row.handT[0] + m[4 * r + 1] * row.handT[1] + m[4 * r + 2] * row.handT[2]) - holderT[r];
    }
    for (int c = 0; c < 3; ++c) {
        const float l = Len3(seat[c], seat[4 + c], seat[8 + c]);
        if (!(l > 1e-6f) || !std::isfinite(l)) { return false; }
        for (int r = 0; r < 3; ++r) { m[4 * r + c] *= l; }
    }
    for (int k = 0; k < 12; ++k) { if (!std::isfinite(m[k])) { return false; } }
    memcpy(out, m, sizeof(m));
    return true;
}

// The mag's place in the hand = the row's, AT ONCE (Jay after HP2: no 100 ms ease). Only with the ACHIEVED hand (H is then the drawn
// hand bone's basis at the drawn handholder - MagRowRel); the commanded fallback's basis is the controller's, so the caller keeps the
// grab's own latch. False = not set (no row / no achieved hand / no seat / a degenerate row) - rel untouched.
bool SetRelRow(ItemState* st, const reload::HandRow* row, bool achieved, const float* holderT, bool haveSeat, const float seat[12])
{
    if (!row || !achieved || !holderT || !haveSeat) { return false; }
    float to[12];
    if (!MagRowRel(*row, holderT, seat, to)) { return false; }
    memcpy(st->rel, to, sizeof(st->rel));
    st->haveRel = true;
    st->relRow = row;
    return true;
}

const char* StepName(reload::Step s)
{
    switch (s) {
    case reload::Step::Idle: return "Idle";
    case reload::Step::Pulling: return "Pulling";
    case reload::Step::MagInHand: return "MagInHand";
    case reload::Step::Empty: return "Empty";
    case reload::Step::FreshInHand: return "FreshInHand";
    case reload::Step::NeedsRack: return "NeedsRack";
    case reload::Step::Racking: return "Racking";
    }
    return "?";
}
const char* FamilyName(reload::Family f)
{
    switch (f) {
    case reload::Family::None: return "None";
    case reload::Family::Pistol: return "Pistol";
    case reload::Family::Smg: return "Smg";
    case reload::Family::Rifle: return "Rifle";
    case reload::Family::Crossbow: return "Crossbow";
    case reload::Family::Pump: return "Pump";
    case reload::Family::DBarrel: return "DBarrel";
    case reload::Family::Break6: return "Break6";
    case reload::Family::Gate: return "Gate";
    case reload::Family::Fuel: return "Fuel";
    }
    return "?";
}
const char* EventName(reload::Event e)
{
    switch (e) {
    case reload::Event::None: return "None";
    case reload::Event::Grab: return "Grab";
    case reload::Event::PullOut: return "PullOut";
    case reload::Event::Drop: return "Drop";
    case reload::Event::PouchTake: return "PouchTake";
    case reload::Event::PouchEmpty: return "PouchEmpty";
    case reload::Event::SnapBack: return "SnapBack";
    case reload::Event::Seat: return "Seat";
    case reload::Event::RackPull: return "RackPull";
    case reload::Event::RackDone: return "RackDone";
    case reload::Event::Done: return "Done";
    }
    return "?";
}

void Publish(bool withholdR2, bool ownsLeft)
{
    InterlockedExchange(&g_withholdR2, withholdR2 ? 1 : 0);
    InterlockedExchange(&g_ownsLeft, ownsLeft ? 1 : 0);
    InterlockedExchange64(&g_publishMs, static_cast<LONG64>(GetTickCount64()));
}

// ===== THE CROSSBOW (plan docs/superpowers/plans/2026-11-03-manual-reload-crossbow.md Task 5) =====
// Jay's order (spec §1 "ORDER CHANGED"): the EMPTY left hand pulls the rack (bone_stringreload) back to the catch, then takes a bolt over
// the LEFT shoulder, carries it in the fist, lays it in the groove and slides it to the seat. The steps are xbow_logic.h (XMachine,
// unit-tested - never the gun Machine, carry ruling T2-d); this draws the parts, carries the bolt and calls the game. Frames (ledger
// ruling P1): every XbowRow vector is in bone_root's REST LOCAL frame except rackSpot; the three parts are children of bone_root
// (checked once per draw), so a part's world = bone_root's live world x its local.
struct XbowDraw {
    const reload::XbowRow* row = nullptr;
    const reload::HandRow* rackRow = nullptr;    // Jay's rack grab (by mesh): the lock and the fingers while Racking
    const reload::HandRow* carryRow = nullptr;   // Jay's bolt fist: the fingers while Carry / Laid (no lock)
    int       rootId = -1, stringId = -1, rackId = -1, boltId = -1;
    uintptr_t boltObj = 0;                       // CrossbowVis + 1480, read ONCE per draw
    float     rackDoneM = 0.0f;                  // ledger ruling P2: rack travel x reload_rack_pull_m / the pistol slide's travel
    bool      fresh = false;                     // the first update of this draw (ruling R4 - XInput::freshDraw)
    float     lastAlong = 0.0f;                  // the laid bolt's last place along the groove (held through an update with no hand)
    // The three parts' REST locals are the ROW's (XbowRow::stringRest / rackRest / boltRest - mesh data, fix round 1 ruling T5-a): never
    // a live read, which would measure our own leftover write after a re-classification (INV-672).
};
XbowDraw    g_xb;                       // game thread
// Fix round 1 (ruling T5-b): the CrossbowVis whose bolt_pose (the AIM POINT, INV-494) we moved off its rest on the groove; 0 = at rest.
// Put back on every exit from Laid that is not a seat, on a refused redraw of that model and in ReloadOffTick. Game thread.
uintptr_t   g_xbBoltMovedModel = 0;
// [XBOW] AIM HELD (Jay 2026-11-05, RUN BW3 "that hasn't fixed it" -> "Yes"): per run of updates in which the draw put bolt_pose at its
// REST (not Laid), how far the GAME's own pose of that update was from it - the measured cause of the empty-crossbow crosshair (INV-676
// addendum: Shoot 9-10 = 5.83 / 8.35 deg). One line when the run ends (an update without a rest write, or another model); the first
// sample of the launch logged unconditionally. Game thread. COST: two compares + 4 adds per rest write; one line per reload.
struct XbowAimHeld {
    uintptr_t model = 0;
    long      n = 0, unread = 0;
    double    sumDeg = 0.0;
    float     minDeg = 1e9f, maxDeg = 0.0f, maxMm = 0.0f;
};
XbowAimHeld g_xbAim;
bool        g_xbAimFirstSaid = false;
HandCarry   g_xbCarry;                  // the bolt on l_hand - one left hand, one carry (game thread)
// Final re-review residual (Jay 2026-11-03: "Yes, fix it and rebuild"): handcarry.cpp Faulted() drops a carry whose engine call faulted
// but does not remember WHICH object - without this the Take below retried the same pointer every update (a "HANDCARRY FAULT" line
// ~90 a second under the log lock, and a repeated call on a faulted pointer, against handcarry.cpp's "never a second call on that
// pointer"). The bolt pointer that faulted is latched here and never called on again; a different pointer (a new draw's object)
// clears it. 0 = none. Game thread.
uintptr_t   g_xbFaultedObj = 0;
// Jay after RUN XB1 (2026-11-03, "Add all 3 fixes"): while the crossbow is not loaded (any step but Idle - empty, racking, cocked,
// carrying, laid) the LEFT grip never reaches the game as L1 - an accidental press next to the quiver no longer fires the game's
// left-hand action. Separate from g_ownsLeft on purpose: it withholds L1 only and does NOT claim the press from loot / push-grab.
// Written by the game thread each crossbow update with its time; read by the pad thread (ReloadWithholdsL1), stale after
// kPublishFreshMs (a weapon switch stops the writes).
volatile LONG   g_xbL1Block = 0;
volatile LONG64 g_xbL1Ms = 0;
bool        g_xbTakeLiveSet = false;    // HolsterSetXbowTakeLive(true) was sent (game thread)
volatile LONG g_xbHandFree = 0;         // ReloadXbowHandFree: Carry / Laid (game thread writes)
volatile LONG g_vrGripSeen = -1;        // ReloadPadPoll's last vr_grip, for the ARMED / TAKE OFF lines; -1 = not polled yet
bool        g_xbArmedSaid = false, g_xbTakeOffSaid = false;
long        g_nXbow = 0;
long        g_xbCounts[12] = {};        // per XEvent, for the counts line
ULONGLONG   g_xbLastPeriod = 0;
const char* g_xbSndRefused[4] = {};     // the sounds already named REFUSED (one line each)
const char* g_xbLastCarryDid = nullptr; // the last CARRY result logged (a literal) - a repeat is counted, not logged (final review I1)
long        g_xbCarryRepeats = 0;       // repeats since the last CARRY line
long        g_xbCarryRepeatsTotal = 0;  // all repeats this session (the 5 s counts line)
bool XbowLine() { return ++g_nXbow <= kMaxXbowLines; }

const char* XStepName(reload::XStep s)
{
    switch (s) {
    case reload::XStep::Idle: return "Idle";
    case reload::XStep::Empty: return "Empty";
    case reload::XStep::Racking: return "Racking";
    case reload::XStep::Cocked: return "Cocked";
    case reload::XStep::Carry: return "Carry";
    case reload::XStep::Laid: return "Laid";
    }
    return "?";
}

// The crossbow is not driven this update (no held model / another model / manual_reload off): the take zone, the bolt display and the
// hand-free flag drop at once. COST: one bool test + two stores.
void XbowDropLive()
{
    if (g_xbTakeLiveSet) {
        HolsterSetXbowTakeLive(false);
        g_xbTakeLiveSet = false;
    }
    g_xbowShowVis = 0;
    InterlockedExchange(&g_xbHandFree, 0);
}

// The held model changed (once per change). A carried bolt whose crossbow is NOT the new model is FORGOTTEN - never called on (its
// CrossbowVis may be gone; the bow nock's rule, INV-613 F7). The same model back (a blip with no model) keeps it: the fresh draw then
// gives it back (XMachine freshDraw -> Empty -> handcarry::Decide GiveBack).
void XbowAimFlush();   // [XBOW] AIM HELD - below XbowWriteBolt
void XbowModelChanged(uintptr_t model)
{
    if (g_xbAim.n || g_xbAim.unread) { XbowAimFlush(); }   // the run of rest writes ends with the switch
    if (g_xbCarry.active && reinterpret_cast<void*>(model) != g_xbCarry.homeModel) {
        HandCarryForget(g_xbCarry, "the held model changed - the crossbow is not in the right hand");
    }
    XbowDropLive();
    g_xb = XbowDraw{};
}

// out = base with its local t moved d along the unit axis (bone_root's frame). Pure.
void LocalAlong(const float* base, const reload::Vec3& ax, float d, float out[12])
{
    memcpy(out, base, 12 * sizeof(float));
    out[3] += ax.x * d;
    out[7] += ax.y * d;
    out[11] += ax.z * d;
}

// Ruling T2-a: the COMMANDED left hand BONE's origin - the controller's grip point less l_handholder's offset in l_hand, turned by the
// published hand basis (MagRowRel's convention: hand bone = H x [I | -holderT]). No basis / offset: the grip point itself. No commanded
// point: the published hand. False = no hand at all. Pure arithmetic.
bool HandBoneOrigin(const float* cmdPos, const float* hand, const float* handR9, const float* holderT, float out[3])
{
    const float* p = cmdPos ? cmdPos : hand;
    if (!p) { return false; }
    float H[12];
    if (handR9 && holderT && Hand34(p, handR9, H)) {
        for (int r = 0; r < 3; ++r) {
            out[r] = p[r] - (H[4 * r] * holderT[0] + H[4 * r + 1] * holderT[1] + H[4 * r + 2] * holderT[2]);
        }
        return true;
    }
    for (int r = 0; r < 3; ++r) { out[r] = p[r]; }
    return true;
}

// bolt_pose written at `local12` (bone_root's frame, the ROW's rotation / translation - fix round 1 ruling T5-a) with its basis columns
// kept at the LENGTHS the bone has now: the mesh's nodes.json bolt_pose local is unit-scaled while OUR clip carries an authored static
// scale 3.22526 on it (INV-676, the C5 exemption) - which one the engine draws is [UNKNOWN], so the scale is never ours to change. A
// read-back of our own earlier write has the same lengths (idempotent); a failed read writes the row's own scale. COST: one world read
// + one write. gameDegOut / gameMmOut (optional, the [XBOW] AIM HELD line): how far the pose found (this update's, before the write)
// is from the written one - its +Z (the bore, INV-494) in degrees and its origin in mm; -1 when unread.
bool XbowWriteBolt(uintptr_t model, int boltId, const float rootW[12], const float local12[12], float* gameDegOut = nullptr,
                   float* gameMmOut = nullptr)
{
    float w[12], live[12];
    Mul34(rootW, local12, w);
    if (gameDegOut) { *gameDegOut = -1.0f; }
    if (gameMmOut) { *gameMmOut = -1.0f; }
    if (ElemMtx(g_world, model, boltId, live)) {
        if (gameDegOut) {
            const float lz = Len3(live[2], live[6], live[10]), wz = Len3(w[2], w[6], w[10]);
            if (lz > 1e-6f && wz > 1e-6f && std::isfinite(lz)) {
                float c = (live[2] * w[2] + live[6] * w[6] + live[10] * w[10]) / (lz * wz);
                c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                *gameDegOut = std::acos(c) * 57.2957795f;
            }
        }
        if (gameMmOut) { *gameMmOut = 1000.0f * Len3(live[3] - w[3], live[7] - w[7], live[11] - w[11]); }
        for (int c = 0; c < 3; ++c) {
            const float lw = Len3(w[c], w[4 + c], w[8 + c]), ll = Len3(live[c], live[4 + c], live[8 + c]);
            if (lw > 1e-6f && ll > 1e-6f && std::isfinite(ll)) {
                const float k = ll / lw;
                w[c] *= k;
                w[4 + c] *= k;
                w[8 + c] *= k;
            }
        }
    }
    return SetWorld(model, boltId, w);
}

// [XBOW] AIM HELD: one rest write's sample (deg / mm < 0 = the game's pose was unreadable) and the end of a run. Game thread.
void XbowAimFlush()
{
    if (g_xbAim.n == 0 && g_xbAim.unread == 0) { g_xbAim = XbowAimHeld{}; return; }
    const long read = g_xbAim.n;
    // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=yes
    LOG("reload: [XBOW] AIM HELD %ld update(s) at rest (model %p): the game's own bolt_pose +Z was mean %.2f, min %.2f, max %.2f deg off "
        "the rest (swing %.2f deg), origin up to %.1f mm; unreadable %ld | the crosshair's bore is published AFTER this write.",
        read + g_xbAim.unread, reinterpret_cast<void*>(g_xbAim.model), read ? g_xbAim.sumDeg / read : -1.0,
        read ? g_xbAim.minDeg : -1.0f, read ? g_xbAim.maxDeg : -1.0f, read ? g_xbAim.maxDeg - g_xbAim.minDeg : -1.0f,
        read ? g_xbAim.maxMm : -1.0f, g_xbAim.unread);
    g_xbAim = XbowAimHeld{};
}
void XbowAimSample(uintptr_t model, float deg, float mm)
{
    if (g_xbAim.model && g_xbAim.model != model) { XbowAimFlush(); }
    g_xbAim.model = model;
    if (deg < 0.0f) { ++g_xbAim.unread; return; }
    if (!g_xbAimFirstSaid) {
        g_xbAimFirstSaid = true;
        // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [XBOW] AIM HELD FIRST (logged unconditionally): the game's own bolt_pose +Z %.2f deg off the rest, origin %.1f mm "
            "(model %p) - bolt_pose written at rest; the crosshair reads it after this.", deg, mm, reinterpret_cast<void*>(model));
    }
    ++g_xbAim.n;
    g_xbAim.sumDeg += deg;
    if (deg < g_xbAim.minDeg) { g_xbAim.minDeg = deg; }
    if (deg > g_xbAim.maxDeg) { g_xbAim.maxDeg = deg; }
    if (mm > g_xbAim.maxMm) { g_xbAim.maxMm = mm; }
}

// Ruling T5-b: bolt_pose (the AIM POINT, INV-494) back at the row's rest when we left it moved along the groove. `why` = a literal.
// COST: one compare unless it was moved; then one bone_root world read + XbowWriteBolt + one line.
bool g_xbRestoreFailSaid = false;
void XbowRestoreBolt(uintptr_t model, const char* why)
{
    if (!g_xbBoltMovedModel || g_xbBoltMovedModel != model) { return; }
    const XbowDraw& x = g_xb;
    float rootW[12];
    const bool ok = x.row && x.boltId >= 0 && ElemMtx(g_world, model, x.rootId, rootW) && XbowWriteBolt(model, x.boltId, rootW, x.row->boltRest);
    if (ok) {
        g_xbBoltMovedModel = 0;
        g_xbRestoreFailSaid = false;
    }
    if ((ok || !g_xbRestoreFailSaid) && XbowLine()) {
        if (!ok) { g_xbRestoreFailSaid = true; }
        // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [XBOW] BOLT_POSE %s (%s) - model %p: the aim point %s.", ok ? "RESTORED" : "*** NOT RESTORED ***", why,
            reinterpret_cast<void*>(model), ok ? "is back at the row's rest" : "is still moved along the groove (retried each update)");
    }
}

// A crossbow sound the shared player could not play: one line per sound (carry ruling T2-c - the script plays them with PlaySound23D,
// gunsound.cpp replays the 2D export by name / volume / start).
void XbowSoundRefused(const char* name)
{
    for (const char*& s : g_xbSndRefused) {
        if (s == name) { return; }
        if (!s) {
            s = name;
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("reload: [XBOW] SOUND REFUSED - \"%s\" was not played (gunsound's replica of the 2D PlaySound export; the crossbow script "
                "plays it with PlaySound23D) - the step goes on silent.", name ? name : "(null)");
            return;
        }
    }
}

// Classify a held crossbow (ClassifyModel, once per model change): the row, bone_root and the three parts' parents, the two hand rows,
// the bolt object (CrossbowVis + 1480, ONCE per draw), the detour. Any missing -> REFUSED by name and false: the game reloads it as
// today this draw. The ARMED line (first crossbow draw of the session) is logged unconditionally, refused or not.
bool ClassifyXbow(uintptr_t model, uintptr_t item, const reload::Classified& c, const char* meshNorm, const char* meshShown,
                  const char* prefix, int stringId, int rackId, int boltId)
{
    g_xb = XbowDraw{};
    XbowDraw& x = g_xb;
    x.row = c.xrow;
    x.stringId = stringId;
    x.rackId = rackId;
    x.boltId = boltId;
    ElemId(model, "bone_root", &x.rootId);
    int ps = -1, pr = -1, pb = -1;
    ElemParent(model, stringId, &ps);
    ElemParent(model, rackId, &pr);
    ElemParent(model, boltId, &pb);
    x.rackRow = reload::RackRowFor(reload::Family::Crossbow, meshNorm, prefix);   // once per draw (a scan of the rack rows)
    x.carryRow = reload::CarryRowFor(reload::Family::Crossbow);
    uintptr_t obj = 0;
    const bool okObj = ReadPtr(model + kXbowBoltObjOff, &obj);
    x.boltObj = okObj ? obj : 0;
    x.rackDoneM = x.row ? x.row->rackTravelM * ReloadRackPullM / reload::kPistolSlideTravelM : 0.0f;
    const char* why = !x.row ? "no crossbow row"
                    : g_xbowDetour != 1 ? "the bolt-visibility detour is not live (the XBOW line at READY says why)"
                    : x.rootId < 0 ? "the mesh has no bone_root"
                    : (ps != x.rootId || pr != x.rootId || pb != x.rootId)
                        ? "bone_string / bone_stringreload / bolt_pose are not all children of bone_root"
                    : !x.rackRow ? "no rack hand row for this mesh (reload_hand_poses.inc)"
                    : !x.carryRow ? "no bolt-carry hand row crossbow_bolt (reload_hand_poses.inc)"
                    : !okObj ? "the bolt object read (CrossbowVis +1480) faulted"
                    : !x.boltObj ? "the bolt object (CrossbowVis +1480) is null"
                    : nullptr;
    if (!g_xbArmedSaid) {
        g_xbArmedSaid = true;
        const char* takeOff = HolsterLeftTakeRefusal();
        const LONG vg = g_vrGripSeen;
        // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [XBOW] ARMED (first crossbow draw, logged unconditionally) - manual_reload=%d | row %s (mesh '%s', prefix '%s') | "
            "elements bone_root %d, bone_string %d (parent %d), bone_stringreload %d (parent %d), bolt_pose %d (parent %d) | bolt object "
            "(CrossbowVis +1480) %p%s | bolt-visibility detour +755370 %s%s | rack travel %.4f m, string travel %.4f m, catch at %.4f m "
            "(reload_rack_pull_m %.3f), rack grab %.3f m | lay %.3f m within %.1f deg, hold %.3f m, seat %.3f m, groove line %.2f m | "
            "hand rows: rack %s, carry %s | left-shoulder take: vr_grip %s, left zones %s%s%s | this draw: %s",
            ManualReload ? 1 : 0, x.row ? x.row->mesh : "NONE", meshShown, prefix, x.rootId, stringId, ps, rackId, pr, boltId, pb,
            reinterpret_cast<void*>(x.boltObj), okObj ? "" : " (read FAULTED)", g_xbowDetour > 0 ? "LIVE" : "OFF - ",
            (g_xbowDetour > 0 || !g_xbowDetourWhy) ? "" : g_xbowDetourWhy, x.row ? x.row->rackTravelM : 0.0f,
            x.row ? x.row->stringTravelM : 0.0f, x.rackDoneM, ReloadRackPullM, ReloadGrabM, BowNockCatchM, BowNockCatchDeg, BowNockHoldM,
            BowNockSeatM, BowNockCatchLenM, x.rackRow ? x.rackRow->key : "NONE", x.carryRow ? x.carryRow->key : "NONE",
            vg < 0 ? "not polled yet" : vg ? "1" : "*** 0 - the left grip is not read: the take is OFF ***",
            takeOff ? "*** REFUSED (" : "not refused", takeOff ? takeOff : "", takeOff ? ") - the take is OFF ***" : "",
            why ? why : "manual");
    }
    if (why) {
        if (XbowLine()) {
            // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [XBOW] REFUSED model %p item %p (mesh '%s'): %s - the crossbow reloads as today this draw.",
                reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown, why);
        }
        return false;
    }
    x.fresh = true;
    if (XbowLine()) {
        // Final-review fix (ledger minor T5 M3 / ruling T5-e): bone_root's WORLD scale (its basis column lengths - the groove distance is
        // measured in world metres and applied as a bone_root local offset) and bolt_pose's LIVE world scale (what XbowWriteBolt keeps),
        // each the mean of its three column lengths, -1 = unreadable. Read once per draw (two world reads).
        float rw[12], bw[12];
        const float rootScale = ElemMtx(g_world, model, x.rootId, rw)
            ? (Len3(rw[0], rw[4], rw[8]) + Len3(rw[1], rw[5], rw[9]) + Len3(rw[2], rw[6], rw[10])) / 3.0f : -1.0f;
        const float boltScale = ElemMtx(g_world, model, boltId, bw)
            ? (Len3(bw[0], bw[4], bw[8]) + Len3(bw[1], bw[5], bw[9]) + Len3(bw[2], bw[6], bw[10])) / 3.0f : -1.0f;
        // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [XBOW] DRAW model %p item %p (mesh '%s'): manual - bolt object %p on bolt_pose %d; the parts' rests are the row's "
            "(mesh data): rack t (%+.4f %+.4f %+.4f), string t (%+.4f %+.4f %+.4f), bolt_pose t (%+.4f %+.4f %+.4f) | world scale: "
            "bone_root %.4f, bolt_pose %.4f (live - kept by every bolt_pose write; -1 = unreadable).",
            reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown, reinterpret_cast<void*>(x.boltObj), boltId,
            x.row->rackRest[3], x.row->rackRest[7], x.row->rackRest[11], x.row->stringRest[3], x.row->stringRest[7],
            x.row->stringRest[11], x.row->boltRest[3], x.row->boltRest[7], x.row->boltRest[11], rootScale, boltScale);
    }
    return true;
}

// One update of the held crossbow (ReloadTick, game thread). Returns what was drawn. COST per update with a crossbow held: one rounds
// call, one bone_root world read, two 3x3-by-vector products, the step machine; + the spare (2 game calls) only while Cocked;
// + LockedHand while Empty; + one l_hand world read and two 3x4 products while Carry / Laid; + one export call (HandCarryStillOn) while
// carrying; + 2-3 SetElementWorldMatrix while a step draws (Empty / Racking: the rack + the string; Cocked / Carry / Laid / settle: +
// bolt_pose, whose write adds one world read - XbowWriteBolt). Events only: TakeAs / GiveBack (4 / 3 export calls), the add-bullet, a
// pulse, up to 2 sound calls.
const char* DriveXbow(uintptr_t ctrl, ItemState* st, const float* hand, const float* handR9, const float* cmdPos, const float* holderT,
                      void* playerModel, int lHandId, ULONGLONG now)
{
    using reload::XEvent;
    using reload::XStep;
    XbowDraw& x = g_xb;
    const reload::XbowRow& row = *x.row;
    // The left-shoulder take is live while this crossbow is driven (holster.cpp gates it by manual_reload too). COST: one bool test.
    if (!g_xbTakeLiveSet) {
        HolsterSetXbowTakeLive(true);
        g_xbTakeLiveSet = true;
    }
    // Carry ruling T4 M4: the take OFF by the holster's own refusal (vr_grip=0 / an overlapping left pair) is named once.
    // COST: two volatile reads until said.
    if (!g_xbTakeOffSaid) {
        const char* off = HolsterLeftTakeRefusal();
        if (g_vrGripSeen == 0) {
            g_xbTakeOffSaid = true;
            // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [XBOW] TAKE OFF - vr_grip=0: the left grip is not read, so neither the rack nor the left-shoulder take can be "
                "grabbed (the crossbow stays empty).");
        } else if (off) {
            g_xbTakeOffSaid = true;
            // [PROBE: XBOW] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [XBOW] TAKE OFF - the left zones are refused (%s): the left-shoulder take never fires, so no bolt can be taken "
                "(the rack still works).", off);
        }
    }
    const XStep before = st->xm.step();
    float rootW[12];
    const bool haveRoot = ElemMtx(g_world, g_gun.model, x.rootId, rootW);
    const bool canDraw = haveRoot;
    // The FORWARD spots: the row's rest (mesh data, ruling T5-a) - travel x the BACK axis.
    float rackFwd[12] = {}, stringFwd[12] = {};
    LocalAlong(row.rackRest, row.rackAxis, -row.rackTravelM, rackFwd);
    LocalAlong(row.stringRest, row.stringAxis, -row.stringTravelM, stringFwd);
    // The rack's BACK axis and the groove's FORWARD axis in the world, and the seat point (bone_root x the row's rest-local data).
    float rackAxW[3] = { 0.0f, 0.0f, 0.0f }, grooveW[3] = { 0.0f, 0.0f, 1.0f }, seatW[3] = {};
    const bool haveRackAx = haveRoot && AxisOf(rootW, row.rackAxis, rackAxW);
    const bool haveGroove = haveRoot && AxisOf(rootW, row.grooveAxis, grooveW);
    if (haveRoot) { XformPt(rootW, row.seatPoint, seatW); }
    // reload_hands.h UseCommandedHand: while the lock holds the DRAWN hand on the rack, the pull is the controller's.
    const float* pullHand = cmdPos ? cmdPos : hand;

    reload::XInput in{};
    in.manualOn = ManualReload;
    in.isCrossbow = true;
    in.freshDraw = x.fresh;
    x.fresh = false;
    in.gripDown = g_gripDown != 0;
    in.inShoulder = HolsterLeftShoulderIn();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    if (before == XStep::Cocked) { st->spare = SpareOf(ctrl, st->item); }   // the take reads it - only while cocked
    in.spare = st->spare;
    // THE RACK GRAB (ruling T2-a): the commanded left hand bone vs where Jay's rack row puts the hand bone on the FORWARD rack (the
    // lock's own maths, reload_hands.h LockedHand). Only while Empty.
    bool haveGrabMeasure = false;
    if (before == XStep::Empty && canDraw && x.rackRow) {
        float rw[12], r9[9], target[3], bone[3];
        Mul34(rootW, rackFwd, rw);
        if (reload::LockedHand(rw, *x.rackRow, target, r9) && HandBoneOrigin(cmdPos, hand, handR9, holderT, bone)) {
            in.handToRackM = Len3(bone[0] - target[0], bone[1] - target[1], bone[2] - target[2]);
            haveGrabMeasure = true;
        }
    }
    if (before == XStep::Racking && pullHand && haveRackAx) {
        in.rackBackM = (pullHand[0] - st->xGrabCmd[0]) * rackAxW[0] + (pullHand[1] - st->xGrabCmd[1]) * rackAxW[1] +
                       (pullHand[2] - st->xGrabCmd[2]) * rackAxW[2];
    }
    // THE HAND'S NOCK (Carry / Laid): the rendered l_hand x Jay's bolt-carry frame (XbowRow::boltCarry: nock origin, +z to the tip) -
    // where TakeAs draws the carried bolt - against the groove line from the seat, BowNockCatchLenM forward.
    float nockW[3] = {}, tipW[3] = {};
    float along = 0.0f;
    bool haveNock = false;
    if ((before == XStep::Carry || before == XStep::Laid) && haveRoot && haveGroove && playerModel && lHandId >= 0) {
        float lh[12], bw[12];
        if (ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh)) {
            Mul34(lh, row.boltCarry, bw);
            nockW[0] = bw[3];
            nockW[1] = bw[7];
            nockW[2] = bw[11];
            haveNock = Axis(bw, 2, tipW) && std::isfinite(nockW[0]) && std::isfinite(nockW[1]) && std::isfinite(nockW[2]);
        }
    }
    if (haveNock) {
        const float d[3] = { nockW[0] - seatW[0], nockW[1] - seatW[1], nockW[2] - seatW[2] };
        const float s = d[0] * grooveW[0] + d[1] * grooveW[1] + d[2] * grooveW[2];
        along = s < 0.0f ? 0.0f : (s > BowNockCatchLenM ? BowNockCatchLenM : s);
        in.nockToLineM = Len3(seatW[0] + grooveW[0] * along - nockW[0], seatW[1] + grooveW[1] * along - nockW[1],
                              seatW[2] + grooveW[2] * along - nockW[2]);
        in.nockToSeatM = along;
        float ct = tipW[0] * grooveW[0] + tipW[1] * grooveW[1] + tipW[2] * grooveW[2];
        ct = ct > 1.0f ? 1.0f : (ct < -1.0f ? -1.0f : ct);
        in.dirDeg = std::acos(ct) * 57.2957795f;
    } else if (before == XStep::Laid) {
        // No hand this update: the laid bolt HOLDS (the bow nock's rule - no unlay / seat from a stand-in point).
        in.nockToLineM = 0.0f;
        in.nockToSeatM = 1e9f;
    }

    reload::XParams p;
    p.rackGrabM = reload::kXbowRackGrabM + reload::kGrabWidenM;   // 0.10 m (Jay 2026-11-04) + 0.04 (trace 20261116_1126), not reload_grab_m
    p.rackDoneM = x.rackDoneM;
    p.catchM = BowNockCatchM;
    p.holdM = BowNockHoldM;
    p.seatM = BowNockSeatM;
    p.catchDeg = BowNockCatchDeg;
    const reload::XOut o = st->xm.Update(in, p);
    if (o.ev == XEvent::RackGrab && pullHand) { memcpy(st->xGrabCmd, pullHand, sizeof(st->xGrabCmd)); }

    // THE CARRY (handcarry.h; the bow nock's single-owner rule handcarry::Decide): the bolt object rides l_hand while Carry - the engine
    // moves it, nothing is written per frame; any other step gives it back to bolt_pose as found (Laid: the groove write below then
    // moves bolt_pose). A changed object is forgotten, never called on.
    {
        handcarry::In hc{};
        if (g_xbFaultedObj && x.boltObj != g_xbFaultedObj) { g_xbFaultedObj = 0; }   // a new object: the latch is for the old one only
        const bool faultedObj = x.boltObj && x.boltObj == g_xbFaultedObj;            // COST: two pointer compares per update
        hc.wantCarry = o.step == XStep::Carry && x.boltObj && !faultedObj && playerModel && lHandId >= 0 && x.boltId >= 0;
        hc.active = g_xbCarry.active;
        hc.sameObject = handcarry::SameObject(reinterpret_cast<void*>(g_gun.model), reinterpret_cast<void*>(x.boltObj),
                                              g_xbCarry.homeModel, g_xbCarry.obj);
        hc.bodyChanged = g_xbCarry.active && playerModel != g_xbCarry.onModel;
        hc.stillOn = !(hc.active && hc.sameObject && hc.wantCarry && !hc.bodyChanged) || HandCarryStillOn(g_xbCarry);
        hc.relChanged = false;   // XbowRow::boltCarry is fixed data: nothing moves the bolt in the fist while it is carried
        float rel[12];
        const char* did = nullptr;
        const long forgetsBefore = g_xbCarry.forgets;   // Faulted() bumps it; HandCarryForget (Act::Forget) does too - excluded below
        const handcarry::Act act = handcarry::Decide(hc);
        switch (act) {
        case handcarry::Act::Take:
            // Carry ruling T3 M-1: our own line around TakeAs (its internal refusal lines carry the bow's tag).
            did = HandCarryTakeAs(g_xbCarry, reinterpret_cast<void*>(x.boltObj), reinterpret_cast<void*>(g_gun.model), x.boltId,
                                  playerModel, lHandId, row.boltCarry)
                ? "bolt IN THE FIST (TakeAs at Jay's carry frame)" : "*** TakeAs REFUSED - the bolt stays on bolt_pose ***";
            break;
        case handcarry::Act::Retake:
            memcpy(rel, g_xbCarry.appliedRel, sizeof(rel));   // carry ruling T3 M-4: the applied rel, never `drawn`
            did = HandCarryReattach(g_xbCarry, rel) ? "bolt attached back to l_hand (the game had moved it)" : "*** re-attach FAILED ***";
            break;
        case handcarry::Act::SetLocal:
            memcpy(rel, g_xbCarry.appliedRel, sizeof(rel));
            HandCarrySetLocal(g_xbCarry, rel);
            did = "bolt local set again";
            break;
        case handcarry::Act::GiveBack:
            did = HandCarryGiveBack(g_xbCarry, hc.bodyChanged ? "the body changed"
                                             : o.step == XStep::Laid ? "laid on the groove"
                                             : o.ev == XEvent::EarlyRelease ? "early release"
                                             : "the crossbow left the carry step")
                ? "bolt back on bolt_pose" : "*** give-back NOT HOME ***";
            break;
        case handcarry::Act::Forget:
            HandCarryForget(g_xbCarry, "the crossbow or its bolt object changed");
            did = "bolt forgotten (the object changed - not called on)";
            break;
        default: break;
        }
        // An engine call inside TakeAs / Reattach / SetLocal / GiveBack FAULTED (handcarry.cpp Faulted: forgets went up on an act that
        // is not a Forget): latch the object - never called on again (see g_xbFaultedObj). Logged once per latch (an event, capped).
        if (act != handcarry::Act::Forget && act != handcarry::Act::None && g_xbCarry.forgets != forgetsBefore && x.boltObj) {
            g_xbFaultedObj = x.boltObj;
            did = "*** an engine call FAULTED - this bolt object is latched and never called on again (no bolt in the fist) ***";
        }
        // Final-review fix (INV-9 / INV-10): a carry that keeps failing (TakeAs refused and retried every update, ~90 a second) must not
        // spend the run's line cap - the CARRY line is logged only when its result CHANGES; the repeats are counted and named on the
        // next line and in the 5 s counts line. The retry itself is unchanged. COST: one pointer compare per carry decision.
        if (did && did == g_xbLastCarryDid) {
            ++g_xbCarryRepeats;
            ++g_xbCarryRepeatsTotal;
        } else if (did) {
            const long repeats = g_xbCarryRepeats;
            g_xbLastCarryDid = did;
            g_xbCarryRepeats = 0;
            if (XbowLine()) {
                // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
                LOG("reload: [XBOW] CARRY %s - step %s, object %p, l_hand %d on body %p (takes %ld, give-backs %ld, forgets %ld, attached "
                    "back %ld; the previous carry result repeated %ld more time(s), not logged).", did, XStepName(o.step),
                    reinterpret_cast<void*>(x.boltObj), lHandId, playerModel, g_xbCarry.takes, g_xbCarry.giveBacks, g_xbCarry.forgets,
                    g_xbCarry.retakes, repeats);
            }
        }
        if ((o.ev == XEvent::Take || o.ev == XEvent::Unlay) && !hc.wantCarry && XbowLine()) {
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("reload: [XBOW] CARRY *** NOT TAKEN *** - %s: the bolt is not drawn in the fist (the step goes on; the lay and the seat "
                "still read the fist).", lHandId < 0 ? "l_hand is unresolved on this body (the CARRY HAND line)"
                                       : !playerModel ? "no player body" : "no bolt object");
        }
    }

    // THE SEAT: the game's own add-bullet through the trampoline (the pistol's seat) - clip 1, so the game shows the bolt itself.
    if (o.callAddBullet) {
        const int r0 = rounds, s0 = SpareOf(ctrl, st->item);
        const bool called = CallAddBullet(ctrl, st->item);
        int r1 = -1;
        CallCtlInt(g_rounds, ctrl, &r1);
        const int s1 = SpareOf(ctrl, st->item);
        if (XbowLine()) {
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] SEAT rounds %d -> %d, spare %d -> %d (the game's own add-bullet)%s | the nock %.3f m from the seat along "
                "the groove, %.3f m off the line | pulse %s.", r0, r1, s0, s1, called ? "" : " - *** the add-bullet call FAULTED ***",
                in.nockToSeatM > 1e8f ? -1.0f : in.nockToSeatM, in.nockToLineM > 1e8f ? -1.0f : in.nockToLineM,
                HolsterHaptic > 0.0f ? "yes" : "no (holster_haptic 0)");
        }
    }
    if (o.ev != XEvent::None) {
        const int ei = static_cast<int>(o.ev);
        if (ei >= 0 && ei < 12) { ++g_xbCounts[ei]; }
    }
    if (o.ev != XEvent::None && o.ev != XEvent::Seat && XbowLine()) {
        const char* pulse = !o.haptic ? "no pulse" : HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)";
        const float toRack = in.handToRackM > 1e8f ? -1.0f : in.handToRackM;
        const float toLine = in.nockToLineM > 1e8f ? -1.0f : in.nockToLineM;
        switch (o.ev) {
        case XEvent::RackGrab:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] RACK GRAB - the hand bone %.3f m from Jay's rack pose on the forward rack (grab %.3f m); the pull is read "
                "from the %s hand; %s.", toRack, p.rackGrabM, cmdPos ? "COMMANDED (controller)" : "published", pulse);
            break;
        case XEvent::Catch:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] CATCH - the rack pulled back %.3f m (the catch at %.3f m of its %.4f m travel): the string is cocked; %s.",
                in.rackBackM, x.rackDoneM, row.rackTravelM, pulse);
            break;
        case XEvent::RackLetGo:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] RACK LET GO - at %.3f m back (the catch at %.3f m): the rack and the string go forward again; %s.",
                in.rackBackM, x.rackDoneM, pulse);
            break;
        case XEvent::Take:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] TAKE - a bolt from over the left shoulder (spare %d); %s.", in.spare, pulse);
            break;
        case XEvent::TakeNoBolt:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] TAKE REFUSED - no bolts (count %d): nothing taken; %s.", in.spare, pulse);
            break;
        case XEvent::TakeNotCocked:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] TAKE REFUSED - string not at the catch (rack it back first): nothing taken; %s.", pulse);
            break;
        case XEvent::Lay:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] LAY - the nock %.3f m off the groove line, the bolt %.1f deg off its direction (lay %.3f m within %.1f deg), "
                "%.3f m from the seat: on the groove.", toLine, in.dirDeg, p.catchM, p.catchDeg, along);
            break;
        case XEvent::LayRefusedAngle:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] LAY REFUSED - the bolt points %.1f deg off the groove (limit %.1f deg; the nock %.3f m off the line).",
                in.dirDeg, p.catchDeg, toLine);
            break;
        case XEvent::Unlay:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] UNLAY - the nock %.3f m off the line (hold %.3f m): back in the fist.", toLine, p.holdM);
            break;
        case XEvent::EarlyRelease:
            // [PROBE: XBOW] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [XBOW] EARLY RELEASE - the grip let go in step %s: the bolt goes back, nothing consumed (spare %d); the string "
                "stays cocked.", XStepName(before), in.spare);
            break;
        default: break;
        }
    }
    // Spec 4.6 / 4.9: one pulse on rack grab (Jay 2026-11-04), take, catch, seat (o.haptic); the crossbow's own sounds at the catch and the seat (reload_fx.h row).
    if (o.haptic && HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
    if (o.fx == reload::Fx::RackDone || o.fx == reload::Fx::Seat) {
        int ns = 0;
        const reload::Snd* snd = reload::SoundsFor(g_gun.fx, o.fx, &ns);
        for (int i = 0; i < ns; ++i) {
            if (!GunSoundPlay(g_curPlayer, snd[i].name, snd[i].vol, snd[i].delayS, snd[i].startS)) { XbowSoundRefused(snd[i].name); }
        }
        if (ns == 0) {
            XbowSoundRefused(o.fx == reload::Fx::RackDone ? "crossbow_load_begin_00.wav (no fx row sound for this mesh)"
                                                          : "crossbow_load_bolt_00.wav (no fx row sound for this mesh)");
        }
    }

    // DRAW (after the game's update, this tail - the guns' route). Siblings under bone_root: no parent-first order between them.
    const char* drew = "nothing (the game's own pose)";
    float rackDrawn[12] = {};
    bool haveRackDrawn = false;
    bool restWrote = false;   // [XBOW] AIM HELD: bolt_pose written at its rest this update
    if (canDraw) {
        float rl[12], sl[12], bl[12], w[12];
        bool wr = false, ws = false, wb = false;
        if (o.step == XStep::Empty) {
            memcpy(rl, rackFwd, sizeof(rl));   // ruling P3: the rack sits FORWARD at the string, where Jay grabs it
            // Fix round 1 ruling T5-c: the STRING forward too, every update - after a RACK LET GO the game may not re-pose the part we
            // drew part-way back (INV-672); forward is what the game's own empty pose draws.
            memcpy(sl, stringFwd, sizeof(sl));
            wr = ws = true;
            drew = "rack + string forward, bolt_pose at rest (empty)";
        } else if (o.step == XStep::Racking) {
            // The rack follows the hand back, clamped to its travel; the string by the same SHARE of its own travel.
            float b = in.rackBackM;
            b = b < 0.0f ? 0.0f : (b > row.rackTravelM ? row.rackTravelM : b);
            const float share = row.rackTravelM > 1e-6f ? b / row.rackTravelM : 0.0f;
            LocalAlong(rackFwd, row.rackAxis, b, rl);
            LocalAlong(stringFwd, row.stringAxis, share * row.stringTravelM, sl);
            wr = ws = true;
            drew = "rack + string with the hand, bolt_pose at rest";
        } else if (o.stringAtCatch || o.stringSettle) {
            // Cocked / Carry / Laid: at REST (cocked) every update - the game's empty pose draws them forward; the settle after the
            // seat the same (INV-672: a part we stop writing stays where our last write left it).
            memcpy(rl, row.rackRest, sizeof(rl));
            memcpy(sl, row.stringRest, sizeof(sl));
            wr = ws = true;
            drew = o.stringSettle ? "rack + string + bolt_pose at rest (settle)" : "rack + string at the catch";
        }
        if (o.step == XStep::Laid) {
            // Ruling R3: bolt_pose on the groove at the projection of the fist's nock, clamped seat .. seat + bow_nock_catch_len_m.
            const float a = haveNock ? along : x.lastAlong;
            memcpy(bl, row.boltRest, sizeof(bl));
            bl[3] = row.seatPoint.x + row.grooveAxis.x * a;
            bl[7] = row.seatPoint.y + row.grooveAxis.y * a;
            bl[11] = row.seatPoint.z + row.grooveAxis.z * a;
            wb = true;
            drew = "string at the catch, bolt on the groove";
        } else if (o.stringAtCatch || o.stringSettle || o.boltAtRest || g_xbBoltMovedModel == g_gun.model) {
            // Fix round 1 ruling T5-b: bolt_pose (the AIM POINT, INV-494) at the row's rest while Cocked / Carry / settling, and on EVERY
            // update that leaves Laid without a seat (EarlyRelease -> Cocked, Unlay -> Carry, a fresh draw -> Empty): never left moved.
            // Empty / Racking too (o.boltAtRest, Jay 2026-11-05: "The crosshair sits off and shakes" -> "Yes"): the game's empty idle
            // (Shoot 9-10) animates bolt_pose 5.8 / 8.4 deg off the loaded aim and the crosshair follows it (INV-676 addendum).
            // COST: one XbowWriteBolt (one world read + one world write) per update while empty - the rack + string already write.
            memcpy(bl, row.boltRest, sizeof(bl));
            wb = true;
        }
        bool ok = true;
        if (wr) {
            Mul34(rootW, rl, rackDrawn);
            haveRackDrawn = true;
            ok = SetWorld(g_gun.model, x.rackId, rackDrawn) && ok;
        }
        if (ws) {
            Mul34(rootW, sl, w);
            ok = SetWorld(g_gun.model, x.stringId, w) && ok;
        }
        if (wb) {
            float gameDeg = -1.0f, gameMm = -1.0f;
            const bool okB = XbowWriteBolt(g_gun.model, x.boltId, rootW, bl, &gameDeg, &gameMm);
            ok = okB && ok;
            if (okB && o.step != XStep::Laid) {
                restWrote = true;
                XbowAimSample(g_gun.model, gameDeg, gameMm);
            }
            // Ruling T5-b bookkeeping: moved while Laid; back at rest once a rest write lands.
            if (o.step == XStep::Laid) {
                if (okB) { g_xbBoltMovedModel = g_gun.model; }
            } else if (okB && g_xbBoltMovedModel == g_gun.model) {
                g_xbBoltMovedModel = 0;
            }
        }
        if ((wr || ws || wb) && !ok) { drew = "*** a crossbow part WRITE FAULTED ***"; }
    }
    if (!restWrote && (g_xbAim.n || g_xbAim.unread)) { XbowAimFlush(); }   // the run of rest writes ended (Laid / loaded / no draw)
    if (haveNock) { x.lastAlong = along; }

    // The left hand's poses: the rack LOCK (Jay's rack row on bone_stringreload as drawn) + its fingers while Racking; the bolt fist's
    // fingers while Carry / Laid (no lock, switched at once). COST: HandsPublish's (a few compares; the seqlock write while on).
    const bool rackLock = o.rackPose && x.rackRow && haveRackDrawn;
    const reload::HandRow* fingerRow = o.rackPose ? x.rackRow : o.carryPose ? x.carryRow : nullptr;
    HandsPublish(rackLock, rackLock ? x.rackRow : nullptr, haveRackDrawn ? rackDrawn : nullptr, fingerRow, o.rackPose, now);
    // For the pad thread (carry ruling T4 M1): R2 withheld in every step but Idle; a left press is the reload's at the rack (Empty), in
    // the left-shoulder zone, or in a step the hand is doing (Racking / Carry / Laid) - its L1 withheld through ReloadPadPoll.
    const bool atRack = o.step == XStep::Empty && haveGrabMeasure && in.handToRackM <= p.rackGrabM;
    const bool handStep = o.step == XStep::Racking || o.step == XStep::Carry || o.step == XStep::Laid;
    Publish(o.withholdR2, reload::ReloadOwnsLeftPress(handStep, atRack || in.inShoulder));
    // L1 withheld while not loaded (Jay after RUN XB1). COST: two interlocked stores per crossbow update.
    InterlockedExchange(&g_xbL1Block, o.step != XStep::Idle ? 1 : 0);
    InterlockedExchange64(&g_xbL1Ms, static_cast<LONG64>(GetTickCount64()));
    // The bolt-visibility detour: our answer for this CrossbowVis while the bolt is in the fist / on the groove / settling.
    g_xbowShowVis = (o.step == XStep::Carry || o.step == XStep::Laid || o.stringSettle) ? g_gun.model : 0;
    InterlockedExchange(&g_xbHandFree, (o.step == XStep::Carry || o.step == XStep::Laid) ? 1 : 0);
    return drew;
}

// The counts line, every kXbowPeriodMs behind diag_periodic_logs (the first sample is the unconditional ARMED line). COST: one compare.
void XbowPeriodic(const ItemState* st, const char* drew, ULONGLONG now)
{
    if (now - g_xbLastPeriod < kXbowPeriodMs) { return; }
    g_xbLastPeriod = now;
    // [PROBE: XBOW] gate=manual_reload,diag_periodic_logs  site=HEADSET  log-only=yes
    LOG_PERIODIC("reload: [XBOW] 5 s: step %s, drew %s, bolt_pose %s | rack grab %ld, let go %ld, catch %ld | take %ld (no bolt %ld, not "
                 "cocked %ld), lay %ld (refused %ld), unlay %ld, early release %ld, seat %ld | bolt shown by us on %lld predicate calls | "
                 "carried: takes %ld, give-backs %ld, forgets %ld, attached back %ld, repeated results not logged %ld | START refused "
                 "x%lld, ADD-BULLET refused x%lld.",
                 XStepName(st->xm.step()), drew, g_xbBoltMovedModel ? "MOVED (on the groove)" : "at rest", g_xbCounts[1], g_xbCounts[2],
                 g_xbCounts[3], g_xbCounts[4], g_xbCounts[5], g_xbCounts[6], g_xbCounts[7], g_xbCounts[8], g_xbCounts[9],
                 g_xbCounts[10], g_xbCounts[11], static_cast<long long>(g_xbowShowCalls), g_xbCarry.takes, g_xbCarry.giveBacks,
                 g_xbCarry.forgets, g_xbCarry.retakes, g_xbCarryRepeatsTotal, static_cast<long long>(g_startRefused),
                 static_cast<long long>(g_addRefused));
}

// ===== THE PUMP SHOTGUN (plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 4; spec 2026-11-04-manual-reload-pump-
// shotgun-design, INV-677) =====
// wn_shotgun_b / f (ItemType_Shotgun 24, AnimPrefix shotgun_pa): shells taken at the LEFT HIP (the pouch flag the guns use, ruling R1),
// pushed one at a time into the loading port (Jay's dot) - one 36-frame cycle of Jay's C13 animation per push, the game's own add-bullet at
// cycle frame 23 - and racked by hand: the rack hold IS the two-hand grip (ruling P5), the stroke = the left CONTROLLER's travel along the
// pump's world axis since the grip latched. The steps are pump_logic.h (PMachine, unit-tested); this draws bone_reload_shotgun EVERY update
// (also after a shot - the game's shoot clip would rack it, spec 4.6), draws the gun's own shell ammo_bullet at l_hand (plan P2 (b): one
// shell; ruling R11: l_hand world x the frame's shell matrix) and writes it back to its rest ONCE when it stops (ruling R3), withholds R2
// while not chambered, silences the game's per-shot rack sound (pumpsound.h) and publishes the anim frame (the finger pass) and the stroke
// (the two-hand pin slide). Element ids resolve BY NAME once per draw (Task 0: the node order is static data only).
constexpr long      kMaxPumpLines = 200;        // [PUMP] event / draw lines a run (INV-9)
constexpr long      kMaxPumpRefuseLines = 40;   // [PUMP] REFUSED / fault lines a run
constexpr ULONGLONG kPumpPeriodMs = 5000;       // the counts line (diag_periodic_logs)
constexpr ULONGLONG kPumpPublishFreshMs = 150;  // the anim frame / stroke readers' staleness (the hand poses' rule)
struct PumpDraw {
    const reload::PumpRow* row = nullptr;
    int   rootId = -1, pumpId = -1, ammoId = -1, hbId = -1, hlId = -1;   // bone_root, bone_reload_shotgun, ammo_bullet, holder_bullet, holderl
    bool  fresh = false;          // the first update of this draw (PInput::freshDraw)
    bool  gripWas = false;        // grippedPump last update (the stroke's latch edge)
    reload::PStroke stroke;       // final fix wave (I1 / #19): (left - right) . axis from the grip's latch; a stale sample holds
    // PS1 fixes (R18): the port lock
    const reload::HandRow* lockRow = nullptr;   // this mesh's push pose as a HandRow (handR / handT only - g_pumpLockRows), null = no lock
    bool  snapped = false;
    bool  snapArmed = true;       // PS1-fix review C1: false after a release until the RENDERED hand's shell is seen beyond 1.5 x
                                  // reload_snap_m (the drawn hand still sits at the lock while it blends out - no re-snap, no phantom push)
    bool  haveLr0 = false;
    float lr0L[3] = {};           // (left - right) at the snap, in bone_root's axes (I1: a turn of the gun with both hands cancels)
    float v0L[3] = {};            // the hand's shell minus the port at the snap, in bone_root's axes
    float d0 = 0.0f, est = 0.0f;  // the shell's distance from the port at the snap / estimated now
    int   lastCycle = 0;          // the machine's animCycle last update (a cycle playing keeps the lock)
    float strokeMaxFrac = 0.0f;   // the deepest stroke of this hold (the RACK SHORT line)
    bool  takeWas = false;        // (grip held AND in the hip zone) last update - the spare is read only on its rising edge
    bool  writeFaultSaid = false;
};
PumpDraw    g_pd;                          // game thread
// Ruling R3 + fix round 1 (review M1, ruling R15): the models whose ammo_bullet WE moved to the hand, EACH with its own ids and row (a
// straight b -> f switch keeps b's record; b's shell goes home at b's next draw). A model that is not held is never written (the
// crossbow's model-changed rule). Game thread. 4 slots (3 pump items exist); a full table drops the oldest, named.
struct PumpShellMoved { uintptr_t model = 0; const reload::PumpRow* row = nullptr; int ammoId = -1, hbId = -1; ULONGLONG ms = 0; };
constexpr int kPumpShellSlots = 4;
PumpShellMoved g_pumpShells[kPumpShellSlots];
bool        g_pumpSilencedSet = false;     // PumpSoundSetSilenced(true) was sent (game thread)
// Fix round 1 (review M2, ruling R15): the pump left the right hand (no model / another model) at g_pumpAwayMs; its hand / cycle / stroke
// are cleared only once it has been away > kPumpAwayMs (the bow's stale guard), so a blip keeps them. Game thread.
constexpr ULONGLONG kPumpAwayMs = 150;
uintptr_t   g_pumpAwayModel = 0, g_pumpAwayItem = 0;
ULONGLONG   g_pumpAwayMs = 0;              // 0 = not away
const char* g_pumpAwayWhy = "";
// Fix round 1 (review C1) + final fix wave (I1, ruling R17): EACH controller's grip point in TRACKING space ([0] right, [1] left),
// published by its arm solve BEFORE the two-hand pin moves the wrist target (boneblend.cpp ReloadNoteController) - the rack stroke's only
// source: (left - right) . the pump's back axis. Written only while a pump is driven (g_pumpWantCtl).
volatile LONG g_pumpWantCtl = 0;
SRWLOCK     g_lcLock = SRWLOCK_INIT;
float       g_lcPos[2][3] = {};
ULONGLONG   g_lcMs[2] = {};
uintptr_t   g_pumpLastItem = 0, g_pumpLastModel = 0;   // final fix wave (#17): the last pump drawn - "same item on a new model" is named
// PS1 fixes (R18): the port lock's rows as HandRows (only handR / handT are read - reload::LockedHand), one per kPumpLockRows row, built
// once; reload.cpp's own pointers (the lock's consumers ReloadPinPos / ReloadPinRot are in this file - no cross-TU table). Game thread.
reload::HandRow g_pumpLockRows[reload::kPumpLockRowCount] = {};
bool        g_pumpLockRowsBuilt = false;
long        g_pumpSnaps = 0, g_pumpReleases = 0;   // the 5 s counts line
bool        g_pumpArmedSaid = false;
long        g_nPump = 0;                   // game thread
volatile LONG g_nPumpRefuse = 0;
long        g_pumpCounts[12] = {};         // per PEvent, the counts line
ULONGLONG   g_pumpLastPeriod = 0;
long long   g_pumpEpochQ = 0;              // the frame clock's zero in quarter-ms (PumpClockQ; the first pump update of the session)

// PS2 smoothing (Jay: the loading animation is "framey"): the frame clock and the finger pass's word run on QueryPerformanceCounter in
// quarter-ms (GetTickCount64 steps ~15.6 ms = ~2 C13 frames at 0.3 s a cycle). COST: one QPC call (~20-30 ns) + a multiply; the
// frequency is read once.
long long PumpClockQ()
{
    static long long s_freq = 0;
    LARGE_INTEGER c;
    if (!s_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_freq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    QueryPerformanceCounter(&c);
    return static_cast<long long>(static_cast<double>(c.QuadPart) * 4000.0 / static_cast<double>(s_freq));
}
const char* g_pumpSndRefused[4] = {};      // sounds already named REFUSED (one line each)
// Published to the finger pass (any thread): (ms stamp << 16) | (cycle << 8) | frame; 0 = no anim. One interlocked store per update.
volatile LONG64 g_pumpAnimWord = 0;
// Published to the two-hand pin (the solve's thread): the drawn stroke (m) and the pump's BACK axis in holderl's frame. Shared SRW lock.
SRWLOCK     g_pumpLock = SRWLOCK_INIT;
bool        g_pumpStrokeOn = false;
float       g_pumpStrokeM = 0.0f;
float       g_pumpAxisHl[3] = {};
ULONGLONG   g_pumpStrokeMs = 0;
// The finger pass's own counts (boneblend.cpp ReloadFingersApply -> ReloadNotePumpFingerPass): proof the anim was APPLIED.
volatile LONG64 g_pumpFingerPasses = 0, g_pumpFingerPins = 0;
bool PumpLine() { return ++g_nPump <= kMaxPumpLines; }
bool PumpRefuseLine() { return InterlockedIncrement(&g_nPumpRefuse) <= kMaxPumpRefuseLines; }

const char* PEventName(reload::PEvent e)
{
    switch (e) {
    case reload::PEvent::None: return "None";
    case reload::PEvent::Take: return "Take";
    case reload::PEvent::TakeNoShell: return "TakeNoShell";
    case reload::PEvent::PortPush: return "PortPush";
    case reload::PEvent::PortFull: return "PortFull";
    case reload::PEvent::ShellIn: return "ShellIn";
    case reload::PEvent::Shot: return "Shot";
    case reload::PEvent::RackBack: return "RackBack";
    case reload::PEvent::RackForward: return "RackForward";
    case reload::PEvent::RackShort: return "RackShort";
    case reload::PEvent::Chambered: return "Chambered";
    case reload::PEvent::HandCleared: return "HandCleared";
    }
    return "?";
}

// The anim frame / stroke readers see nothing (the pump is not driven this update). COST: two stores + one SRW acquire.
void PumpUnpublish()
{
    InterlockedExchange64(&g_pumpAnimWord, 0);
    AcquireSRWLockExclusive(&g_pumpLock);
    g_pumpStrokeOn = false;
    ReleaseSRWLockExclusive(&g_pumpLock);
}

// The pump is not driven this update (no model / another model / manual_reload off): the game's rack sound comes back and nothing is
// published. COST: one bool test unless the pump was driven.
void PumpDropLive()
{
    if (!g_pumpSilencedSet) { return; }
    PumpSoundSetSilenced(false);
    g_pumpSilencedSet = false;
    InterlockedExchange(&g_pumpWantCtl, 0);
    PumpUnpublish();
    g_pd.snapped = false;   // review M2: a manual_reload toggle while locked leaves no stale lock record (re-arm on the drawn hand)
    g_pd.snapArmed = false;
}

// The left controller's grip point published by the solve (fix round 1, C1), if fresh (150 ms). COST: one shared SRW + a 12-byte copy.
// Final fix wave (I1, R17): the left-minus-right tracking vector, if BOTH sides are fresh (150 ms). False = either side stale -> the caller
// HOLDS the stroke (#19). COST: one shared SRW + a 24-byte copy.
bool PumpControllersLR(float lr[3])
{
    AcquireSRWLockShared(&g_lcLock);
    const ULONGLONG t = GetTickCount64();
    const bool ok = g_lcMs[0] != 0 && g_lcMs[1] != 0 && t - g_lcMs[0] <= kPumpPublishFreshMs && t - g_lcMs[1] <= kPumpPublishFreshMs;
    if (ok) {
        for (int k = 0; k < 3; ++k) { lr[k] = g_lcPos[1][k] - g_lcPos[0][k]; }
    }
    ReleaseSRWLockShared(&g_lcLock);
    return ok;
}

// The moved-shell record for a model (fix round 1, M1); null = its shell is at rest. COST: 4 compares.
PumpShellMoved* PumpShellFor(uintptr_t model)
{
    if (!model) { return nullptr; }
    for (PumpShellMoved& s : g_pumpShells) {
        if (s.model == model) { return &s; }
    }
    return nullptr;
}
bool PumpShellAny()
{
    for (const PumpShellMoved& s : g_pumpShells) {
        if (s.model) { return true; }
    }
    return false;
}
// The shell on THIS draw's model was written at the hand: record it with this draw's ids (once per model; refreshed each write - cheap).
void PumpShellMark(uintptr_t model, ULONGLONG now)
{
    PumpShellMoved* s = PumpShellFor(model);
    if (!s) {
        int at = 0;
        for (int i = 0; i < kPumpShellSlots; ++i) {
            if (!g_pumpShells[i].model) { at = i; break; }
            if (g_pumpShells[i].ms < g_pumpShells[at].ms) { at = i; }
        }
        if (g_pumpShells[at].model && PumpRefuseLine()) {
            // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("reload: [PUMP] SHELL RECORD DROPPED - model %p: %d pump models already hold a moved shell; the oldest record is "
                "dropped (its shell stays where the hand left it until the game re-poses it).",
                reinterpret_cast<void*>(g_pumpShells[at].model), kPumpShellSlots);
        }
        s = &g_pumpShells[at];
    }
    s->model = model;
    s->row = g_pd.row;
    s->ammoId = g_pd.ammoId;
    s->hbId = g_pd.hbId;
    s->ms = now;
}

// A pump sound the shared player could not play: one line per sound.
void PumpSoundRefused(const char* name)
{
    for (const char*& s : g_pumpSndRefused) {
        if (s == name) { return; }
        if (!s) {
            s = name;
            // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("reload: [PUMP] SOUND REFUSED - \"%s\" was not played (gunsound's replica of the 2D PlaySound export) - the step goes on "
                "silent.", name ? name : "(null)");
            return;
        }
    }
}

// Ruling R3: ammo_bullet back at its REST (holder_bullet's live world x the row's mesh-data local), ONCE, when the pump stops drawing it at
// the hand. Only on a model that has a record (fix round 1, M1: per model, with that model's own ids), and only called while that model
// is the held one (never a write to a model that may be gone). COST: 4 compares unless moved; then one world read + one write + one line.
// quiet (2026-11-08, C14 - Jay: "Fix the log lines"): a SUCCESSFUL restore says nothing and takes no PumpLine slot - used only for the
// planned empty hand inside a loading cycle (cycle frames 24-28: the pushed shell is in the gun, the next one not yet under the thumb),
// which happens on every push. A failed restore (*** NOT RESTORED ***) is always logged.
void PumpRestoreShell(uintptr_t model, const char* why, bool quiet = false)
{
    PumpShellMoved* s = PumpShellFor(model);
    if (!s) { return; }
    float hb[12], w[12];
    const bool ok = s->row && s->ammoId >= 0 && ElemMtx(g_world, model, s->hbId, hb) &&
                    (Mul34(hb, s->row->ammoLocal, w), SetWorld(model, s->ammoId, w));
    if (ok) { *s = PumpShellMoved{}; }
    if (!(ok && quiet) && (ok ? PumpLine() : PumpRefuseLine())) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] SHELL %s (%s) - model %p: ammo_bullet %s.", ok ? "RESTORED" : "*** NOT RESTORED ***", why,
            reinterpret_cast<void*>(model), ok ? "is back at its rest under holder_bullet" : "is still at the hand (retried on the next chance)");
    }
}

// The pump is gone for good (away > kPumpAwayMs, or manual_reload off): the hand's shells are gone, no add-bullet can follow (the tick no
// longer drives it - Review Focus 1), the machine is reset so its next draw starts clean, the silencer and the publishes drop.
// COST: one compare unless a pump was away.
void PumpClearAway()
{
    // Final fix wave (#17): never reset an item that a pump draw is driving NOW (the same item back on another model is resumed by
    // ClassifyPumpDraw, which clears this record - this is the belt to that brace). manual_reload off always resets.
    const bool itemDrivenNow = ManualReload && g_gun.built && g_gun.family == reload::Family::Pump && g_gun.item == g_pumpAwayItem;
    if (g_pumpAwayItem && !itemDrivenNow) {
        for (ItemState& s : g_items) {
            if (s.item != g_pumpAwayItem) { continue; }
            const int had = s.pm.handShells();
            if (had > 0 && PumpLine()) {
                // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
                LOG("reload: [PUMP] HAND CLEARED - %s (away > %llu ms): %d shell(s) gone from the hand, no add-bullet after (spares "
                    "untouched - they move only on an add-bullet).", g_pumpAwayWhy, static_cast<unsigned long long>(kPumpAwayMs), had);
            }
            reload::PInput off{};   // manualOn false: PMachine::Reset - no cycle, no hand, chambered re-derived on the next draw
            s.pm.Update(off, reload::PParams{});
        }
    }
    // Another pump driven now (a straight b -> f switch) keeps its own silencer / publishes / draw state; otherwise they drop.
    const bool otherPumpDriven = g_gun.built && g_gun.family == reload::Family::Pump && g_gun.model != g_pumpAwayModel;
    // a later driven update of the same model (manual_reload back on) is a fresh draw: chambered = rounds > 0
    if (g_gun.model == g_pumpAwayModel) { g_pd.fresh = true; }
    g_pumpAwayModel = 0;
    g_pumpAwayItem = 0;
    g_pumpAwayMs = 0;
    if (!otherPumpDriven) { PumpDropLive(); }
}

// The held model is no longer this pump (put away / switched / a blip with no model): fix round 1 (M2) - NOT cleared yet. The time is
// recorded and PumpAwayTick clears it once the pump has been away > kPumpAwayMs; the same pump back within that keeps its hand and cycle
// (ClassifyModel resumes it). The stroke is unpublished at once (the two-hand pin must not slide another gun). COST: one compare.
void PumpLeave(const char* why)
{
    if (g_gun.family != reload::Family::Pump || !g_gun.built || !g_gun.item) { return; }
    const ULONGLONG now = GetTickCount64();
    if (g_pumpAwayMs && g_pumpAwayModel != g_gun.model) { PumpClearAway(); }   // an earlier pump still pending: cleared now (one record)
    if (!g_pumpAwayMs) {
        g_pumpAwayModel = g_gun.model;
        g_pumpAwayItem = g_gun.item;
        g_pumpAwayMs = now ? now : 1;
        g_pumpAwayWhy = why;
    }
    AcquireSRWLockExclusive(&g_pumpLock);
    g_pumpStrokeOn = false;
    ReleaseSRWLockExclusive(&g_pumpLock);
}

// Once per ReloadTick (fix round 1, M2): a pump away longer than kPumpAwayMs is cleared. COST: one compare while nothing is away.
void PumpAwayTick(ULONGLONG now)
{
    if (!g_pumpAwayMs || now - g_pumpAwayMs <= kPumpAwayMs) { return; }
    PumpClearAway();
}

// Classify a held pump (ClassifyModel, once per model change): the row, the five elements by NAME and their parents, the fx row. Any
// missing -> REFUSED by name and false: the game reloads it as today this draw. The ARMED line (first pump draw of the session) is
// logged unconditionally, refused or not.
bool ClassifyPumpDraw(uintptr_t model, uintptr_t item, const reload::Classified& c, const char* meshNorm, const char* meshShown,
                      const char* prefix, int pumpId, int ammoId, ULONGLONG now)
{
    // Fix round 1 (M2): the SAME pump back within kPumpAwayMs (a blip, or a switch straight back) RESUMES - its hand, cycle, chambered
    // flag (the item's machine, untouched while away) and this draw's stroke latch are kept; no fresh draw. Only this pump's own g_pd
    // can still be here (another pump's draw clears the away record first - PumpLeave).
    // Final fix wave (#17): keyed on the ITEM - the same item on a NEW model object resumes too (only the model changed; the engine may
    // re-create it). The old model's shell record stays (never written - it may be gone). Named in one line either way (below).
    const bool resume = g_pumpAwayMs && item == g_pumpAwayItem;
    const bool newModel = (resume && model != g_pumpAwayModel) || (!resume && item == g_pumpLastItem && model != g_pumpLastModel);
    const uintptr_t oldModel = resume ? g_pumpAwayModel : g_pumpLastModel;
    const ULONGLONG awayFor = resume ? now - g_pumpAwayMs : 0;
    const PumpDraw keep = g_pd;
    g_pd = PumpDraw{};
    PumpDraw& d = g_pd;
    d.row = c.prow;
    d.pumpId = pumpId;
    d.ammoId = ammoId;
    ElemId(model, "bone_root", &d.rootId);
    ElemId(model, "holder_bullet", &d.hbId);
    ElemId(model, "holderl", &d.hlId);
    int pp = -1, pa = -1, ph = -1;
    ElemParent(model, pumpId, &pp);
    ElemParent(model, ammoId, &pa);
    ElemParent(model, d.hbId, &ph);
    const reload::FxRow* fx = meshNorm ? reload::FindFxRow(meshNorm) : nullptr;   // once per draw (a walk of ~24 rows)
    // 2026-11-07: a row that does not rack (the semi-auto wn_shotgun_c, Jay "No rack") has no pump bone and needs only the seat sound.
    const bool racks = d.row && d.row->racks;
    const char* why = !d.row ? "no pump row"
                    : d.rootId < 0 ? "the mesh has no bone_root"
                    : d.hbId < 0 ? "the mesh has no holder_bullet"
                    : (racks && pp != d.rootId) ? "bone_reload_shotgun is not a child of bone_root (the row's rest is in bone_root's frame)"
                    : pa != d.hbId ? "ammo_bullet is not a child of holder_bullet (the row's rest is in holder_bullet's frame)"
                    : ph != d.rootId ? "holder_bullet is not a child of bone_root"
                    : !fx ? "no fx row for this mesh (reload_fx_table.inc)"
                    : (racks && (fx->nSeat < 1 || fx->nRack < 1 || fx->nMagOut < 1)) ? "the fx row lacks a seat / rack / move sound"
                    : (!racks && fx->nSeat < 1) ? "the fx row lacks a seat sound"
                    : nullptr;
    if (!g_pumpArmedSaid) {
        g_pumpArmedSaid = true;
        const LONG vg = g_vrGripSeen;
        const char* hipOff = HolsterLeftTakeRefusal();
        // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [PUMP] ARMED (first pump draw, logged unconditionally) - manual_reload=%d | row %s (mesh '%s', prefix '%s') | elements "
            "by name: bone_root %d, bone_reload_shotgun %d (parent %d), ammo_bullet %d (parent %d), holder_bullet %d (parent %d), holderl %d "
            "| port (%+.4f %+.4f %+.4f), pump rest (%+.4f %+.4f %+.4f), axis back (%+.3f %+.3f %+.3f), travel %.4f m, rack done at %.3f of "
            "it (reload_rack_pull_m %.3f / %.4f) | port push %.3f m, re-arm beyond %.3f m (reload_snap_m) | fx row %s | rack-sound "
            "silencer %s | vr_grip %s | left zones %s%s | this draw: %s",
            ManualReload ? 1 : 0, d.row ? d.row->mesh : "NONE", meshShown, prefix, d.rootId, pumpId, pp, ammoId, pa, d.hbId, ph, d.hlId,
            d.row ? d.row->port.x : 0.0f, d.row ? d.row->port.y : 0.0f, d.row ? d.row->port.z : 0.0f, d.row ? d.row->pumpRest.x : 0.0f,
            d.row ? d.row->pumpRest.y : 0.0f, d.row ? d.row->pumpRest.z : 0.0f, d.row ? d.row->pumpAxis.x : 0.0f,
            d.row ? d.row->pumpAxis.y : 0.0f, d.row ? d.row->pumpAxis.z : 0.0f, d.row ? d.row->travelM : 0.0f,
            ReloadRackPullM / reload::kPistolSlideTravelM, ReloadRackPullM, reload::kPistolSlideTravelM, ReloadSnapM, 1.5f * ReloadSnapM,
            fx ? fx->mesh : "NONE", PumpSoundInstall() ? "LIVE" : "OFF (its own line says why - the game's rack sound stays)",
            vg < 0 ? "not polled yet" : vg ? "1" : "*** 0 - the left grip is not read: no take, no push, no rack ***",
            hipOff ? "*** REFUSED: " : "not refused", hipOff ? hipOff : "", why ? why : "manual");
    }
    if (why) {
        if (PumpRefuseLine()) {
            // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [PUMP] REFUSED model %p item %p (mesh '%s'): %s - the pump reloads as today this draw.", reinterpret_cast<void*>(model),
                reinterpret_cast<void*>(item), meshShown, why);
        }
        return false;
    }
    d.fresh = !resume;
    // PS1 fixes (R18): this mesh's port-lock row (reload_pump_lock.inc), as a HandRow for the existing lock path. Once per draw.
    if (!g_pumpLockRowsBuilt) {
        for (int i = 0; i < reload::kPumpLockRowCount; ++i) {
            g_pumpLockRows[i].key = reload::kPumpLockRows[i].mesh;
            memcpy(g_pumpLockRows[i].handR, reload::kPumpLockRows[i].handR, sizeof(g_pumpLockRows[i].handR));
            memcpy(g_pumpLockRows[i].handT, reload::kPumpLockRows[i].handT, sizeof(g_pumpLockRows[i].handT));
        }
        g_pumpLockRowsBuilt = true;
    }
    {
        const reload::PumpLockRow* lr = meshNorm ? reload::PumpLockFor(meshNorm) : nullptr;
        d.lockRow = lr ? &g_pumpLockRows[lr - reload::kPumpLockRows] : nullptr;
    }
    if (resume) {
        d.gripWas = keep.gripWas;
        d.stroke = keep.stroke;
        d.strokeMaxFrac = keep.strokeMaxFrac;
        d.takeWas = keep.takeWas;
        g_pumpAwayModel = 0;   // back in the hand: nothing to clear
        g_pumpAwayItem = 0;
        g_pumpAwayMs = 0;
    }
    if (newModel && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=no
        LOG("reload: [PUMP] SAME ITEM ON A NEW MODEL - item %p was on model %p, now on %p: %s.", reinterpret_cast<void*>(item),
            reinterpret_cast<void*>(oldModel), reinterpret_cast<void*>(model),
            resume ? "RESUMED within the away guard - the hand, the cycle and chambered kept"
                   : "a fresh draw (it was away > 150 ms and cleared - chambered = rounds > 0, plan P1)");
    }
    g_pumpLastItem = item;
    g_pumpLastModel = model;
    if (PumpLine()) {
        char res[160] = "";
        if (resume) {
            wsprintfA(res, " (RESUMED after %u ms away - within the %u ms guard: the hand, the cycle and chambered kept)",
                      static_cast<unsigned>(awayFor), static_cast<unsigned>(kPumpAwayMs));
        }
        // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [PUMP] DRAW model %p item %p (mesh '%s'): manual%s - %s, capacity %d - pump %d, shell ammo_bullet %d, holder_bullet "
            "%d, holderl %d%s; the rests are the row's (mesh data): pump t (%+.4f %+.4f %+.4f), shell t (%+.4f %+.4f %+.4f) under "
            "holder_bullet | port lock %s (hand bone t %+.4f %+.4f %+.4f in the gun frame) | load cycle %.2f s.",
            reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown, res,
            d.row->racks ? "RACKS (by hand, after every shot)" : "NO RACK (the gun cycles itself; chambered = a round in the tube)",
            d.row->capacity, pumpId, ammoId, d.hbId, d.hlId,
            d.hlId < 0 ? " (*** no holderl: the two-hand pin does not slide with the pump ***)" : "", d.row->pumpLocal[3],
            d.row->pumpLocal[7], d.row->pumpLocal[11], d.row->ammoLocal[3], d.row->ammoLocal[7], d.row->ammoLocal[11],
            d.lockRow ? d.lockRow->key : "*** NONE (no reload_pump_lock.inc row) - no snap ***", d.lockRow ? d.lockRow->handT[0] : 0.0f,
            d.lockRow ? d.lockRow->handT[1] : 0.0f, d.lockRow ? d.lockRow->handT[2] : 0.0f, reload::kPumpCycleS);
    }
    return true;
}

// ammo_bullet written at w with its basis columns kept at the LENGTHS the element has now (XbowWriteBolt's rule: the scale is never ours
// to change; a read-back of our own write has the same lengths). COST: one world read + one write.
bool PumpWriteShell(uintptr_t model, int id, float w[12])
{
    float live[12];
    if (ElemMtx(g_world, model, id, live)) {
        for (int c = 0; c < 3; ++c) {
            const float lw = Len3(w[c], w[4 + c], w[8 + c]), ll = Len3(live[c], live[4 + c], live[8 + c]);
            if (lw > 1e-6f && ll > 1e-6f && std::isfinite(ll)) {
                const float k = ll / lw;
                w[c] *= k;
                w[4 + c] *= k;
                w[8 + c] *= k;
            }
        }
    }
    return SetWorld(model, id, w);
}

// One update of the held pump (ReloadTick, game thread). Returns what was drawn.
// [LOADBEARING: manual reload pump] (manual_reload) every pump step: R2 withheld while not chambered, the pump bone drawn where the hand
//      puts it (rest otherwise - the game's shoot clip would rack it), the shell at the hand, the game's add-bullet per pushed shell.
// COST per update with a pump held: one rounds call, one bone_root world read, one SetElementWorldMatrix (the pump), the step machine,
// two interlocked stores (the anim word, R2 / the press publish); + the spare (2 game calls) on the hip take edge only; + one l_hand world
// read and two 3x4 products while shells are held / a cycle plays, + one world read + one write for the shell; + one holderl world read
// and one SRW acquire while the two-hand grip holds the pump. Events only: the add-bullet (+ 4 game calls for the line), a pulse, up to 2
// sound calls.
const char* DrivePump(uintptr_t ctrl, ItemState* st, void* playerModel, int lHandId, ULONGLONG now)
{
    using reload::PEvent;
    PumpDraw& d = g_pd;
    const reload::PumpRow& row = *d.row;
    const uintptr_t model = g_gun.model;
    if (!g_pumpSilencedSet) {   // the game's per-shot rack sound is ours while this pump is driven (pumpsound.h). COST: one bool test.
        PumpSoundSetSilenced(true);
        g_pumpSilencedSet = true;
        InterlockedExchange(&g_pumpWantCtl, 1);   // fix round 1 (C1): the left solve publishes the controller's grip point from now on
    }
    const long long nowQ = PumpClockQ();
    if (!g_pumpEpochQ) { g_pumpEpochQ = nowQ; }
    float rootW[12];
    const bool haveRoot = ElemMtx(g_world, model, d.rootId, rootW);
    float axisW[3] = { 0.0f, 0.0f, -1.0f };
    const bool haveAxis = haveRoot && AxisOf(rootW, row.pumpAxis, axisW);
    const int handBefore = st->pm.handShells();

    reload::PInput in{};
    in.manualOn = ManualReload;
    in.isPump = true;
    in.freshDraw = d.fresh;
    d.fresh = false;
    in.gripDown = g_gripDown != 0;
    in.inHip = HolsterLeftPouchHeld();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    // The spare: only on the rising edge of (grip held AND in the hip zone) - the update the machine's take may fire on (ruling R1).
    const bool takeNow = in.gripDown && in.inHip;
    if (takeNow && !d.takeWas) { st->spare = SpareOf(ctrl, st->item); }
    d.takeWas = takeNow;
    in.spare = st->spare;
    // The machine's parameters - built BEFORE the port test, whose CanPush must use this gun's capacity (2026-11-07: c holds 8, the pumps
    // 6; CanPush's default was 6). capacity + racks are the ROW's. COST: a few stores.
    reload::PParams p;
    const float done = ReloadRackPullM / reload::kPistolSlideTravelM;   // the SMG share rule (no new key)
    p.rackDoneFrac = done > 1.0f ? 1.0f : done;
    p.cycleFrames = reload::kPumpCycleFrames;
    p.capacity = row.capacity;
    p.racks = row.racks;
    // The PUSH (plan P3): the hand's shell point (the rendered l_hand x C13 frame 1's shell 1 centre) vs Jay's port through bone_root.
    // Measured only while shells are held (nothing else reads it).
    float lh[12];
    const bool haveHand = playerModel && lHandId >= 0 && ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh);
    float lr[3] = {};
    const bool haveLR = PumpControllersLR(lr);   // (left - right) in tracking space - the rack stroke and the port lock's release
    float toPort = -1.0f;
    const bool holding = handBefore > 0 || d.lastCycle > 0;
    // THE PORT LOCK (PS1 fixes, ruling R18 - Jay: "Snap + buzz"): the hand's shell entering the port (<= reload_snap_m) while shells are
    // held SNAPS the drawn left hand onto the gun at the approved push pose (reload_pump_lock.inc, HandsPublish below - the 100 ms lock
    // blend in / out) with one left pulse. While locked the RENDERED hand sits at the port, so the distance that releases it (and re-arms
    // the next push) is ESTIMATED from the controllers (PS1-fix review I1): |(shell - port at the snap) + how far (left - right) has moved
    // since|, both as VECTORS in bone_root's axes - pushing deeper does not count against it, and turning the gun with both hands (or a
    // smooth turn) cancels, as in the rack stroke. Released beyond 1.5 x reload_snap_m, on a grip release, or once the hand is empty and
    // no cycle plays. After a release no new snap or push is taken until the RENDERED hand's shell has been seen beyond 1.5 x
    // reload_snap_m (review C1: the drawn hand is still at the lock while it blends out). The push itself is unchanged: one shell per
    // entry, fired on the same update as the snap.
    // COST: off the lock one compare; locked two 3x3 direction products + a distance per update; the snap / release are events.
    const char* release = nullptr;
    if (d.snapped) {
        float lrL[3];
        if (haveLR && haveRoot && DirToLocal(rootW, lr, lrL)) {
            if (!d.haveLr0) { memcpy(d.lr0L, lrL, sizeof(d.lr0L)); d.haveLr0 = true; }
            d.est = Len3(d.v0L[0] + lrL[0] - d.lr0L[0], d.v0L[1] + lrL[1] - d.lr0L[1], d.v0L[2] + lrL[2] - d.lr0L[2]);
        }   // a stale controller pair (or no bone_root) holds the last estimate (the lock never releases on staleness)
        toPort = d.est;
        in.inPort = d.est <= ReloadSnapM;
        in.outOfPort = d.est > 1.5f * ReloadSnapM;
        release = in.outOfPort ? "the hand left the port (beyond 1.5 x reload_snap_m, from the controllers)"
                : !in.gripDown ? "the grip let go"
                : !holding ? "the hand is empty and no cycle plays"
                : !haveRoot ? "no bone_root world"
                : nullptr;
    } else if (handBefore > 0 && haveHand && haveRoot) {
        const float* s1 = reload::PumpAnimFrame(1, 1).shell[0];
        const reload::Vec3 s1c{ s1[3], s1[7], s1[11] };
        float sp[3], pw[3];
        XformPt(lh, s1c, sp);
        XformPt(rootW, row.port, pw);
        const float vw[3] = { sp[0] - pw[0], sp[1] - pw[1], sp[2] - pw[2] };
        toPort = Len3(vw[0], vw[1], vw[2]);
        if (!d.snapArmed && toPort > 1.5f * ReloadSnapM) { d.snapArmed = true; }
        // C1: not while the drawn hand is still coming off the lock. PS2 (Jay: "No snap until ready"): not while the gun cannot take a
        // shell (a cycle plays, the tube is full, the push is disarmed) - the entry is withheld (no lock, no buzz, the machine stays
        // armed), so the snap + push land on the first update the gun is ready with the hand still at the port. COST: 4 compares.
        in.inPort = d.snapArmed && toPort <= ReloadSnapM && st->pm.CanPush(in.rounds, p);
        in.outOfPort = toPort > 1.5f * ReloadSnapM;
        float v0L[3];
        if (in.inPort && in.gripDown && d.lockRow && DirToLocal(rootW, vw, v0L)) {
            d.snapped = true;
            d.d0 = toPort;
            d.est = toPort;
            memcpy(d.v0L, v0L, sizeof(d.v0L));
            float lrL[3];
            d.haveLr0 = haveLR && DirToLocal(rootW, lr, lrL);
            if (d.haveLr0) { memcpy(d.lr0L, lrL, sizeof(d.lr0L)); }
            ++g_pumpSnaps;
            if (HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
            if (PumpLine()) {
                // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
                LOG("reload: [PUMP] SNAP - the hand's shell %.3f m from the port (reload_snap_m %.3f): the left hand LOCKED to the push pose "
                    "(%s, 100 ms blend); %s; %d shell(s) in the hand.", toPort, ReloadSnapM, d.lockRow->key,
                    HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)", handBefore);
            }
        }
    }
    if (release) {
        d.snapped = false;
        d.snapArmed = false;   // C1: re-armed only once the drawn hand's shell is seen beyond 1.5 x reload_snap_m
        ++g_pumpReleases;
        if (PumpLine()) {
            // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [PUMP] SNAP RELEASED - %s (estimated %.3f m from the port): the hand eases back to the controller (100 ms).",
                release, d.est);
        }
    }
    // THE RACK (plan P5): the two-hand grip on this pump. Final fix wave (final review I1, ruling R17): the stroke = (LEFT controller -
    // RIGHT controller), both in the SAME tracking space (boneblend.cpp ReloadNoteController: Ry(bodyYaw) x (controller - headRep), each
    // published BEFORE the two-hand pin moves the wrist target - fix round 1, C1), projected on the pump's BACK axis (direction only, from
    // bone_root's world - its position lags ~0.3 m in motion, INV-48, and is never used), from its value at the grip's first fresh sample,
    // clamped 0 .. travel (pump_logic.h PStroke). Locomotion, turning and moving the gun with both hands cancel; only the left hand sliding
    // back along the gun relative to the right racks. #19: a stale sample on EITHER controller (or no bone_root) HOLDS the last stroke.
    const bool fresh = haveLR && haveAxis;
    const float sNow = fresh ? lr[0] * axisW[0] + lr[1] * axisW[1] + lr[2] * axisW[2] : 0.0f;
    // 2026-11-07: a gun that does not rack (the semi-auto) never reports a grip on a pump - no stroke, no pin slide, no rack events;
    // the two-hand grip itself is untouched (twohand.cpp holds it as today).
    const bool gripped = row.racks && TwoHandGripping();
    if (gripped && !d.gripWas) { d.strokeMaxFrac = 0.0f; }
    d.gripWas = gripped;
    const float strokeM = d.stroke.Update(gripped, fresh, sNow, row.travelM);   // COST: a few compares
    const float strokeFrac = row.travelM > 1e-6f ? strokeM / row.travelM : 0.0f;
    if (gripped && strokeFrac > d.strokeMaxFrac) { d.strokeMaxFrac = strokeFrac; }
    in.grippedPump = gripped;
    in.strokeFrac = strokeFrac;
    // PS1 fixes (Jay: "0.3 s per shell (4x)"): one 36-frame cycle in kPumpCycleS (pump_logic.h PumpFrameAt; the add-bullet at frame
    // 23 = 0.183 s after the push). PS2 smoothing: the clock is PumpClockQ (1 ms steps into PumpFrameAt, not ~15.6 ms), and the
    // fraction past frameNow is kept for the blended draw (frameFrac). COST: a few multiplies and a divide.
    const long long elapsedQ = nowQ - g_pumpEpochQ > 0 ? nowQ - g_pumpEpochQ : 0;
    in.frameNow = reload::PumpFrameAt(static_cast<unsigned long long>(elapsedQ / 4), reload::kPumpCycleS, reload::kPumpCycleFrames);
    float frameFrac = static_cast<float>(static_cast<double>(elapsedQ) * 0.25 * reload::kPumpCycleFrames /
                                         (static_cast<double>(reload::kPumpCycleS) * 1000.0) - static_cast<double>(in.frameNow));
    if (!(frameFrac >= 0.0f)) { frameFrac = 0.0f; }
    if (frameFrac > 0.999f) { frameFrac = 0.999f; }

    const reload::POut o = st->pm.Update(in, p);

    // THE ADD-BULLET (cycle frame 23): the game's own, through the trampoline (the crossbow's seat). Synchronous (ledger R8).
    int rAfter = rounds, s0 = -1, s1 = -1;
    bool called = true;
    if (o.callAddBullet) {
        s0 = SpareOf(ctrl, st->item);
        called = CallAddBullet(ctrl, st->item);
        // Fix round 1 (review M6): a FAULTED add added nothing - the machine must not count it, or the next update's unchanged round
        // count reads as a false Shot and un-chambers (pump_logic.h AddFailed, test FaultedAddIsNotCounted). COST: one call on a fault.
        if (!called) { st->pm.AddFailed(); }
        CallCtlInt(g_rounds, ctrl, &rAfter);
        s1 = SpareOf(ctrl, st->item);
        st->spare = s1;
    }
    for (int e = 1; e < 12; ++e) {
        if (o.Has(static_cast<PEvent>(e))) { ++g_pumpCounts[e]; }
    }
    // The lines - one per event raised (several may land in one update; ev is only the first).
    const char* pulse = !o.haptic ? "no pulse" : HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)";
    const long long skipped = static_cast<long long>(PumpSoundSkipped());
    if (o.Has(PEvent::HandCleared) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] HAND CLEARED - %s: %d shell(s) gone from the hand (nothing consumed - the spares move only on an add-bullet; "
            "spare %d).", in.freshDraw ? "the pump was drawn again (a switch / redraw)" : "the left grip let go", handBefore, in.spare);
    }
    if (o.Has(PEvent::Take) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] TAKE - %d shell(s) into the hand from the left hip (spare %d, rounds %d / %d, hand max %d); %s.", o.handShells,
            in.spare, rounds, p.capacity, p.handMax, pulse);
    }
    if (o.Has(PEvent::TakeNoShell) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] TAKE REFUSED - no shell to take (spare %d, rounds %d / %d): nothing in the hand, no pulse.", in.spare, rounds,
            p.capacity);
    }
    if (o.Has(PEvent::PortPush) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] PORT PUSH - the hand's shell %.3f m from the port (push %.3f m): load cycle %d plays (add-bullet at frame %d), "
            "%d shell(s) in the hand.", toPort, ReloadSnapM, o.animCycle, p.addFrame, o.handShells);
    }
    if (o.Has(PEvent::PortFull) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] PORT REFUSED - full (rounds %d / %d): the hand keeps its %d shell(s) until the grip opens.", rounds, p.capacity,
            o.handShells);
    }
    if (o.Has(PEvent::ShellIn) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] SHELL IN rounds %d -> %d, spare %d -> %d (the game's own add-bullet, cycle %d)%s | %d shell(s) left in the hand "
            "| finger passes applied %lld (%lld pins) | %s.", rounds, rAfter, s0, s1, o.animCycle,
            called ? "" : " - *** the add-bullet call FAULTED ***", o.handShells, static_cast<long long>(g_pumpFingerPasses),
            static_cast<long long>(g_pumpFingerPins), pulse);
    }
    if (o.Has(PEvent::Shot) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=no
        LOG("reload: [PUMP] SHOT - rounds now %d: %s (the game's rack sounds skipped so far %lld).", rounds,
            row.racks ? "not chambered, R2 withheld until a rack"
                      : rounds > 0 ? "no rack - the gun cycles itself, the next round is chambered" : "no rack - the tube is empty, R2 withheld",
            skipped);
    }
    if (o.Has(PEvent::RackBack) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] RACK BACK - the pump %.4f m back (done at %.4f m of its %.4f m); %s.", strokeM, p.rackDoneFrac * row.travelM,
            row.travelM, pulse);
    }
    if (o.Has(PEvent::RackForward) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] RACK FORWARD - rounds %d: nothing chambered (an empty tube), R2 stays withheld; %s.", rounds, pulse);
    }
    if (o.Has(PEvent::Chambered) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] CHAMBERED - the rack's forward end with %d round(s): R2 free; %s.", rounds, pulse);
    }
    if (o.Has(PEvent::RackShort) && PumpLine()) {
        // [PROBE: PUMP] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [PUMP] RACK SHORT - the grip let go at %.4f m deepest (%.0f%% of the travel; a rack needs %.4f m back AND forward): "
            "nothing chambered by it.", d.strokeMaxFrac * row.travelM, d.strokeMaxFrac * 100.0f, p.rackDoneFrac * row.travelM);
    }
    // Pulses (spec 4.8: take, each shell in, rack back, rack forward) and the game's own sounds: the seat at each add-bullet, the move at
    // the stroke's BACK end (ruling R12: the magOut slot), the rack at the FORWARD end. Every fx raised this update (POut fx[0..nFx)).
    if (o.haptic && HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
    for (int k = 0; k < o.nFx; ++k) {
        const reload::Fx f = o.fx[k];
        const reload::Fx slot = f == reload::Fx::RackGrab ? reload::Fx::MagGrab   // R12: SoundsFor(MagGrab) = the row's magOut slot
                              : (f == reload::Fx::RackDone || f == reload::Fx::Seat) ? f : reload::Fx::None;
        if (slot == reload::Fx::None) { continue; }
        int ns = 0;
        const reload::Snd* snd = reload::SoundsFor(g_gun.fx, slot, &ns);
        for (int i = 0; i < ns; ++i) {
            if (!GunSoundPlay(g_curPlayer, snd[i].name, snd[i].vol, snd[i].delayS, snd[i].startS)) { PumpSoundRefused(snd[i].name); }
        }
    }

    // DRAW (after the game's update, this tail). The pump every update: rest + the drawn stroke back along the row's axis.
    const char* drew = !row.racks ? "no pump (this gun does not rack)" : "nothing (no bone_root world)";
    bool ok = true;
    if (haveRoot && row.racks) {   // 2026-11-07: a gun with no pump (the semi-auto, d.pumpId -1) draws no pump
        float local[12], w[12];
        memcpy(local, row.pumpLocal, sizeof(local));
        const float back = gripped ? strokeM : 0.0f;
        local[3] += row.pumpAxis.x * back;
        local[7] += row.pumpAxis.y * back;
        local[11] += row.pumpAxis.z * back;
        Mul34(rootW, local, w);
        ok = SetWorld(model, d.pumpId, w);
        drew = !ok ? "*** the pump WRITE FAULTED ***" : gripped ? "pump with the hand" : "pump at rest";
    }
    // The shell at the hand (plan P2 (b), ruling R11): the frame's first shell IN THE HAND (shellOn) - under the thumb, or the next one
    // sliding up after an add-bullet - while shells are held or a cycle is before its add-bullet. The hold (no cycle) = cycle 1 frame 1.
    const int cyc = o.animCycle > 0 ? o.animCycle : 1;
    const int frm = o.animCycle > 0 ? o.animFrame : 1;
    const bool shellShows = o.handShells > 0 || (o.animCycle > 0 && o.animFrame < p.addFrame);
    bool shellDrawn = false;
    // PS2 smoothing: the shell between frame k and k + 1 by the clock's fraction (reload_hands.h Lerp34) - only while the SAME shell
    // shows in both (a frame where it is handed over keeps frame k's). COST: one rotation lerp + Gram-Schmidt.
    const float frmF = o.animCycle > 0 ? static_cast<float>(frm) + frameFrac : 1.0f;
    int fk = 1, fk2 = 1;
    float ft = 0.0f;
    reload::PumpFrameSplit(frmF, &fk, &fk2, &ft);
    if (shellShows && haveHand) {
        const reload::AnimFrame& af = reload::PumpAnimFrame(cyc, fk);
        const reload::AnimFrame& af2 = reload::PumpAnimFrame(cyc, fk2);
        int j = -1;
        for (int k = 0; k < 4 && j < 0; ++k) {
            if (af.shellOn[k]) { j = k; }
        }
        if (j >= 0) {
            float sm[12], w[12];
            if (af2.shellOn[j] && ft > 0.0f) { reload::Lerp34(af.shell[j], af2.shell[j], ft, sm); }
            else { memcpy(sm, af.shell[j], sizeof(sm)); }
            Mul34(lh, sm, w);
            if (PumpWriteShell(model, d.ammoId, w)) {
                PumpShellMark(model, now);   // fix round 1 (M1): this model's own record, with this draw's ids. COST: <= 4 compares
                shellDrawn = true;
            } else {
                ok = false;
            }
        }
    }
    if (!shellDrawn && PumpShellFor(model)) {
        // C14's planned empty hand (a cycle playing, shells still held, the hand read): every push passes through it - restored quietly.
        // Everything else (no hand, the hand empty, no cycle) still says so.
        const bool cycleGap = o.animCycle > 0 && o.handShells > 0 && haveHand;
        PumpRestoreShell(model, o.handShells > 0 ? "no shell in this frame / no hand" : "the hand is empty", cycleGap);
    }
    if (!ok && !d.writeFaultSaid && PumpRefuseLine()) {
        d.writeFaultSaid = true;
        // [PROBE: PUMP] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [PUMP] WRITE FAULTED - model %p: a SetElementWorldMatrix on the pump (%d) or the shell (%d) faulted (said once a draw).",
            reinterpret_cast<void*>(model), d.pumpId, d.ammoId);
    }

    // The finger pass: the anim frame while a cycle plays, the hold (cycle 1 frame 1) while shells are held, else nothing (ruling R9).
    // PS2 smoothing: the word carries the FLOAT frame and a quarter-ms stamp (reload_hands.h PumpAnimPack); the reader advances it to
    // its own time and blends the neighbouring frames. COST: one interlocked store.
    LONG64 word = 0;
    if (o.animCycle > 0) {
        word = reload::PumpAnimPack(nowQ, o.animCycle, frmF);
    } else if (o.handShells > 0) {
        word = reload::PumpAnimPack(nowQ, 0, 1.0f);   // the HOLD (cycle 1 frame 1, not advanced)
    }
    InterlockedExchange64(&g_pumpAnimWord, word);
    // The two-hand pin's slide (plan P5): the drawn stroke and the pump's BACK axis in holderl's frame (the pin's rel is holderl's frame -
    // twohand.cpp OffsetRel). COST while gripped: one holderl world read + 3 dots + one SRW acquire; else one SRW acquire.
    float axHl[3] = {};
    float hlW[12];
    const bool strokeOn = gripped && haveAxis && d.hlId >= 0 && ElemMtx(g_world, model, d.hlId, hlW);
    if (strokeOn) {
        for (int c = 0; c < 3; ++c) {
            const float l = Len3(hlW[c], hlW[4 + c], hlW[8 + c]);
            axHl[c] = l > 1e-6f ? (hlW[c] * axisW[0] + hlW[4 + c] * axisW[1] + hlW[8 + c] * axisW[2]) / l : 0.0f;
        }
    }
    AcquireSRWLockExclusive(&g_pumpLock);
    g_pumpStrokeOn = strokeOn;
    g_pumpStrokeM = strokeM;
    memcpy(g_pumpAxisHl, axHl, sizeof(g_pumpAxisHl));
    g_pumpStrokeMs = now;
    ReleaseSRWLockExclusive(&g_pumpLock);
    // PS1 fixes (R18): the port lock through the existing reload lock path (the mags' well-rail lock): the left hand bone LOCKED at the push
    // pose on bone_root as drawn this update (reload::LockedHand(rootW, row) - ReloadPinPos / ReloadPinRot, the two-hand pin skipped),
    // eased in / out over kHandBlendMs; no finger row (the C13 fingers are the pump anim's). A grip release / the hand emptied also drops
    // the lock (the release above). COST: HandsPublish's (a few compares; the seqlock write while on or easing out).
    d.lastCycle = o.animCycle;
    const bool lockOn = d.snapped && haveRoot && d.lockRow != nullptr;
    HandsPublish(lockOn, lockOn ? d.lockRow : nullptr, haveRoot ? rootW : nullptr, nullptr, true, now);
    // For the pad thread: R2 withheld while not chambered; no left press is the pump's (the hip press is the holster's, the rack press the
    // two-hand grip's - claiming it here would refuse the rack hold).
    Publish(o.withholdR2, false);
    return drew;
}

// The counts line, every kPumpPeriodMs behind diag_periodic_logs (the first sample is the unconditional ARMED line). COST: one compare.
void PumpPeriodic(const ItemState* st, const char* drew, ULONGLONG now)
{
    if (now - g_pumpLastPeriod < kPumpPeriodMs) { return; }
    g_pumpLastPeriod = now;
    // [PROBE: PUMP] gate=manual_reload,diag_periodic_logs  site=HEADSET  log-only=yes
    // PS1 fixes: the labels follow PEvent's order exactly (RackShort = 9, Chambered = 10 - RUN PS1 printed "chambered 0, short 31"
    // against 31 CHAMBERED lines, the two arguments swapped); + the port lock's snaps / releases.
    LOG_PERIODIC("reload: [PUMP] 5 s: drew %s, chambered %d, hand %d | take %ld (none %ld), push %ld (full %ld), shell in %ld, shot %ld, rack "
                 "back %ld, forward %ld, short %ld, chambered %ld, hand cleared %ld | port lock: snaps %ld, releases %ld | finger passes %lld "
                 "(%lld pins) | game rack sounds skipped %ld | START refused x%lld, ADD-BULLET refused x%lld.",
                 drew, st->pm.chambered() ? 1 : 0, st->pm.handShells(), g_pumpCounts[1], g_pumpCounts[2], g_pumpCounts[3],
                 g_pumpCounts[4], g_pumpCounts[5], g_pumpCounts[6], g_pumpCounts[7], g_pumpCounts[8],
                 g_pumpCounts[static_cast<int>(reload::PEvent::RackShort)], g_pumpCounts[static_cast<int>(reload::PEvent::Chambered)],
                 g_pumpCounts[11], g_pumpSnaps, g_pumpReleases, static_cast<long long>(g_pumpFingerPasses),
                 static_cast<long long>(g_pumpFingerPins), PumpSoundSkipped(), static_cast<long long>(g_startRefused),
                 static_cast<long long>(g_addRefused));
}

// ===== THE DOUBLE-BARREL wn_shotgun_a (2026-11-07; spec 2026-11-04-manual-reload-pump-shotgun-design.md section 9, every pick quoted
// there; the steps are dbarrel_logic.h - unit-tested - and the hand / shell data dbarrel_hands.h, GENERATED from Jay's approved files by
// tools/dbarrel_anim_export.py). Rides manual_reload; one new key, reload_flick_dps (the flick's threshold, Tier D).
//   - the hinge: bone_reload_shotgun's world = bone_root x [Rx(angle) | pivot] while the barrels are off shut (+ once back at rest);
//   - the two shells ammo_bullet (LEFT chamber) / ammo_bullet_b (RIGHT) drawn by us - seated, in the pull / load animation, or in the
//     hand - and back at their rest (inside the stock, unseen) otherwise (INV-700: the gun has no chamber shells of its own);
//   - the left hand LOCKED through the reload lock path (HandsPublish, an identity row: the published part IS the hand bone) while it
//     rides the barrels or plays the pull / load; the fingers by g_dbAnimWord (boneblend.cpp ReloadFingersApply -> DbFingersApply);
//   - ONE round per pushed shell: the game's own add-bullet with the controller's +429 cleared for the call and put back (INV-699);
//   - R2 withheld while the gun is not shut; the game's own reload START / add-bullets refused as for every manual gun (IsManualItem).
constexpr long      kMaxDbLines = 200;          // [DBARREL] event / draw lines a run (INV-9)
constexpr long      kMaxDbRefuseLines = 40;
constexpr ULONGLONG kDbPeriodMs = 5000;
constexpr ULONGLONG kDbAwayMs = 150;            // the pump's away guard: the same gun back within this resumes (a blip)
constexpr size_t    kCtlClipReloadOff = 429;    // fire controller +429 (+1ADh) - INV-699
struct DbDraw {
    int   rootId = -1, hingeId = -1, ammoId[2] = { -1, -1 }, hbId[2] = { -1, -1 }, hlId = -1;
    bool  fresh = false;
    bool  hingeOff = false;          // we wrote the hinge off its rest (put back at rest once when the barrels shut)
    bool  moved[2] = {};             // we moved shell s off its rest
    bool  rideLatched = false;       // the ride's latch: the grip point (mesh) + (left - right) in bone_root's axes + the angle
    float rideG0[3] = {}, rideLr0[3] = {}, rideA0 = 0.0f;
    bool  havePrev = false;          // the flick: bone_root's last world + its time
    float prevRoot[12] = {};
    long long prevQ = 0;
    float peakUpOpen = 0.0f;         // the fastest muzzle-up rate seen while open (logged at the shut - the threshold's tuning data)
    bool  haveLastLock = false;      // the last lock's hand in the mesh frame (the 100 ms blend-out follows the gun)
    float lastLock[12] = {};
    reload::DPhase lastPhase = reload::DPhase::Closed;
    bool  writeFaultSaid = false;
    bool  takeWas = false;           // (grip held AND in the hip zone) last update - the spare is read only on its rising edge
    bool  gripWas = false;           // the left grip last update (the fresh squeeze - RUN DB1 fix)
    bool  ownsWas = false;           // the last update published the left press as the reload's (what the pad thread decided on)
    bool  haveGrabPalm = false;      // the palm (mesh frame) at the last squeeze - the ride's lever
    float grabPalmM[3] = {};
    int   clipReload = -1;           // +429 read at the draw (-1 = unread)
    const reload::DbGunRow* row = nullptr;   // 2026-11-07 the mesh's row (reload_dbarrel_guns.inc): its tables, muzzle, shell rests
    int   set = reload::kDbSetA;     // row->set: kDbSetA (a / d / g) or kDbSetE (the Ox e, one shell)
};
DbDraw      g_db;                     // game thread
// A shell's REST local in its holder: the mesh's own row (d's sits 1.63 mm off a's, e's is turned - dbarrel_guns_export G6), a's if none;
// the single-shot's round (2026-11-09, DbAmmoRestOf).
const float* DbAmmoRest(const DbDraw& d, int s) { return reload::DbAmmoRestOf(d.set, d.row, s); }
// 2026-11-09 (Task 8a): the gun's MESH-frame world = bone_root's world x inv(its rest) - bone_root's rest IS the mesh frame on the
// double-barrels (the identity: unchanged), rotated on the single-shot. COST: one rigid inverse + one 3x4 product.
void DbMeshWorld(const DbDraw& d, const float rootW[12], float out[12])
{
    float ri[12];
    RigidInv(reload::DbRootRestOf(d.set), ri);
    Mul34(rootW, ri, out);
}
uintptr_t   g_dbAwayItem = 0;
ULONGLONG   g_dbAwayMs = 0;
uintptr_t   g_dbLastModel = 0;        // the model whose hinge / shells we may have moved (restored at its next draw / on the off switch)
volatile LONG64 g_dbAnimWord = 0;     // the finger pass's word (dbarrel_hands.h DbAnimPack); 0 = none
volatile LONG64 g_dbFingerPasses = 0, g_dbFingerPins = 0;
long        g_nDb = 0;
volatile LONG g_nDbRefuse = 0;
long        g_dbCounts[static_cast<int>(reload::DEvent::Count)] = {};
long        g_dbAdd429 = 0, g_dbAddExtra = 0;   // adds made with +429 cleared / adds that raised the count by more than one
ULONGLONG   g_dbLastPeriod = 0;
bool        g_dbArmedSaid = false;
reload::HandRow g_dbIdRow = {};       // the identity lock row (handR = I, handT = 0): the published part IS the l_hand bone
bool DbLine() { return ++g_nDb <= kMaxDbLines; }
bool DbRefuseLine() { return InterlockedIncrement(&g_nDbRefuse) <= kMaxDbRefuseLines; }

const char* DEventName(reload::DEvent e)
{
    switch (e) {
    case reload::DEvent::None: return "None";
    case reload::DEvent::OpenGrab: return "OpenGrab";
    case reload::DEvent::Opened: return "Opened";
    case reload::DEvent::OpenCancel: return "OpenCancel";
    case reload::DEvent::CloseGrab: return "CloseGrab";
    case reload::DEvent::Closed: return "Closed";
    case reload::DEvent::FlickClose: return "FlickClose";
    case reload::DEvent::PullStart: return "PullStart";
    case reload::DEvent::PullDone: return "PullDone";
    case reload::DEvent::SpentDropped: return "SpentDropped";
    case reload::DEvent::Take: return "Take";
    case reload::DEvent::TakeNoShell: return "TakeNoShell";
    case reload::DEvent::TakeShut: return "TakeShut";
    case reload::DEvent::LoadStart: return "LoadStart";
    case reload::DEvent::ShellIn: return "ShellIn";
    case reload::DEvent::LoadDone: return "LoadDone";
    case reload::DEvent::HandCleared: return "HandCleared";
    case reload::DEvent::Shot: return "Shot";
    case reload::DEvent::Count: return "Count";
    }
    return "?";
}
const char* DChName(reload::DChamber c)
{
    return c == reload::DChamber::Live ? "live" : c == reload::DChamber::Spent ? "spent" : "empty";
}

// A 3x4 with its basis columns made orthonormal (Gram-Schmidt: x, then y, z = x cross y - LockedHand's rule), t kept. False = degenerate.
bool Ortho34(const float* m, float* o)
{
    float x[3] = { m[0], m[4], m[8] }, y[3] = { m[1], m[5], m[9] };
    const float lx = Len3(x[0], x[1], x[2]);
    if (!(lx > 1e-6f)) { return false; }
    for (float& v : x) { v /= lx; }
    const float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; ++i) { y[i] -= d * x[i]; }
    const float ly = Len3(y[0], y[1], y[2]);
    if (!(ly > 1e-6f)) { return false; }
    for (float& v : y) { v /= ly; }
    const float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    for (int r = 0; r < 3; ++r) {
        o[4 * r] = x[r];
        o[4 * r + 1] = y[r];
        o[4 * r + 2] = z[r];
        o[4 * r + 3] = m[4 * r + 3];
    }
    return true;
}

// The hinge and both shells back at their rest on THIS model (the held one - never a write to a model that may be gone). COST: up to
// two world reads + three writes, only when something was moved.
void DbRestore(uintptr_t model, const char* why)
{
    DbDraw& d = g_db;
    float rootW[12], hb[12], w[12];
    const bool haveRoot = ElemMtx(g_world, model, d.rootId, rootW);
    bool ok = true;
    int did = 0;
    if (d.hingeOff && haveRoot) {
        float meshW[12];
        DbMeshWorld(d, rootW, meshW);
        reload::DbHingeWorld(d.set, meshW, 0.0f, w);   // a / e: rootW x DbHingeLocal(0) as before; the single-shot: its hinge's rest
        if (SetWorld(model, d.hingeId, w)) { d.hingeOff = false; ++did; } else { ok = false; }
    }
    for (int s = 0; s < 2; ++s) {
        if (!d.moved[s] || d.ammoId[s] < 0) { continue; }
        if (ElemMtx(g_world, model, d.hbId[s], hb)) {
            Mul34(hb, DbAmmoRest(d, s), w);
            if (PumpWriteShell(model, d.ammoId[s], w)) { d.moved[s] = false; ++did; } else { ok = false; }
        } else {
            ok = false;
        }
    }
    if (did && (ok ? DbLine() : DbRefuseLine())) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [DBARREL] RESTORED (%s) - model %p: %d part(s) back at rest%s.", why, reinterpret_cast<void*>(model), did,
            ok ? "" : " - *** a part did NOT go back (retried on the next chance) ***");
    }
}

// The double-barrel left the right hand: marked AWAY (cleared after kDbAwayMs - the pump's M2 rule; the same item back sooner resumes).
// The finger word drops at once (no hand pose on another gun). COST: one compare off a double-barrel.
void DbLeave(const char* why)
{
    if (g_gun.family != reload::Family::DBarrel || !g_gun.built || !g_gun.item) { return; }
    g_dbAwayItem = g_gun.item;
    const ULONGLONG now = GetTickCount64();
    g_dbAwayMs = now ? now : 1;
    InterlockedExchange64(&g_dbAnimWord, 0);
    if (DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [DBARREL] AWAY - %s: the reload state is kept %llu ms (a blip resumes), then cleared.", why,
            static_cast<unsigned long long>(kDbAwayMs));
    }
}
void DbAwayTick(ULONGLONG now)
{
    if (!g_dbAwayMs || now - g_dbAwayMs <= kDbAwayMs) { return; }
    for (ItemState& s : g_items) {
        if (s.item != g_dbAwayItem) { continue; }
        reload::DInput off{};   // manualOn false: DMachine reset - the hand empty, no animation, chambers re-derived at the next draw
        s.dm.Update(off, reload::DParams{});
    }
    g_dbAwayItem = 0;
    g_dbAwayMs = 0;
    // the controllers' grip points were published for the ride; a pump driven now keeps them (it set the flag itself)
    if (!g_pumpSilencedSet && !(g_gun.built && g_gun.family == reload::Family::DBarrel)) { InterlockedExchange(&g_pumpWantCtl, 0); }
}

// Classify a held double-barrel (ClassifyModel, once per model change): the elements by NAME and their parents, the fx row, +429.
// Any missing -> REFUSED by name and false: the game reloads it as today this draw. The ARMED line (the first draw of the session) is
// logged unconditionally, refused or not.
bool ClassifyDbDraw(uintptr_t model, uintptr_t item, uintptr_t ctrl, const char* meshNorm, const char* meshShown, const char* prefix,
                    int hingeId, int ammoA, int ammoB)
{
    const bool resume = g_dbAwayMs && item == g_dbAwayItem;
    const ULONGLONG awayFor = resume ? GetTickCount64() - g_dbAwayMs : 0;
    const DbDraw keep = g_db;
    g_db = DbDraw{};
    DbDraw& d = g_db;
    d.hingeId = hingeId;
    d.ammoId[0] = ammoA;
    d.ammoId[1] = ammoB;
    // 2026-11-09 (Task 8a): the row / set FIRST - the holders are looked up by the set's names (the single-shot has one, no _b)
    d.row = reload::DbGunFor(meshNorm);
    d.set = d.row ? d.row->set : reload::kDbSetA;
    const reload::DbNames& nm = reload::DbNamesOf(d.set);
    const bool two = nm.ammo[1] != nullptr;
    ElemId(model, "bone_root", &d.rootId);
    ElemId(model, nm.holder[0], &d.hbId[0]);
    if (two) { ElemId(model, nm.holder[1], &d.hbId[1]); } else { d.ammoId[1] = -1; }
    ElemId(model, "holderl", &d.hlId);
    int ph = -1, pa = -1, pb = -1, pha = -1, phb = -1;
    ElemParent(model, hingeId, &ph);
    ElemParent(model, ammoA, &pa);
    if (two) { ElemParent(model, ammoB, &pb); }
    ElemParent(model, d.hbId[0], &pha);
    if (two) { ElemParent(model, d.hbId[1], &phb); }
    unsigned char clip = 0;
    d.clipReload = ReadU8(ctrl + kCtlClipReloadOff, &clip) ? clip : -1;
    if (!g_dbIdRow.key) {
        g_dbIdRow.key = "dbarrel identity";
        const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        memcpy(g_dbIdRow.handR, I, sizeof(I));
    }
    const reload::FxRow* fx = meshNorm ? reload::FindFxRow(meshNorm) : nullptr;
    // the single-shot's holder_bullet is a child of `holder` (the mesh root), not bone_root - its draw uses the holder's own world, so
    // only the double-barrels' holders are required under bone_root (their old rule)
    const char* why = !d.row ? "no break-open row for this mesh (reload_dbarrel_guns.inc / kDbPeRow)"
                    : d.rootId < 0 ? "the mesh has no bone_root"
                    : d.hbId[0] < 0 ? "the mesh has no holder for its first carried shell / round"
                    : two && d.hbId[1] < 0 ? "the mesh has no holder_bullet_b"
                    : ph != d.rootId ? "the hinge bone is not a child of bone_root (the hinge is drawn in bone_root's / the mesh frame)"
                    : pa != d.hbId[0] ? "the first carried shell / round is not a child of its holder"
                    : two && pb != d.hbId[1] ? "ammo_bullet_b is not a child of holder_bullet_b"
                    : two && (pha != d.rootId || phb != d.rootId) ? "holder_bullet / holder_bullet_b is not a child of bone_root"
                    : !fx ? "no fx row for this mesh (reload_fx_table.inc)"
                    : (fx->nMagOut < 1 || fx->nSeat < 1 || fx->nRack < 1) ? "the fx row lacks the open / load / close sound"
                    : nullptr;
    if (!g_dbArmedSaid) {
        g_dbArmedSaid = true;
        const LONG vg = g_vrGripSeen;
        const char* hipOff = HolsterLeftTakeRefusal();
        // [PROBE: DBARREL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [DBARREL] ARMED (first double-barrel draw, logged unconditionally) - manual_reload=%d | mesh '%s', prefix '%s' | "
            "elements by name: bone_root %d, bone_reload_shotgun %d (parent %d), ammo_bullet %d (parent %d), ammo_bullet_b %d (parent "
            "%d), holder_bullet %d / _b %d (parents %d / %d), holderl %d | controller +429 = %d (INV-699: set = the add-bullet fills the "
            "clip; it is cleared for each of OUR calls) | THIS SET opens %.0f deg about its hinge, pivot (%+.4f %+.4f %+.4f) | grabs "
            "within %.3f m, re-arm beyond %.3f m (reload_snap_m) | flick %.0f deg/s for 2 updates (reload_flick_dps), shut in 0.15 s | "
            "tables: pull %d frames, load %d (seats %d / %d, 1-shell from %d) @ %d fps; the Ox e's: pull %d, one push %d (seat %d) | "
            "this mesh: row %s, set %d (%s), muzzle z %.4f | fx row %s | vr_grip %s | left zones %s%s | this draw: %s",
            ManualReload ? 1 : 0, meshShown, prefix, d.rootId, hingeId, ph, ammoA, pa, ammoB, pb, d.hbId[0], d.hbId[1], pha, phb, d.hlId,
            d.clipReload, reload::DbOpenDegOf(d.set), reload::DbGeomOf(d.set).pivot[0], reload::DbGeomOf(d.set).pivot[1],
            reload::DbGeomOf(d.set).pivot[2], ReloadSnapM,
            1.5f * ReloadSnapM, ReloadFlickDps, reload::kDbPullFrames, reload::kDbLoadFrames, reload::kDbLoadSeatFrame[0],
            reload::kDbLoadSeatFrame[1], reload::kDbLoadOneStart, reload::kDbFps, reload::kDbEPullFrames, reload::kDbELoadFrames,
            reload::kDbELoadSeatFrame, d.row ? d.row->mesh : "NONE", d.set,
            d.set == reload::kDbSetPe ? "the single-shot's one round" : d.set == reload::kDbSetE ? "one shell" : "two shells",
            d.row ? d.row->topZ1 : 0.0f, fx ? fx->mesh : "NONE",
            vg < 0 ? "not polled yet" : vg ? "1" : "*** 0 - the left grip is not read: nothing opens, takes or loads ***",
            hipOff ? "*** REFUSED: " : "not refused", hipOff ? hipOff : "", why ? why : "manual");
    }
    if (why) {
        if (DbRefuseLine()) {
            // [PROBE: DBARREL] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [DBARREL] REFUSED model %p item %p (mesh '%s'): %s - the double-barrel reloads as today this draw.",
                reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown, why);
        }
        return false;
    }
    d.fresh = !resume;
    if (resume && model == g_dbLastModel) {   // a blip on the SAME model: what we drew there is still ours to keep drawing
        d.hingeOff = keep.hingeOff;
        d.moved[0] = keep.moved[0];
        d.moved[1] = keep.moved[1];
        d.peakUpOpen = keep.peakUpOpen;
        d.lastPhase = keep.lastPhase;
    } else if (model == g_dbLastModel) {      // the same model drawn again: whatever we left on it goes home
        d.hingeOff = keep.hingeOff;
        d.moved[0] = keep.moved[0];
        d.moved[1] = keep.moved[1];
        DbRestore(model, "the double-barrel was drawn again");
    }
    g_dbAwayItem = 0;
    g_dbAwayMs = 0;
    g_dbLastModel = model;
    if (DbLine()) {
        char res[96] = "";
        if (resume) { wsprintfA(res, " (RESUMED after %u ms away - the open / hand / chambers kept)", static_cast<unsigned>(awayFor)); }
        // [PROBE: DBARREL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [DBARREL] DRAW model %p item %p (mesh '%s'): manual%s - hinge %d, shells %d / %d under %d / %d, holderl %d | "
            "controller +429 = %d | set %d (%s), muzzle z %.4f.", reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown,
            res, hingeId, ammoA, ammoB, d.hbId[0], d.hbId[1], d.hlId, d.clipReload, d.set,
            d.set == reload::kDbSetE ? "the Ox's one shell" : "two shells", d.row->topZ1);
    }
    return true;
}

// One update of the held double-barrel (ReloadTick, game thread). Returns what was drawn.
// [LOADBEARING: manual reload double-barrel] (manual_reload) every double-barrel step: the hinge drawn where the hand / the flick puts it,
//      the two shells drawn seated / in the hand / in Jay's pull-out and load, the left hand locked while it rides the barrels or plays
//      them, ONE round per pushed shell (+429 cleared for the call, INV-699), R2 withheld while the gun is not shut.
// COST per update with the double-barrel held: one rounds call, one bone_root world read, one l_hand world read, the step machine, one
//      controller-pair read (shared SRW), a 3x3 rate (the flick), a few compares; + one hinge write while the barrels are off shut; +
//      one world read + one write per drawn shell (<= 2); + HandsPublish (a seqlock write while locked / easing) and one interlocked
//      store (the finger word). Events only: the add-bullet (+ 2 byte writes, 4 game calls for the line), a pulse, a sound.
// twohand.cpp kPalmFromHandholder (:315) - the palm point the two-hand grab zone tests; the SAME values, so both sides judge one point.
constexpr float kDbPalmFromHandholder[3] = { -0.0460f, 0.0040f, -0.0027f };
constexpr long  kMaxDbSqueezeLines = 150;   // [DBARREL] SQUEEZE lines a run (RUN DB1 had 75 left squeezes in 2 minutes)
long        g_nDbSqueeze = 0;

const char* DriveDBarrel(uintptr_t ctrl, ItemState* st, void* playerModel, int lHandId, const float* leftPos, const float* leftR9,
                         bool leftAchieved, const float* leftCmdPos, ULONGLONG now)
{
    using reload::DEvent;
    using reload::DPhase;
    DbDraw& d = g_db;
    const uintptr_t model = g_gun.model;
    InterlockedExchange(&g_pumpWantCtl, 1);   // the left solve publishes each controller's grip point (the ride's source, the rack's rule)
    const long long nowQ = PumpClockQ();
    float rootRaw[12], rootW[12], rootO[12], lh[12];
    // 2026-11-09 (Task 8a): every table / zone is in the gun's MESH frame = bone_root's world x inv(its rest) (DbMeshWorld) - the
    // identity rest on the double-barrels (bit for bit the old rootW), rotated on the single-shot. rootW below IS that mesh-frame world.
    const bool haveRoot = ElemMtx(g_world, model, d.rootId, rootRaw) && (DbMeshWorld(d, rootRaw, rootW), true) && Ortho34(rootW, rootO);
    const bool haveHand = playerModel && lHandId >= 0 && ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh);
    // the gun's hinge geometry (its pivot, axis, bore, breech, top band - the band ending at THIS gun's muzzle) and open angle
    reload::DbGeom geom = reload::DbGeomOf(d.set);
    if (d.row) { geom.topZ1 = d.row->topZ1; }
    const float openDeg = reload::DbOpenDegOf(d.set);
    const reload::DbFrame& openRow = reload::DbFrameAt(reload::DbOpenAnim(d.set), 1);
    const DPhase phase0 = st->dm.phase();
    const float angle0 = st->dm.angle();
    const int hand0 = st->dm.hand();   // the SQUEEZE line

    reload::DInput in{};
    in.manualOn = ManualReload;
    in.freshDraw = d.fresh;
    d.fresh = false;
    in.gripDown = g_gripDown != 0;
    in.inHip = HolsterLeftPouchHeld();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    // The spare: only on the rising edge of (grip held AND in the hip zone) - the update the machine's take may fire on (the pump's R1).
    const bool takeNow = in.gripDown && in.inHip;
    if (takeNow && !d.takeWas) { st->spare = SpareOf(ctrl, st->item); }
    d.takeWas = takeNow;
    in.spare = st->spare;
    in.nowS = static_cast<double>(nowQ) / 4000.0;
    in.forendHeld = TwoHandGripping();

    // WHERE THE HAND IS (RUN DB1 fix; Jay: OPEN vs FOREND "A" = by where the hand is). The DRAWN PALM - the two-hand zone's own point:
    // the achieved handholder + the drawn basis x kPalmFromHandholder, else the commanded grip (twohand ZonePoint's fallback) - in the
    // BARRELS' frame (bone_root's, turned back by the angle). Over the barrels = the top grip (a fresh squeeze); at the breech = the
    // pull (a fresh squeeze) / the load (the grip held from the hip). RUN DB1's wrist-within-5-cm-of-the-authored-wrist test missed every
    // chamber squeeze (0 PULL START in 20) and let the forend hold take 24 squeezes meant for the top. COST: a few mul-adds + a sqrt.
    const bool gripEdge = in.gripDown && !d.gripWas;
    d.gripWas = in.gripDown;
    in.gripEdge = gripEdge;
    float palmW[3] = {}, palmM[3] = {}, palmB[3] = {};
    bool havePalm = false;
    if (leftAchieved && leftPos && leftR9) {
        for (int r = 0; r < 3; ++r) {
            palmW[r] = leftPos[r] + leftR9[3 * r] * kDbPalmFromHandholder[0] + leftR9[3 * r + 1] * kDbPalmFromHandholder[1] +
                       leftR9[3 * r + 2] * kDbPalmFromHandholder[2];
        }
        havePalm = true;
    } else if (leftCmdPos) {
        memcpy(palmW, leftCmdPos, sizeof(palmW));
        havePalm = true;
    }
    reload::DbZone zone;
    if (havePalm && haveRoot) {
        float inv[12];
        RigidInv(rootO, inv);
        const reload::Vec3 v{ palmW[0], palmW[1], palmW[2] };
        XformPt(inv, v, palmM);
        reload::DbToBarrelsG(geom, angle0, palmM, palmB);
        zone = reload::DbZoneOfG(geom, palmB);   // the band ends at THIS gun's muzzle (geom.topZ1 = its row's)
        in.nearTop = zone.top;
        in.nearPull = zone.breech;
        in.nearLoad = zone.breech;
        in.farLoad = zone.breechM > 1.5f * geom.breechR;
    } else {
        havePalm = false;
    }
    if (gripEdge) {   // the ride's lever: where the hand really grabbed (none seen -> the authored top grip)
        d.haveGrabPalm = havePalm;
        if (havePalm) { memcpy(d.grabPalmM, palmM, sizeof(d.grabPalmM)); }
    }
    float top[12], tmp[12];   // the authored top grip at this angle (the ride's lever if no palm was seen at the grab)
    reload::DbMeshHingeG(geom, angle0, tmp);
    reload::DbMul34(tmp, openRow.hand, top);
    // THE RIDE (OPENING "A" / CLOSING "B"): the barrels follow the hand. Read from the CONTROLLERS (the drawn hand is locked to the
    // barrels and would measure itself): the grip point latched at the grab + how far (left - right) has moved since, both in bone_root's
    // axes (a turn of the gun with both hands, a smooth turn or walking cancel - the pump rack's rule). A stale pair holds the angle.
    float lr[3] = {}, lrL[3] = {};
    const bool haveLR = PumpControllersLR(lr) && haveRoot && DirToLocal(rootW, lr, lrL);
    if (phase0 == DPhase::Riding && haveLR) {
        if (!d.rideLatched) {
            d.rideLatched = true;
            if (d.haveGrabPalm) { memcpy(d.rideG0, d.grabPalmM, sizeof(d.rideG0)); }
            else { d.rideG0[0] = top[3]; d.rideG0[1] = top[7]; d.rideG0[2] = top[11]; }
            memcpy(d.rideLr0, lrL, sizeof(d.rideLr0));
            d.rideA0 = angle0;
        } else {
            const float p[3] = { d.rideG0[0] + lrL[0] - d.rideLr0[0], d.rideG0[1] + lrL[1] - d.rideLr0[1], d.rideG0[2] + lrL[2] - d.rideLr0[2] };
            in.rideAngle = reload::DbRideAngleG(geom, d.rideG0, p, d.rideA0, openDeg);
            in.rideValid = true;
        }
    }
    if (phase0 != DPhase::Riding) { d.rideLatched = false; }
    // THE FLICK (FLICK "A"): the muzzle's upward turn rate from bone_root's direction between updates (no controller angular speed exists
    // in src). COST: a 3x3 cross + 3 sqrt + an asin.
    float upDps = 0.0f;
    if (haveRoot) {
        if (d.havePrev && nowQ > d.prevQ) { upDps = reload::DbMuzzleUpDps(d.prevRoot, rootW, static_cast<float>(nowQ - d.prevQ) / 4000.0f); }
        memcpy(d.prevRoot, rootW, sizeof(d.prevRoot));
        d.prevQ = nowQ;
        d.havePrev = true;
    } else {
        d.havePrev = false;
    }
    in.flickUpDps = upDps;
    if (phase0 == DPhase::Open && upDps > d.peakUpOpen) { d.peakUpOpen = upDps; }

    reload::DParams p;
    p.flickDps = ReloadFlickDps;
    p.set = d.set;
    p.openDeg = openDeg;   // a / e 45, the single-shot 68 (Jay)
    const reload::DOut o = st->dm.Update(in, p);

    // THE ADD-BULLET (each pushed shell): ONE round - the controller's +429 cleared for the call and put back (INV-699: every reader of
    // it is game-thread code, so the synchronous clear-call-restore cannot be seen by another). Synchronous, the game's own trampoline.
    int rAfter = rounds, s0 = -1, s1 = -1, was429 = -1;
    bool called = true;
    if (o.callAddBullet) {
        s0 = SpareOf(ctrl, st->item);
        unsigned char b = 0;
        const bool haveB = ReadU8(ctrl + kCtlClipReloadOff, &b);
        was429 = haveB ? b : -1;
        const bool cleared = haveB && b != 0 && WriteU8(ctrl + kCtlClipReloadOff, 0);
        called = CallAddBullet(ctrl, st->item);
        if (cleared) { WriteU8(ctrl + kCtlClipReloadOff, b); ++g_dbAdd429; }
        if (!called) { st->dm.AddFailed(o.addChamber); }
        CallCtlInt(g_rounds, ctrl, &rAfter);
        if (rAfter > rounds + 1) { ++g_dbAddExtra; }
        s1 = SpareOf(ctrl, st->item);
        st->spare = s1;
    }
    for (int e = 1; e < static_cast<int>(DEvent::Count); ++e) {
        if (o.Has(static_cast<DEvent>(e))) { ++g_dbCounts[e]; }
    }
    const char* pulse = !o.haptic ? "no pulse" : HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)";
    if (o.Has(DEvent::OpenGrab) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] OPEN GRAB - the palm %.3f m above the barrels' bore line, %.3f m along them (holderl's two-hand hold not "
            "held): the left hand LOCKED to the barrels, they follow it; %s.", zone.aboveM, palmB[2], pulse);
    }
    if (o.Has(DEvent::CloseGrab) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] CLOSE GRAB - the palm %.3f m above the open barrels' bore line: lift to shut; %s.", zone.aboveM, pulse);
    }
    if (o.Has(DEvent::Opened) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] OPENED - the barrels at %.1f deg (the stop): chambers L %s / R %s, rounds %d; R2 withheld; rmngtn_open; %s.",
            o.angle, DChName(o.ch[0]), DChName(o.ch[1]), rounds, pulse);
    }
    if (o.Has(DEvent::OpenCancel) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] OPEN CANCELLED - the grip let go at %.1f deg (before the stop): the barrels back shut, no sound.", angle0);
    }
    if (o.Has(DEvent::FlickClose) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] FLICK - the muzzle snapped up %.0f deg/s (reload_flick_dps %.0f, 2 updates): the barrels swing shut on their own "
            "(%.2f s).", upDps, ReloadFlickDps, p.flickCloseS);
    }
    if (o.Has(DEvent::Closed) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] CLOSED (%s) - chambers L %s / R %s, rounds %d; R2 free; rmngtn_close; %s | the fastest muzzle-up turn while open "
            "%.0f deg/s (flick threshold %.0f).", phase0 == DPhase::FlickClosing ? "the flick" : "lifted by the hand", DChName(o.ch[0]),
            DChName(o.ch[1]), rounds, pulse, d.peakUpOpen, ReloadFlickDps);
        d.peakUpOpen = 0.0f;
    }
    if (o.Has(DEvent::PullStart) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] PULL START - the palm %.3f m from the chambers' mouth: the spent shell(s)%s%s come out, locked to the bore (%.2f s "
            "+ the %.2f s lock-in); %s.", zone.breechM, (o.animMask & 1) ? " LEFT" : "", (o.animMask & 2) ? " RIGHT" : "",
            reload::DbTimeOfFrame(reload::DbPullFramesOf(d.set)), p.leadS, pulse);
    }
    if (o.Has(DEvent::PullDone) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] PULL DONE - %d spent shell(s) in the hand (free; let go to drop); chambers L %s / R %s.",
            reload::DbBits(o.handMask), DChName(o.ch[0]), DChName(o.ch[1]));
    }
    if (o.Has(DEvent::SpentDropped) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] SPENT DROPPED - the grip let go: the spent shell(s) gone.");
    }
    if (o.Has(DEvent::Take) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] TAKE - %d shell(s) from the left hip (spare %d, rounds %d, chambers L %s / R %s); %s.", reload::DbBits(o.handMask),
            in.spare, rounds, DChName(o.ch[0]), DChName(o.ch[1]), pulse);
    }
    if (o.Has(DEvent::TakeNoShell) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] TAKE REFUSED - %s (spare %d, chambers L %s / R %s): nothing in the hand, no pulse.",
            in.spare <= 0 ? "no shell to take" : "no empty chamber (pull the spent shells first)", in.spare, DChName(o.ch[0]),
            DChName(o.ch[1]));
    }
    if (o.Has(DEvent::TakeShut) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] TAKE REFUSED - the gun is shut (break it open first): nothing in the hand.");
    }
    if (o.Has(DEvent::LoadStart) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] PUSH START - the palm %.3f m from the chambers' mouth: the thumb pushes the %s shell in (locked; one shell "
            "per push), %d in the hand; %s.", zone.breechM, (o.animMask & 1) ? "LEFT" : "RIGHT", reload::DbBits(o.animMask), pulse);
    }
    if (o.Has(DEvent::ShellIn) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] SHELL IN %s chamber - rounds %d -> %d, spare %d -> %d (the game's own add-bullet; +429 was %d%s)%s; "
            "rmngtn_bullet_load; %s | finger passes %lld.", o.addChamber == 0 ? "LEFT" : "RIGHT", rounds, rAfter, s0, s1, was429,
            was429 > 0 ? ", cleared for the call" : "", !called ? " - *** the add-bullet call FAULTED ***"
                                                       : rAfter > rounds + 1 ? " - *** MORE THAN ONE ROUND ADDED ***" : "",
            pulse, static_cast<long long>(g_dbFingerPasses));
    }
    if (o.Has(DEvent::LoadDone) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] PUSH DONE - chambers L %s / R %s, rounds %d, %d shell(s) left in the hand: %s.", DChName(o.ch[0]),
            DChName(o.ch[1]), rAfter, reload::DbBits(o.handMask),
            o.handMask ? "pull back out of the breech, then in again to push the next" : "shut it (lift or flick)");
    }
    if (o.Has(DEvent::HandCleared) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] HAND CLEARED - %s: the shell(s) in the hand gone (nothing consumed - the spares move only on an add-bullet).",
            in.freshDraw ? "the gun was drawn again" : "the grip let go");
    }
    if (o.Has(DEvent::Shot) && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [DBARREL] SHOT - rounds now %d: chambers L %s / R %s (the right barrel fires first).", rounds, DChName(o.ch[0]),
            DChName(o.ch[1]));
    }
    // EVERY left squeeze while the double-barrel is held (RUN DB1 fix - a miss logged nothing): where the palm was in the barrels' frame,
    // which zone it was in, who owned the press, and what it did. Capped kMaxDbSqueezeLines (INV-9).
    // BETA lean: compiled out 2026-11-07 (Jay, tester package: "[DBARREL] squeeze/phase"). DEV keeps it.
#if !defined(DLVR_BETA_LEAN)
    if (gripEdge && ++g_nDbSqueeze <= kMaxDbSqueezeLines) {
        const char* did = o.Has(DEvent::OpenGrab) ? "OPEN GRAB" : o.Has(DEvent::CloseGrab) ? "CLOSE GRAB" : o.Has(DEvent::PullStart) ? "PULL"
                        : o.Has(DEvent::Take) ? "TAKE" : o.Has(DEvent::TakeNoShell) || o.Has(DEvent::TakeShut) ? "TAKE REFUSED"
                        : o.Has(DEvent::LoadStart) ? "LOAD" : in.forendHeld ? "nothing - the forend hold has the hand"
                        : in.inHip ? "nothing (at the hip)" : "nothing - outside both zones";
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [DBARREL] SQUEEZE #%ld - phase %d at %.1f deg, hand %d | palm %s (barrels' frame x %+.3f y %+.3f z %+.3f): %.3f m above the "
            "bore line (top band 0..%.2f, z %.2f..%.2f, |x| <= %.2f -> %s), %.3f m from the chambers' mouth (ball %.2f -> %s) | the press was "
            "%s (the forend hold %s) -> %s.", g_nDbSqueeze, static_cast<int>(phase0), angle0, hand0,
            !havePalm ? "*** NOT SEEN ***" : leftAchieved ? "drawn" : "commanded", palmB[0], palmB[1], palmB[2], zone.aboveM,
            geom.topUpM, geom.topZ0, geom.topZ1, geom.topHalfX, zone.top ? "TOP" : "no", zone.breechM,
            geom.breechR, zone.breech ? "BREECH" : "no", d.ownsWas ? "CLAIMED by the reload" : "not claimed",
            in.forendHeld ? "HOLDS" : "free", did);
    }
#endif
    // Pulses (left: the hand's steps; the flick's shut: the RIGHT - the gun hand did it) and the game's own sounds.
    if (o.haptic && HolsterHaptic > 0.0f) {
        const bool flickShut = o.Has(DEvent::Closed) && phase0 == DPhase::FlickClosing;
        PulseController(flickShut ? kRightHand : kLeftHand, HolsterHaptic, kPulseMs);
    }
    for (int k = 0; k < o.nFx; ++k) {
        const reload::Fx slot = o.fx[k] == reload::DFx::Open ? reload::Fx::MagGrab   // the fx row's magOut slot = rmngtn_open
                              : o.fx[k] == reload::DFx::Seat ? reload::Fx::Seat
                              : o.fx[k] == reload::DFx::Close ? reload::Fx::RackDone   // the rack slot = rmngtn_close
                              : reload::Fx::None;
        if (slot == reload::Fx::None) { continue; }
        int ns = 0;
        const reload::Snd* snd = reload::SoundsFor(g_gun.fx, slot, &ns);
        for (int i = 0; i < ns; ++i) {
            if (!GunSoundPlay(g_curPlayer, snd[i].name, snd[i].vol, snd[i].delayS, snd[i].startS)) { PumpSoundRefused(snd[i].name); }
        }
    }

    // DRAW (after the game's update, this tail).
    bool ok = true;
    const char* drew = "shut (the game's own pose)";
    // the hinge
    float hingeW[12];
    bool haveHinge = false;
    if (haveRoot) {
        reload::DbHingeWorld(d.set, rootW, o.angle, hingeW);   // a / e: rootW x DbHingeLocal(a) as before
        haveHinge = true;
        if (o.angle > 0.0f || d.hingeOff) {
            if (SetWorld(model, d.hingeId, hingeW)) { d.hingeOff = o.angle > 0.0f; } else { ok = false; }
            drew = o.angle > 0.0f ? "barrels off shut" : "barrels back at rest";
        }
    }
    // the shells
    const reload::DbAnim an = o.anim;
    int fk = 1, fk2 = 1;
    float ft = 0.0f;
    reload::DbFrameSplit(an, o.animFrame, &fk, &fk2, &ft);
    for (int s = 0; s < 2; ++s) {
        if (d.ammoId[s] < 0) { continue; }   // 2026-11-09: the single-shot has one carried round (no second element)
        const int b = 1 << s;
        float w[12], m[12];
        bool draw = false;
        if ((o.animMask & b) && haveRoot) {
            const reload::DbFrame& f1 = reload::DbFrameAt(an, fk);
            const reload::DbFrame& f2 = reload::DbFrameAt(an, fk2);
            if (ft > 0.0f) { reload::Lerp34(f1.shellM[s], f2.shellM[s], ft, m); } else { memcpy(m, f1.shellM[s], sizeof(m)); }
            if (reload::DbIsPull(an) && o.pullLeadT < 1.0f) {   // the pull starts out of the seat (a: H10): the shells ease out of it
                float hl45[12], seat[12];
                const float I12[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
                reload::DbHingeWorld(d.set, I12, openDeg, hl45);   // the hinge at open, mesh frame (a / e: DbHingeLocal(45))
                reload::DbMul34(hl45, reload::DbSeatOf(d.set, s), seat);
                reload::Lerp34(seat, reload::DbFrameAt(an, 1).shellM[s], o.pullLeadT, m);
            }
            Mul34(rootW, m, w);
            draw = true;
        } else if ((o.handMask & b) && haveHand) {
            const reload::DbFrame& hf = o.handSpent ? reload::DbFrameAt(reload::DbPullAnim(d.set), reload::DbPullFramesOf(d.set))
                                                    : reload::DbFrameAt(reload::DbLoadAnim(d.set), o.handMask == 2 ? reload::DbOneStartOf(d.set) : 1);
            Mul34(lh, hf.shellH[s], w);
            draw = true;
        } else if ((o.seatMask & b) && haveHinge) {
            Mul34(hingeW, reload::DbSeatOf(d.set, s), w);
            draw = true;
        }
        if (draw) {
            if (PumpWriteShell(model, d.ammoId[s], w)) { d.moved[s] = true; } else { ok = false; }
        } else if (d.moved[s]) {
            float hb[12];
            if (ElemMtx(g_world, model, d.hbId[s], hb)) {
                Mul34(hb, DbAmmoRest(d, s), w);
                if (PumpWriteShell(model, d.ammoId[s], w)) { d.moved[s] = false; } else { ok = false; }
            }
        }
    }
    if (!ok && !d.writeFaultSaid && DbRefuseLine()) {
        d.writeFaultSaid = true;
        // [PROBE: DBARREL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [DBARREL] WRITE FAULTED - model %p: a SetElementWorldMatrix on the hinge (%d) or a shell (%d / %d) faulted (said once a "
            "draw).", reinterpret_cast<void*>(model), d.hingeId, d.ammoId[0], d.ammoId[1]);
    }
    // the hand: the lock (the part IS the hand bone - an identity row) + the finger word
    float handM[12];
    bool lockOn = false;
    if (o.locked && haveRoot) {
        if (o.anim == reload::DbAnim::Open || o.anim == reload::DbAnim::OpenPe) {
            float mh[12];
            reload::DbMeshHingeG(geom, o.angle, mh);
            reload::DbMul34(mh, openRow.hand, handM);
        } else {
            const reload::DbFrame& f1 = reload::DbFrameAt(an, fk);
            const reload::DbFrame& f2 = reload::DbFrameAt(an, fk2);
            if (ft > 0.0f) { reload::Lerp34(f1.hand, f2.hand, ft, handM); } else { memcpy(handM, f1.hand, sizeof(handM)); }
        }
        memcpy(d.lastLock, handM, sizeof(d.lastLock));
        d.haveLastLock = true;
        lockOn = true;
    }
    float part[12];
    const bool havePart = haveRoot && d.haveLastLock && (Mul34(rootO, d.lastLock, part), true);
    HandsPublish(lockOn, lockOn ? &g_dbIdRow : nullptr, havePart ? part : nullptr, nullptr, true, now);
    LONG64 word = 0;
    if (o.anim != reload::DbAnim::None) {
        word = reload::DbAnimPack(nowQ, o.anim, o.animFrame,
                                  o.animHeld || o.anim == reload::DbAnim::Open || o.anim == reload::DbAnim::OpenPe);
    }
    InterlockedExchange64(&g_dbAnimWord, word);
#if !defined(DLVR_BETA_LEAN)   // BETA lean: compiled out 2026-11-07 (Jay, tester package); d.lastPhase below stays
    if (o.phase != d.lastPhase && DbLine()) {
        // [PROBE: DBARREL] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [DBARREL] PHASE %d -> %d at %.1f deg (0 shut, 1 riding, 2 open, 3 flick shutting, 4 pulling, 5 loading) | drew %s | "
            "hand %d, anim %d frame %.2f, locked %d.", static_cast<int>(d.lastPhase), static_cast<int>(o.phase), o.angle, drew, o.handMask,
            static_cast<int>(o.anim), o.animFrame, lockOn ? 1 : 0);
    }
#endif
    d.lastPhase = o.phase;
    // [LOADBEARING: manual reload two-hand arbitration] (manual_reload) RUN DB1 fix: a left press that starts with the palm OVER the
    //      barrels, or while the gun is not shut (open / riding / pulling / loading / shutting - every left press is the reload's then),
    //      is the reload's: ReloadPadPoll claims it at its edge, the two-hand grip refuses it ("reload owns 1") and its L1 is withheld. Under
    //      the barrels on the shut gun it stays the forend hold's (Jay: OPEN vs FOREND "A"). The pad thread reads this publish (<= 250 ms).
    const bool owns = o.phase != DPhase::Closed || (havePalm && zone.top);
    d.ownsWas = owns;
    Publish(o.withholdR2, owns);
    return drew;
}

// The counts line, every kDbPeriodMs behind diag_periodic_logs. COST: one compare.
void DbPeriodic(const ItemState* st, const char* drew, ULONGLONG now)
{
    if (now - g_dbLastPeriod < kDbPeriodMs) { return; }
    g_dbLastPeriod = now;
    using reload::DEvent;
    auto c = [](DEvent e) { return g_dbCounts[static_cast<int>(e)]; };
    // [PROBE: DBARREL] gate=manual_reload,diag_periodic_logs  site=HEADSET  log-only=yes
    LOG_PERIODIC("reload: [DBARREL] 5 s: drew %s, phase %d at %.1f deg, chambers L %s / R %s, hand %d | opened %ld (cancel %ld), shut %ld "
                 "(flicks %ld), pulls %ld, dropped %ld, takes %ld (refused %ld + shut %ld), loads %ld, shells in %ld, shots %ld | adds with "
                 "+429 cleared %ld, extra-round adds %ld | finger passes %lld (%lld pins) | START refused x%lld, ADD-BULLET refused x%lld.",
                 drew, static_cast<int>(st->dm.phase()), st->dm.angle(), DChName(st->dm.chamber(0)), DChName(st->dm.chamber(1)),
                 st->dm.hand(), c(DEvent::Opened), c(DEvent::OpenCancel), c(DEvent::Closed), c(DEvent::FlickClose), c(DEvent::PullStart),
                 c(DEvent::SpentDropped), c(DEvent::Take), c(DEvent::TakeNoShell), c(DEvent::TakeShut), c(DEvent::LoadStart),
                 c(DEvent::ShellIn), c(DEvent::Shot), g_dbAdd429, g_dbAddExtra, static_cast<long long>(g_dbFingerPasses),
                 static_cast<long long>(g_dbFingerPins), static_cast<long long>(g_startRefused), static_cast<long long>(g_addRefused));
}

// ======================= 2026-11-09 (plan 2026-11-09-manual-reload-remaining-guns Task 8b-8e): THE REVOLVERS + THE CHAINSAW =======
// One held gun at a time: the Schofield (Family::Break6 - break6_logic.h SMachine), the Sixshooter / Walker (Family::Gate - gate_logic.h
// GMachine) + the fan-cock after their reload (cock_logic.h CMachine), the chainsaw gun (Family::Fuel - fuel_logic.h FMachine). Every
// part we write is drawn from its REST LOCAL in the mesh file (reload_rv_rest.inc - never a live read, INV-672) x its live parent.
// Jay 2026-11-09 (spec 2026-11-09-manual-reload-remaining-guns-design.md 3.4-3.8): the Schofield "Flick open ... Downward flick", spent cases
// out on opening, "all 6 bullets in one hand, feed them in one at a time", close "by hand or upward flick"; the gate "a flick to the
// right", close "flick left" at x1.25; "Left hand, like if you were to fan fire", "Once after reload"; the chainsaw "Pour the fuel by
// hand, then pull the cord once", the bottle from the "Left hip pouch".
struct RvRest { const char* name; const char* parent; float local[12]; float world[12]; };
#include "reload_rv_rest.inc"
constexpr long kMaxRvLines = 200;   // [REVOLVER] / [CHAINSAW] event lines a run (INV-9)
long          g_nRv = 0;
volatile LONG g_nRvRefuse = 0;
bool RvLine() { return ++g_nRv <= kMaxRvLines; }
bool RvRefuseLine() { return InterlockedIncrement(&g_nRvRefuse) <= kMaxRvLines; }
bool g_rvArmedSaid = false;
enum class RvGun { None, Sch, Six, Walker, Saw };
constexpr float kRvHammerBallM = 0.06f;   // the fan-cock zone round bone_hammer (plan R3, my call)
constexpr float kRvFanSweepM = 0.03f;     // the palm's travel from its entry, any direction (plan R3; any direction 2026-11-10)
constexpr float kSawFiller[3] = { 0.0606f, 0.0909f, 0.1351f };   // the game's pour point (spec 3.8), mesh frame
constexpr float kSawFillerBallM = 0.06f;  // the spout within this of it pours (my call)
// RG3: "on its side" = the gate's direction within this of world up. RG9 (Jay: "difficult to get OPEN AND CLOSED"): his real sideways
// hold read 95-123 deg (8 real twists refused at 45) -> 130 deg (an upright gun reads ~149: still refused).
constexpr float kRvGateUpCos = -0.6428f;   // cos(130 deg)
struct RvPart { int id = -1; const RvRest* rest = nullptr; int parentId = -1; };
// RG3 (Jay: "Revolvers need the reload position lock like the shotgun per push"): the pump's PORT LOCK (reload.cpp DrivePump, PS1 /
// PS2 / review C1 + I1) for a revolver chamber - the round in the hand entering reload_snap_m of the push's start (frame 1's round)
// while the gun can take it SNAPS the hand onto the push pose (one left pulse) and starts the push; released beyond 1.5 x reload_snap_m
// (ESTIMATED from the controllers - the rendered hand sits at the lock), on a grip release, or once the hand is empty and no push plays;
// re-armed only once the RENDERED round is seen beyond 1.5 x reload_snap_m.
struct RvSnap {
    bool  snapped = false, armed = true, haveLr0 = false;
    float v0L[3] = {}, lr0L[3] = {}, est = 0.0f;
    int   ch = -1;
    bool  pushed = false;   // the push played since the snap (the lock then holds its LAST frame until released)
};
struct RvDraw {
    RvGun gun = RvGun::None;
    uintptr_t model = 0;
    uintptr_t inv = 0;            // review N1 (2026-11-10): the model's InventoryItem - the game reuses model addresses (ReloadTick)
    RvPart root, hinge, cyl, hammer, gate, holderB, round, clip, handle, start, chHolder[6], chRound[6];
    bool  fresh = false;
    bool  havePrev = false;
    float prevMesh[12] = {};
    long long prevQ = 0;
    bool  takeWas = false, gripWas = false;
    bool  inHammer = false;
    float hammerEntry[3] = {};
    bool  cordHeld = false;
    float cordGrab[3] = {};
    bool  hammerOff = false, gateOff = false, cylOff = false, roundOff = false, clipOff = false, handleOff = false, chOff = false;
    float rideG0[3] = {}, rideLr0[3] = {}, rideA0 = 0.0f;
    bool  rideLatched = false;
    reload::SPhase lastS = reload::SPhase::Closed;
    RvSnap snap;                  // RG3: the push lock
    bool  canPushPrev = false;    // last update: the gun could take a push (the snap tests it before this update's step)
    int   nextPrev = -1;          // last update: the Schofield's next empty chamber
    bool  spoutWas = false;       // RG3: the spout was at the filler (its enter pulse)
    reload::TwistWindow twist;    // RG5: the right controller's recent orientations (the gate flick)
    double lastOnSideS = 0.0;     // RG5: when the gun was last on its side
    bool  twistMissSaid = false, twistNoCtlSaid = false;
    float cordD0 = 0.0f;          // RG3: the handle-in-hand's distance from its rest at the grab (the pull counts from there)
};
RvDraw g_rv;   // game thread

const RvRest* RvRestOf(RvGun g, const char* name)
{
    const RvRest* t = g == RvGun::Sch ? kRvSch : g == RvGun::Six ? kRvSix : g == RvGun::Walker ? kRvWalker : kRvSaw;
    const int n = g == RvGun::Sch ? static_cast<int>(sizeof(kRvSch) / sizeof(kRvSch[0]))
                : g == RvGun::Six ? static_cast<int>(sizeof(kRvSix) / sizeof(kRvSix[0]))
                : g == RvGun::Walker ? static_cast<int>(sizeof(kRvWalker) / sizeof(kRvWalker[0]))
                : static_cast<int>(sizeof(kRvSaw) / sizeof(kRvSaw[0]));
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(t[i].name, name) == 0) { return &t[i]; }
    }
    return nullptr;
}
// Resolve a part BY NAME on the model + its rest row + its live parent element. False (named in the ARMED line) when any is missing.
bool RvResolve(uintptr_t model, RvGun g, const char* name, RvPart& p)
{
    p.rest = RvRestOf(g, name);
    ElemId(model, name, &p.id);
    ElemParent(model, p.id, &p.parentId);
    return p.rest && p.id >= 0 && p.parentId >= 0;
}
// A part's world = its live parent's world x rest local x delta (delta null = at rest). Hidden = the basis scaled (kHideScale). COST:
// one world read + up to two 3x4 products + one write.
bool RvDrawPart(uintptr_t model, const RvPart& p, const float* delta, bool hidden = false)
{
    if (p.id < 0 || !p.rest) { return false; }
    float pw[12], l[12], w[12];
    if (!ElemMtx(g_world, model, p.parentId, pw)) { return false; }
    if (delta) { Mul34(p.rest->local, delta, l); } else { memcpy(l, p.rest->local, sizeof(l)); }
    Mul34(pw, l, w);
    if (hidden) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) { w[4 * r + c] *= kHideScale; }
        }
    }
    return SetWorld(model, p.id, w);
}
void RvRotZ(float deg, float out[12])
{
    const float r = deg * 3.14159265358979f / 180.0f, c = std::cos(r), s = std::sin(r);
    const float m[12] = { c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0 };
    memcpy(out, m, sizeof(m));
}
// the mesh-frame world from bone_root's live world (x inv(its rest world - the mesh file))
bool RvMeshWorld(uintptr_t model, float meshW[12])
{
    float rw[12], ri[12];
    if (!g_rv.root.rest || !ElemMtx(g_world, model, g_rv.root.id, rw)) { return false; }
    RigidInv(g_rv.root.rest->world, ri);
    Mul34(rw, ri, meshW);
    return true;
}
// ONE round through the game's own add-bullet, +429 cleared for the call (INV-699 - the double-barrel's path). *after = the count after.
bool RvAddOne(uintptr_t ctrl, ItemState* st, bool oneRound, int* after)
{
    unsigned char b = 0;
    const bool haveB = oneRound && ReadU8(ctrl + kCtlClipReloadOff, &b);
    const bool cleared = haveB && b != 0 && WriteU8(ctrl + kCtlClipReloadOff, 0);
    const bool called = CallAddBullet(ctrl, st->item);
    if (cleared) { WriteU8(ctrl + kCtlClipReloadOff, b); }
    CallCtlInt(g_rounds, ctrl, after);
    st->spare = SpareOf(ctrl, st->item);
    return called;
}
// the drawn palm (the double-barrel's point) in the MESH frame
bool RvPalmMesh(const float* meshW, const float* leftPos, const float* leftR9, bool leftAchieved, const float* leftCmdPos, float out[3])
{
    float pw[3];
    if (leftAchieved && leftPos && leftR9) {
        for (int r = 0; r < 3; ++r) {
            pw[r] = leftPos[r] + leftR9[3 * r] * kDbPalmFromHandholder[0] + leftR9[3 * r + 1] * kDbPalmFromHandholder[1] +
                    leftR9[3 * r + 2] * kDbPalmFromHandholder[2];
        }
    } else if (leftCmdPos) {
        memcpy(pw, leftCmdPos, sizeof(pw));
    } else {
        return false;
    }
    float o[12], inv[12];
    if (!Ortho34(meshW, o)) { return false; }
    RigidInv(o, inv);
    XformPt(inv, reload::Vec3{ pw[0], pw[1], pw[2] }, out);
    return true;
}
// the gun's roll rate about its barrel (mesh +z), deg/s: + = the top to the RIGHT (the gun's +y toward -x, its left being +x)
float RvRollDps(const float* prev, const float* now, float dt)
{
    if (!(dt > 1e-4f)) { return 0.0f; }
    float yp[3] = { prev[1], prev[5], prev[9] }, yn[3] = { now[1], now[5], now[9] }, zn[3] = { now[2], now[6], now[10] };
    for (float* v : { yp, yn, zn }) {
        const float l = Len3(v[0], v[1], v[2]);
        if (!(l > 1e-6f)) { return 0.0f; }
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
    const float cx = yp[1] * yn[2] - yp[2] * yn[1], cy = yp[2] * yn[0] - yp[0] * yn[2], cz = yp[0] * yn[1] - yp[1] * yn[0];
    float s = cx * zn[0] + cy * zn[1] + cz * zn[2];
    s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    const float r = std::asin(s) * 180.0f / 3.14159265358979f / dt;
    return std::isfinite(r) ? r : 0.0f;
}
// the hand lock at a mesh-frame hand (the part IS the l_hand bone - the double-barrel's identity row) + the finger word
void RvPublishHand(bool lock, const float* meshO, const float* handMesh, reload::DbAnim anim, float frame, bool held, ULONGLONG now)
{
    float part[12];
    const bool havePart = lock && meshO && handMesh && (Mul34(meshO, handMesh, part), true);
    if (!g_dbIdRow.key) {
        g_dbIdRow.key = "dbarrel identity";
        const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        memcpy(g_dbIdRow.handR, I, sizeof(I));
    }
    HandsPublish(havePart, havePart ? &g_dbIdRow : nullptr, havePart ? part : nullptr, nullptr, true, now);
    const LONG64 w = anim != reload::DbAnim::None ? reload::DbAnimPack(PumpClockQ(), anim, frame, held) : 0;
    InterlockedExchange64(&g_dbAnimWord, w);
}
// The game's own sound for a step, from the gun's fx row (reload_fx_table.inc; none on a row = silent). 2026-11-10 (Jay: "Sort out the
// sounds for the revolver & chainsaw"): the revolver scripts DO carry them (m_fpp_walker / sixshooter / schofield.scr, OUR Data0.pak -
// the old "no reload sounds" note was wrong) and the rows are generated now. The double-barrel's slot mapping: open = the magOut slot,
// seat = seat, close = the rack slot, the fan-cock = the cock slot. COST: a table read.
void RvPlayFx(reload::Fx slot)
{
    if (slot == reload::Fx::None) { return; }
    int ns = 0;
    const reload::Snd* snd = reload::SoundsFor(g_gun.fx, slot, &ns);
    for (int i = 0; i < ns; ++i) {
        if (!GunSoundPlay(g_curPlayer, snd[i].name, snd[i].vol, snd[i].delayS, snd[i].startS)) { PumpSoundRefused(snd[i].name); }
    }
}
void RvLeave(const char* why)
{
    if (g_gun.family != reload::Family::Break6 && g_gun.family != reload::Family::Gate && g_gun.family != reload::Family::Fuel) { return; }
    InterlockedExchange64(&g_dbAnimWord, 0);
    // the drivers raised the controllers' grip points (the ride, the push lock, the cord); the next gun's driver raises them again if
    // it needs them
    if (!g_pumpSilencedSet) { InterlockedExchange(&g_pumpWantCtl, 0); }
    g_rv.snap = RvSnap{};   // RG3: no lock survives a put-away / switch
    if (RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] AWAY - %s: the parts we drew are put back at the next draw of this model.", why);
    }
}
// Every part we may have moved back at its rest on THIS model (the held one) - d = its record (g_rv, or a parked one). COST: one write
// per moved part.
void RvRestore(uintptr_t model, RvDraw& d)
{
    if (d.hammerOff && RvDrawPart(model, d.hammer, nullptr)) { d.hammerOff = false; }
    if (d.gateOff && RvDrawPart(model, d.gate, nullptr)) { d.gateOff = false; }
    if (d.cylOff && RvDrawPart(model, d.cyl, nullptr)) { d.cylOff = false; }
    if (d.roundOff && RvDrawPart(model, d.round, nullptr)) { d.roundOff = false; }
    if (d.clipOff && RvDrawPart(model, d.clip, nullptr)) { d.clipOff = false; }
    if (d.handleOff && RvDrawPart(model, d.handle, nullptr)) { d.handleOff = false; }
    if (d.chOff) {
        bool all = RvDrawPart(model, d.hinge, nullptr);
        all = RvDrawPart(model, d.cyl, nullptr) && all;   // the cylinder after its parent (the hinge), the rounds after theirs
        for (int j = 0; j < 6; ++j) { all = RvDrawPart(model, d.chRound[j], nullptr) && all; }
        if (all) { d.chOff = false; }
    }
}
bool RvMoved(const RvDraw& d)
{
    return d.hammerOff || d.gateOff || d.cylOff || d.roundOff || d.clipOff || d.handleOff || d.chOff;
}
// Review N1 (2026-11-10; Jay: "Fix N1"): a revolver / chainsaw whose parts we left moved, PARKED when another revolver / chainsaw is drawn
// (g_rv holds one gun) - its parts go home at that gun's next draw (ClassifyRvDraw) or on the off switch while it is held (ReloadOffTick).
// Keyed by (model, InventoryItem): the game reuses model addresses. Never a write to a parked model unless the right hand holds it.
// A full park drops its oldest record, NAMED (its parts stay where we left them). Game thread.
constexpr int kRvParkMax = 4;
RvDraw g_rvPark[kRvParkMax];
int    g_nRvPark = 0;
int RvParkFind(uintptr_t model, uintptr_t inv)
{
    for (int i = 0; i < g_nRvPark; ++i) {
        if (g_rvPark[i].model == model && g_rvPark[i].inv == inv) { return i; }
    }
    return -1;
}
void RvParkDrop(int i)
{
    for (int k = i; k + 1 < g_nRvPark; ++k) { g_rvPark[k] = g_rvPark[k + 1]; }
    if (g_nRvPark > 0) { g_rvPark[--g_nRvPark] = RvDraw{}; }
}
void RvPark(const RvDraw& d)
{
    const int at = RvParkFind(d.model, d.inv);
    if (at >= 0) { RvParkDrop(at); }
    if (g_nRvPark == kRvParkMax) {
        if (RvLine()) {
            // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [REVOLVER] PARK FULL - model %p (gun %d) dropped from the park: its moved parts stay where we left them.",
                reinterpret_cast<void*>(g_rvPark[0].model), static_cast<int>(g_rvPark[0].gun));
        }
        RvParkDrop(0);
    }
    g_rvPark[g_nRvPark++] = d;
    if (RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] PARKED model %p (gun %d): its moved parts go home at its next draw (%d parked).",
            reinterpret_cast<void*>(d.model), static_cast<int>(d.gun), g_nRvPark);
    }
}

// Classify a held revolver / chainsaw (ClassifyModel, once per model change): every part by NAME + its rest row. Refused = the game
// reloads it this draw. The ARMED line (the first such draw of the session) is logged unconditionally.
bool ClassifyRvDraw(uintptr_t model, uintptr_t item, reload::Family fam, const char* meshNorm, const char* meshShown, const char* prefix)
{
    const RvDraw keep = g_rv;
    const uintptr_t inv = HeldItemInventoryItem(model);
    const bool sameGun = keep.model == model && keep.inv == inv;
    if (keep.model && !sameGun && RvMoved(keep)) { RvPark(keep); }   // review N1: another gun's moved parts wait for its next draw
    g_rv = RvDraw{};
    RvDraw& d = g_rv;
    d.model = model;
    d.inv = inv;
    const bool walker = meshNorm && std::strstr(meshNorm, "walker");
    d.gun = fam == reload::Family::Break6 ? RvGun::Sch : fam == reload::Family::Fuel ? RvGun::Saw : walker ? RvGun::Walker : RvGun::Six;
    bool ok = RvResolve(model, d.gun, "bone_root", d.root);
    const char* missing = ok ? nullptr : "bone_root";
    auto need = [&](const char* nm, RvPart& p) {
        if (!RvResolve(model, d.gun, nm, p) && !missing) { missing = nm; }
    };
    int chFound = 0;
    if (d.gun == RvGun::Sch) {
        need("bone_reload_schofield", d.hinge);
        need("bone_cylinder", d.cyl);
        need("bone_hammer", d.hammer);
        static const char* const hn[6] = { "holder_schofield_bullet00", "holder_schofield_bullet01", "holder_schofield_bullet02",
                                           "holder_schofield_bullet03", "holder_schofield_bullet04", "holder_schofield_bullet05" };
        static const char* const rn[6] = { "schofield_full_bullet_a", "schofield_full_bullet_b", "schofield_full_bullet_c",
                                           "schofield_full_bullet_d", "schofield_full_bullet_e", "schofield_full_bullet_f" };
        for (int j = 0; j < 6; ++j) {   // the rounds are drawn only when all six resolve (else the gun's own - named, not refused)
            const bool h = RvResolve(model, d.gun, hn[j], d.chHolder[j]);
            const bool r = RvResolve(model, d.gun, rn[j], d.chRound[j]);
            chFound += (h && r) ? 1 : 0;
        }
    } else if (d.gun == RvGun::Saw) {
        need("bone_clip", d.clip);
        need("bone_line_handle", d.handle);
        need("bone_line_start", d.start);
    } else {
        need("bone_cylinder", d.cyl);
        need("bone_hammer", d.hammer);
        need("bone_reload_hammer", d.gate);
        need("holder_bullet", d.holderB);
        need("full_bullet", d.round);
    }
    if (!g_rvArmedSaid) {
        g_rvArmedSaid = true;
        const char* hipOff = HolsterLeftTakeRefusal();
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] ARMED (first revolver / chainsaw draw, logged unconditionally) - manual_reload=%d | mesh '%s', prefix '%s', "
            "gun %d (1 Schofield, 2 Sixshooter, 3 Walker, 4 chainsaw) | parts by name: root %d, hinge %d, cylinder %d, hammer %d, gate %d, "
            "round %d (holder %d), bottle %d, T-handle %d (start %d), Schofield chambers resolved %d of 6 | flick %.0f deg/s (the Schofield), "
            "gate: the right controller's twist, open %.0f / close %.0f deg in 0.15 s, only on its side (the gate within %.0f deg of up) | "
            "push snap %.3f m (reload_snap_m), release "
            "beyond %.3f m | fan-cock ball %.2f m, sweep %.2f m | pour ball %.2f m, cord: the handle in the hand, any direction | left zones %s%s | this draw: %s",
            ManualReload ? 1 : 0, meshShown, prefix, static_cast<int>(d.gun), d.root.id, d.hinge.id, d.cyl.id, d.hammer.id, d.gate.id,
            d.round.id, d.holderB.id, d.clip.id, d.handle.id, d.start.id, chFound, ReloadFlickDps, reload::kGateOpenTwistDeg,
            reload::kGateCloseTwistDeg, std::acos(kRvGateUpCos) * 180.0f / 3.14159265358979f, ReloadSnapM, 1.5f * ReloadSnapM,
            kRvHammerBallM, kRvFanSweepM, kSawFillerBallM, hipOff ? "*** REFUSED: " : "not refused", hipOff ? hipOff : "",
            missing ? missing : "manual");
    }
    if (missing) {
        if (RvRefuseLine()) {
            // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [REVOLVER] REFUSED model %p item %p (mesh '%s'): no %s on the model (or no rest row) - it reloads as today.",
                reinterpret_cast<void*>(model), reinterpret_cast<void*>(item), meshShown, missing);
        }
        return false;
    }
    if (d.gun == RvGun::Sch && chFound != 6 && RvRefuseLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] SCHOFIELD CHAMBERS - only %d of 6 holder / round pairs resolved by name: the rounds stay the game's "
            "(not hidden / shown by the count); the reload itself works.", chFound);
    }
    d.fresh = true;
    // the same gun drawn again - or a parked one (review N1): whatever we left on it goes home
    const int pk = sameGun ? -1 : RvParkFind(model, inv);
    const RvDraw* from = sameGun ? &keep : pk >= 0 ? &g_rvPark[pk] : nullptr;
    if (from && from->gun == d.gun) {
        d.hammerOff = from->hammerOff; d.gateOff = from->gateOff; d.cylOff = from->cylOff; d.roundOff = from->roundOff;
        d.clipOff = from->clipOff; d.handleOff = from->handleOff; d.chOff = from->chOff;
        RvRestore(model, d);
        if (pk >= 0 && RvLine()) {
            // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [REVOLVER] UNPARKED model %p (gun %d): its parts back at rest%s.", reinterpret_cast<void*>(model),
                static_cast<int>(d.gun), RvMoved(d) ? " - *** SOME NOT WRITTEN (still flagged) ***" : "");
        }
    }
    if (pk >= 0) { RvParkDrop(pk); }
    if (RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] DRAW model %p item %p (mesh '%s'): manual - gun %d.", reinterpret_cast<void*>(model),
            reinterpret_cast<void*>(item), meshShown, static_cast<int>(d.gun));
    }
    return true;
}

// The fan-cock after a revolver reload (cock_logic.h): the palm in the hammer ball, swept BACK (-z, mesh frame) from its entry. Draws the
// cocked hammer while cocked; puts it back at rest once after. Returns the machine's R2 withhold. COST: one world read + a few compares.
// The push lock, one update (RvSnap above). roundW = the round in the hand (l_hand x frame 1's shellH), targetW = where frame 1 puts it
// at the chamber; lrL = (left - right controller) in the mesh's axes. *edge = it snapped THIS update (the step machine's nearLoad - one
// push per entry), *farOut = released this update or the round beyond 1.5 x reload_snap_m (its farLoad). COST: a distance, or while locked
// one vector sum; the snap / release are events.
void RvSnapStep(RvSnap& s, bool canPush, bool gripDown, bool holding, bool haveRound, const float roundW[3], const float targetW[3],
                const float* meshW, bool haveLrL, const float lrL[3], int ch, const char* gun, bool* edge, bool* farOut)
{
    *edge = false;
    *farOut = false;   // (not "far": a Windows header macro)
    const float vw[3] = { haveRound ? roundW[0] - targetW[0] : 0.0f, haveRound ? roundW[1] - targetW[1] : 0.0f,
                          haveRound ? roundW[2] - targetW[2] : 0.0f };
    const float dist = haveRound ? Len3(vw[0], vw[1], vw[2]) : 1e9f;
    if (s.snapped) {
        if (haveLrL) {
            if (!s.haveLr0) { memcpy(s.lr0L, lrL, sizeof(s.lr0L)); s.haveLr0 = true; }
            s.est = Len3(s.v0L[0] + lrL[0] - s.lr0L[0], s.v0L[1] + lrL[1] - s.lr0L[1], s.v0L[2] + lrL[2] - s.lr0L[2]);
        }   // a stale controller pair holds the last estimate (never releases on staleness - the pump's rule)
        const char* rel = s.est > 1.5f * ReloadSnapM ? "the hand left the chamber (beyond 1.5 x reload_snap_m, from the controllers)"
                        : !gripDown ? "the grip let go" : !holding ? "the hand is empty and no push plays" : nullptr;
        if (rel) {
            s.snapped = false;
            s.armed = false;
            *farOut = true;
            if (RvLine()) {
                // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
                LOG("reload: [REVOLVER] %s SNAP RELEASED - %s (estimated %.3f m from the chamber): the hand eases back (100 ms).", gun, rel,
                    s.est);
            }
        }
        return;
    }
    if (!s.armed && dist > 1.5f * ReloadSnapM) { s.armed = true; }
    *farOut = dist > 1.5f * ReloadSnapM;
    float v0L[3];
    if (s.armed && canPush && gripDown && dist <= ReloadSnapM && meshW && DirToLocal(meshW, vw, v0L)) {
        s.snapped = true;
        s.est = dist;
        memcpy(s.v0L, v0L, sizeof(s.v0L));
        s.haveLr0 = haveLrL;
        if (haveLrL) { memcpy(s.lr0L, lrL, sizeof(s.lr0L)); }
        s.ch = ch;
        s.pushed = false;
        *edge = true;
        if (HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
        if (RvLine()) {
            // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [REVOLVER] %s SNAP - the round in the hand %.3f m from chamber %d's push start (reload_snap_m %.3f): the left hand "
                "LOCKED to the push pose (100 ms blend); %s.", gun, dist, ch, ReloadSnapM,
                HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)");
        }
    }
}

// 2026-11-09: the RG6 mesh-frame check (answered AGREE on RG7) was removed from BETA for the tester package (Jay: "Strip both");
// DEV keeps it. Backup backups/20261109_225839_pre_beta_pkg_probe_strip/.

bool RvCock(uintptr_t model, ItemState* st, bool reloadDone, bool freshDraw, int rounds, bool havePalm, const float palmM[3],
            const float* meshW, bool* fanPose)
{
    RvDraw& d = g_rv;
    reload::CInput in{};
    in.manualOn = ManualReload;
    in.isRevolver = true;
    // Review N3 (2026-11-10; Jay: "Fix N3"): the driver's wasFresh - d.fresh is cleared before this call, so reading it here was always
    // false and cock_logic.h's fresh-draw rule (the zone entry forgotten; the need and the cocked hammer stay) never ran. The driver's own
    // entry is forgotten with it, so a redraw's sweep counts from a new entry.
    in.freshDraw = freshDraw;
    if (freshDraw) { d.inHammer = false; }
    in.reloadDone = reloadDone;
    in.rounds = rounds;
    // RG4 (Jay: "whatever the zone is for the hammer you're detecting seems to be off" -> "Yes, build both"): the zone's centre = where
    // Jay's approved fan pose puts the palm point (reload_extra_poses.inc kFanPalm*, mesh frame - the same point RvPalmMesh measures);
    // RG4's centre (bone_hammer's pivot) sat 6.5 cm from it on the Walker / Sixshooter, outside the 6 cm ball.
    const float* ho = d.gun == RvGun::Sch ? reload::kFanPalmSch : d.gun == RvGun::Walker ? reload::kFanPalmWalker : reload::kFanPalmSix;
    const bool haveH = meshW != nullptr;
    const bool inBall = havePalm && haveH && Len3(palmM[0] - ho[0], palmM[1] - ho[1], palmM[2] - ho[2]) <= kRvHammerBallM;
    // Review N2 (2026-11-10; Jay: "Fix N2"): the entry counts only while a cock is owed - or comes due THIS update (the machine starts
    // its own entry on the same update: needed + in the zone) - so the sweep is measured from where the palm was when the cock came due,
    // never from an entry made before the reload ended (a palm resting in the ball cocked on the next frame with no fan, no pulse).
    const bool owed = st->cm.needed() || reloadDone;
    if (inBall && owed && !d.inHammer) {
        memcpy(d.hammerEntry, palmM, sizeof(d.hammerEntry));
        // RG3 (Jay: "the hammer needs haptic ... feedback"): the hammer zone's ENTER pulse while a cock is owed (body zones pulse on enter)
        if (HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
    }
    d.inHammer = inBall && owed;
    in.palmInZone = inBall;
    // 2026-11-10 (Jay: "Took me like 2/3 fans over it to get the firing to start" - the enter pulse was felt on the misses -> "Yes"):
    // the travel from the entry in ANY direction counts; it was the -z (back along the barrel) part only.
    in.palmTravelM = inBall ? Len3(palmM[0] - d.hammerEntry[0], palmM[1] - d.hammerEntry[1], palmM[2] - d.hammerEntry[2]) : 0.0f;
    const reload::COut o = st->cm.Update(in, kRvFanSweepM);
    *fanPose = st->cm.needed() && inBall;
    // the cock's sound = the row's COCK slot (the game's hammer_pull from the gun's Shot sequence - the rack slot is the close's)
    RvPlayFx(o.fx == reload::Fx::RackDone ? reload::Fx::Cock : o.fx);
    if (o.ev == reload::CEvent::Cocked) {
        // RG3 (Jay: "so the player knows they can fire after a reload"): a STRONGER, longer pulse on BOTH hands - the gun hand included
        const float amp = HolsterHaptic * 1.5f > 1.0f ? 1.0f : HolsterHaptic * 1.5f;
        if (HolsterHaptic > 0.0f) {
            PulseController(kLeftHand, amp, 2 * kPulseMs);
            PulseController(kRightHand, amp, 2 * kPulseMs);
        }
        if (RvLine()) {
            // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [REVOLVER] COCKED - the palm moved %.3f m through the hammer zone: R2 free (once per reload); %s.", in.palmTravelM,
                HolsterHaptic > 0.0f ? "both hands pulsed (x1.5, 2 x the pulse)" : "no pulse (holster_haptic 0)");
        }
    }
    if (o.ev == reload::CEvent::CockNeeded && RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [REVOLVER] COCK NEEDED - the reload is done: sweep the left palm over the hammer (ball %.2f m, %.2f m any direction); "
            "R2 withheld until then.", kRvHammerBallM, kRvFanSweepM);
    }
    if (o.cocked) {
        // the game's own cocked local (reload_extra_poses.inc): the Schofield's / the Walker's rotation, the hammer's OWN rest translation
        const float* ck = d.gun == RvGun::Sch ? reload::kCockSchLocal : reload::kCockWalkerLocal;
        float l[12];
        memcpy(l, ck, sizeof(l));
        l[3] = d.hammer.rest->local[3]; l[7] = d.hammer.rest->local[7]; l[11] = d.hammer.rest->local[11];
        float pw[12], w[12];
        if (ElemMtx(g_world, model, d.hammer.parentId, pw)) {
            Mul34(pw, l, w);
            if (SetWorld(model, d.hammer.id, w)) { d.hammerOff = true; }
        }
    } else if (d.hammerOff && RvDrawPart(model, d.hammer, nullptr)) {
        d.hammerOff = false;   // the hammer fell (a shot) or the need was reset: back at its rest once (our write persists - INV-672)
    }
    return o.withholdR2;
}

// One update of the held Schofield (ReloadTick, game thread). [LOADBEARING: manual reload revolvers] (manual_reload) the Schofield's /
// the gate revolvers' / the chainsaw's every step: the parts drawn where the steps put them, ONE round per push, R2 withheld while not
// shut / not cocked / not started. COST per update: one rounds call, two world reads (bone_root, l_hand), the step machine, a 3x3 rate,
// the drawn parts (a hinge + a cylinder + six rounds while open; <= 3 writes otherwise), HandsPublish + one interlocked store.
const char* DriveBreak6(uintptr_t ctrl, ItemState* st, void* playerModel, int lHandId, const float* leftPos, const float* leftR9,
                        bool leftAchieved, const float* leftCmdPos, ULONGLONG now)
{
    using reload::SEvent;
    using reload::SPhase;
    RvDraw& d = g_rv;
    const uintptr_t model = g_gun.model;
    InterlockedExchange(&g_pumpWantCtl, 1);   // the controllers' grip points (the hand close's ride, the double-barrel's rule)
    const long long nowQ = PumpClockQ();
    float meshW[12], meshO[12], lh[12];
    const bool haveMesh = RvMeshWorld(model, meshW) && Ortho34(meshW, meshO);
    const bool haveHand = playerModel && lHandId >= 0 && ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh);
    const reload::DbGeom& geom = reload::kDbGeomSch;
    const float angle0 = st->sm.angle();
    const SPhase phase0 = st->sm.phase();
    reload::SInput in{};
    in.manualOn = ManualReload;
    in.freshDraw = d.fresh;
    in.gripDown = g_gripDown != 0;
    in.inHip = HolsterLeftPouchHeld();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    const bool takeNow = in.gripDown && in.inHip;
    if (takeNow && !d.takeWas) { st->spare = SpareOf(ctrl, st->item); }
    d.takeWas = takeNow;
    in.spare = st->spare;
    in.nowS = static_cast<double>(nowQ) / 4000.0;
    float palmM[3] = {}, palmB[3] = {};
    const bool havePalm = haveMesh && RvPalmMesh(meshW, leftPos, leftR9, leftAchieved, leftCmdPos, palmM);
    reload::DbZone zone;
    if (havePalm) {
        reload::DbToBarrelsG(geom, angle0, palmM, palmB);
        zone = reload::DbZoneOfG(geom, palmB);
        in.nearTop = zone.top;
    }
    // the hand close's ride (the double-barrel's controller rule)
    float lr[3] = {}, lrL[3] = {};
    const bool haveLR = PumpControllersLR(lr) && haveMesh && DirToLocal(meshW, lr, lrL);
    if (phase0 == SPhase::Riding && haveLR) {
        if (!d.rideLatched) {
            d.rideLatched = true;
            memcpy(d.rideG0, palmM, sizeof(d.rideG0));
            memcpy(d.rideLr0, lrL, sizeof(d.rideLr0));
            d.rideA0 = angle0;
        } else {
            const float p[3] = { d.rideG0[0] + lrL[0] - d.rideLr0[0], d.rideG0[1] + lrL[1] - d.rideLr0[1], d.rideG0[2] + lrL[2] - d.rideLr0[2] };
            in.rideAngle = reload::DbRideAngleG(geom, d.rideG0, p, d.rideA0, reload::kSchOpenDeg);
            in.rideValid = true;
        }
    }
    if (phase0 != SPhase::Riding) { d.rideLatched = false; }
    // RG3: THE PUSH LOCK (RvSnapStep) at the next empty chamber - the round in the hand vs frame 1's round at that chamber, on the barrel
    // at its angle now. The gun can take a push = open, rounds in the hand, a chamber empty, no push playing (last update's step).
    {
        float rw[3] = {}, tw[3] = {};
        const int nx = d.snap.snapped ? d.snap.ch : d.nextPrev;
        const bool haveRound = haveHand && haveMesh && nx >= 0 && st->sm.hand() > 0;
        if (haveRound) {
            const float* s1 = reload::kSchLoad[0].shellH[0];
            XformPt(lh, reload::Vec3{ s1[3], s1[7], s1[11] }, rw);
            float h[12], tr[12] = { 1, 0, 0, reload::kSchChamberDelta[nx][0], 0, 1, 0, reload::kSchChamberDelta[nx][1], 0, 0, 1,
                                    reload::kSchChamberDelta[nx][2] }, a[12], b[12];
            reload::DbMeshHingeG(geom, angle0, h);
            Mul34(h, tr, a);
            Mul34(a, reload::kSchLoad[0].shellM[0], b);
            XformPt(meshW, reload::Vec3{ b[3], b[7], b[11] }, tw);
        }
        const bool holding = st->sm.hand() > 0 || phase0 == SPhase::Loading;
        RvSnapStep(d.snap, d.canPushPrev, in.gripDown, holding, haveRound, rw, tw, haveMesh ? meshW : nullptr, haveLR, lrL, nx,
                   "SCHOFIELD", &in.nearLoad, &in.farLoad);
    }
    float upDps = 0.0f;
    if (haveMesh) {
        if (d.havePrev && nowQ > d.prevQ) { upDps = reload::DbMuzzleUpDps(d.prevMesh, meshW, static_cast<float>(nowQ - d.prevQ) / 4000.0f); }
        memcpy(d.prevMesh, meshW, sizeof(d.prevMesh));
        d.prevQ = nowQ;
        d.havePrev = true;
    }
    in.flickUpDps = upDps > 0.0f ? upDps : 0.0f;
    in.flickDownDps = upDps < 0.0f ? -upDps : 0.0f;   // Jay: "Downward flick" opens it
    reload::SParams p;
    p.flickDps = ReloadFlickDps;
    p.openDeg = reload::kSchOpenDeg;
    const bool wasFresh = d.fresh;
    d.fresh = false;
    const reload::SOut o = st->sm.Update(in, p);
    d.canPushPrev = o.phase == SPhase::Open && o.hand > 0 && o.loadChamber >= 0;   // RG3: the next update's snap test
    d.nextPrev = o.phase == SPhase::Open ? o.loadChamber : -1;
    if (d.snap.snapped && o.phase == SPhase::Loading) { d.snap.pushed = true; }
    int rAfter = rounds;
    bool called = true;
    if (o.callAddBullet) {
        called = RvAddOne(ctrl, st, true, &rAfter);
        if (!called) { st->sm.AddFailed(o.addChamber); }
    }
    const char* pulse = !o.haptic ? "no pulse" : HolsterHaptic > 0.0f ? "pulse" : "no pulse (holster_haptic 0)";
    if (o.haptic && HolsterHaptic > 0.0f) {
        const bool flick = o.Has(SEvent::FlickOpen) || (o.Has(SEvent::Closed) && phase0 == SPhase::Closing);
        PulseController(flick ? kRightHand : kLeftHand, HolsterHaptic, kPulseMs);
    }
    for (int k = 0; k < o.nFx; ++k) {
        RvPlayFx(o.fx[k] == reload::SFx::Open ? reload::Fx::MagGrab : o.fx[k] == reload::SFx::Seat ? reload::Fx::Seat
                 : o.fx[k] == reload::SFx::Close ? reload::Fx::RackDone : reload::Fx::None);
    }
    if (o.ev != SEvent::None && o.ev != SEvent::Shot && RvLine()) {   // RG3 fix: count EVENTS, not updates (the cap ran out in 1.5 s)
        // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [REVOLVER] SCHOFIELD %s - phase %d -> %d at %.1f deg, hand %d, rounds %d -> %d (spare %d)%s, the muzzle %+.0f deg/s "
            "(flick %.0f); %s.", o.Has(SEvent::FlickOpen) ? "FLICK OPEN" : o.Has(SEvent::Opened) ? "OPENED" : o.Has(SEvent::CasesOut) ? "CASES OUT"
            : o.Has(SEvent::Take) ? "TAKE" : o.Has(SEvent::TakeNoRound) ? "TAKE REFUSED" : o.Has(SEvent::LoadStart) ? "PUSH START"
            : o.Has(SEvent::RoundIn) ? "ROUND IN" : o.Has(SEvent::LoadDone) ? "PUSH DONE" : o.Has(SEvent::HandCleared) ? "HAND CLEARED"
            : o.Has(SEvent::FlickClose) ? "FLICK CLOSE" : o.Has(SEvent::CloseGrab) ? "CLOSE GRAB" : o.Has(SEvent::Closed) ? "CLOSED" : "EVENT",
            static_cast<int>(phase0), static_cast<int>(o.phase), o.angle, o.hand, rounds, rAfter, st->spare,
            !called ? " *** the add-bullet FAULTED ***" : "", upDps, ReloadFlickDps, pulse);
    }
    // DRAW: the hinge (+ the cylinder and the six rounds - written ourselves: whether the hinge carries its children is [UNKNOWN])
    float hingeW[12] = {}, cylW[12] = {};
    bool haveHinge = false;
    if (haveMesh && (o.angle > 0.0f || d.chOff)) {
        float h[12], t[12];
        reload::DbMeshHingeG(geom, o.angle, h);
        reload::DbMul34(h, reload::kSchHingeRest, t);
        Mul34(meshW, t, hingeW);
        haveHinge = SetWorld(model, d.hinge.id, hingeW);
        if (haveHinge) {
            Mul34(hingeW, d.cyl.rest->local, cylW);
            SetWorld(model, d.cyl.id, cylW);
            d.chOff = o.angle > 0.0f;
        }
    }
    const bool loading = o.phase == SPhase::Loading;
    const reload::DbFrame* f = nullptr;
    int fk = 1;
    if (loading) {
        fk = static_cast<int>(o.loadFrame);
        f = &reload::DbFrameAt(reload::DbAnim::LoadSch, fk);
    }
    if (haveHinge && d.chRound[5].id >= 0) {
        for (int j = 0; j < 6; ++j) {
            const bool pushing = loading && o.loadChamber == j;
            const bool live = o.ch[j] == reload::DChamber::Live;
            float w[12];
            if (pushing && f) {   // the round on the push's path, chamber j (the barrel's closed frame x the hinge x the chamber's delta)
                float h[12], tr[12] = { 1, 0, 0, reload::kSchChamberDelta[j][0], 0, 1, 0, reload::kSchChamberDelta[j][1], 0, 0, 1,
                                        reload::kSchChamberDelta[j][2] }, a[12], b[12];
                reload::DbMeshHingeG(geom, o.angle, h);
                Mul34(h, tr, a);
                Mul34(a, f->shellM[0], b);
                Mul34(meshW, b, w);
                SetWorld(model, d.chRound[j].id, w);
            } else if (!live && o.hand > 0 && !loading && o.loadChamber == j && haveHand) {   // the next round, in the hand (Jay: only that one)
                Mul34(lh, reload::kSchLoad[0].shellH[0], w);
                SetWorld(model, d.chRound[j].id, w);
            } else {
                float hw2[12], rw[12];
                Mul34(cylW, d.chHolder[j].rest->local, hw2);
                Mul34(hw2, d.chRound[j].rest->local, rw);
                if (!live) {
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) { rw[4 * r + c] *= kHideScale; }
                    }
                }
                SetWorld(model, d.chRound[j].id, rw);
            }
        }
    }
    if (!haveHinge && d.chOff && haveMesh) { RvRestore(model, d); }
    // the hand: locked while pushing (the push frame at its chamber) / riding; fingers: the push, the fan-cock, else none
    bool fan = false;
    const bool cockR2 = RvCock(model, st, o.Has(SEvent::ReloadDone), wasFresh, rAfter, havePalm, palmM, haveMesh ? meshW : nullptr, &fan);
    float handM[12];
    bool lock = false;
    // RG3: locked while the push plays AND while snapped (the shotgun's lock): before the push frame 1, after it its last frame
    const int lc = loading ? o.loadChamber : d.snap.ch;
    if ((loading || d.snap.snapped) && haveMesh && lc >= 0 && lc < 6) {
        const reload::DbFrame* lf = loading && f ? f : &reload::DbFrameAt(reload::DbAnim::LoadSch, d.snap.pushed ? reload::kSchLoadFrames : 1);
        float h[12], tr[12] = { 1, 0, 0, reload::kSchChamberDelta[lc][0], 0, 1, 0, reload::kSchChamberDelta[lc][1], 0,
                                0, 1, reload::kSchChamberDelta[lc][2] }, a[12];
        reload::DbMeshHingeG(geom, o.angle, h);
        Mul34(h, tr, a);
        Mul34(a, lf->hand, handM);
        lock = true;
    }
    const reload::DbAnim an = loading || o.hand > 0 ? reload::DbAnim::LoadSch : fan ? reload::DbAnim::FanSch : reload::DbAnim::None;
    RvPublishHand(lock, meshO, lock ? handM : nullptr, an, loading ? o.loadFrame : 1.0f, !loading, now);
    if (wasFresh && RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] SCHOFIELD FIRST UPDATE - rounds %d, chambers live %d, mesh frame %s, hand %s.", rounds,
            [&]() { int n = 0; for (int j = 0; j < 6; ++j) { n += o.ch[j] == reload::DChamber::Live ? 1 : 0; } return n; }(),
            haveMesh ? "read" : "*** NOT READ ***", haveHand ? "read" : "not read");
    }
    d.lastS = o.phase;
    Publish(o.withholdR2 || cockR2, o.phase != SPhase::Closed || (havePalm && zone.top));
    return o.angle > 0.0f ? "the barrel off shut" : "shut";
}

// One update of the held Sixshooter / Walker (ReloadTick). COST: as DriveBreak6, the gate + the cylinder + the carried round instead.
const char* DriveGate(uintptr_t ctrl, ItemState* st, void* playerModel, int lHandId, const float* leftPos, const float* leftR9,
                      bool leftAchieved, const float* leftCmdPos, ULONGLONG now)
{
    using reload::GEvent;
    RvDraw& d = g_rv;
    const uintptr_t model = g_gun.model;
    const long long nowQ = PumpClockQ();
    const bool walker = d.gun == RvGun::Walker;
    float meshW[12], meshO[12], lh[12];
    const bool haveMesh = RvMeshWorld(model, meshW) && Ortho34(meshW, meshO);
    const bool haveHand = playerModel && lHandId >= 0 && ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh);
    reload::GInput in{};
    in.manualOn = ManualReload;
    in.freshDraw = d.fresh;
    in.gripDown = g_gripDown != 0;
    in.inHip = HolsterLeftPouchHeld();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    const bool takeNow = in.gripDown && in.inHip;
    if (takeNow && !d.takeWas) { st->spare = SpareOf(ctrl, st->item); }
    d.takeWas = takeNow;
    in.spare = st->spare;
    in.nowS = static_cast<double>(nowQ) / 4000.0;
    float palmM[3] = {};
    const bool havePalm = haveMesh && RvPalmMesh(meshW, leftPos, leftR9, leftAchieved, leftCmdPos, palmM);
    const float* mouth = walker ? reload::kWalkerMouth : reload::kSixMouth;
    const reload::DbAnim la = walker ? reload::DbAnim::LoadWalker : reload::DbAnim::LoadSix;
    const reload::DbAnim hold = walker ? reload::DbAnim::HoldWalker : reload::DbAnim::HoldSix;   // RG6: the held pose
    // RG3: THE PUSH LOCK (RvSnapStep) at the gate's loading chamber - the round in the hand vs frame 1's round there
    InterlockedExchange(&g_pumpWantCtl, 1);   // the controllers' grip points (the lock's release estimate)
    float lr[3] = {}, lrL[3] = {};
    const bool haveLR = PumpControllersLR(lr) && haveMesh && DirToLocal(meshW, lr, lrL);
    {
        float rw[3] = {}, tw[3] = {};
        const bool haveRound = haveHand && haveMesh && st->gm.hand() > 0;
        if (haveRound) {
            const reload::DbFrame& f1 = reload::DbFrameAt(la, 1);
            const reload::DbFrame& fh = reload::DbFrameAt(hold, 1);   // RG6: the round as HELD (Jay's frame-1 pinch)
            XformPt(lh, reload::Vec3{ fh.shellH[0][3], fh.shellH[0][7], fh.shellH[0][11] }, rw);
            XformPt(meshW, reload::Vec3{ f1.shellM[0][3], f1.shellM[0][7], f1.shellM[0][11] }, tw);
        }
        RvSnapStep(d.snap, d.canPushPrev, in.gripDown, st->gm.hand() > 0 || st->gm.loading(), haveRound, rw, tw,
                   haveMesh ? meshW : nullptr, haveLR, lrL, 0, walker ? "WALKER" : "SIXSHOOTER", &in.nearLoad, &in.farLoad);
    }
    // RG3 (Jay: "way too easy to open/close accidentally (Sideways controller rotation, remember)"): ON ITS SIDE = the direction from the
    // cylinder's axis out to the loading chamber (mesh frame, across the barrel) within 45 deg of world up (Y, STATE U-52)
    float gateUp = -1.0f;
    if (haveMesh && d.cyl.rest) {
        float gx = mouth[0] - d.cyl.rest->world[3], gy = mouth[1] - d.cyl.rest->world[7];
        const float gl = Len3(gx, gy, 0.0f);
        if (gl > 1e-6f) {
            gx /= gl; gy /= gl;
            const float wv[3] = { meshO[0] * gx + meshO[1] * gy, meshO[4] * gx + meshO[5] * gy, meshO[8] * gx + meshO[9] * gy };
            const float wl = Len3(wv[0], wv[1], wv[2]);
            gateUp = wl > 1e-6f ? wv[1] / wl : -1.0f;
        }
    }
    // RG5: "on its side" at any update in the twist's window (the twist itself rolls the gun - its last update may be past 45 deg)
    const double nowS = static_cast<double>(nowQ) / 4000.0;
    if (gateUp >= kRvGateUpCos) { d.lastOnSideS = nowS; }
    in.onSide = d.lastOnSideS > 0.0 && nowS - d.lastOnSideS <= reload::kGateTwistWindowS + 0.05;
    // the gun's roll rate - LOGGED ONLY now (RG3 / RG4's source; its sign is the one Jay's opens confirmed - the twist's check)
    float roll = 0.0f;
    if (haveMesh) {
        if (d.havePrev && nowQ > d.prevQ) { roll = RvRollDps(d.prevMesh, meshW, static_cast<float>(nowQ - d.prevQ) / 4000.0f); }
        memcpy(d.prevMesh, meshW, sizeof(d.prevMesh));
        d.prevQ = nowQ;
        d.havePrev = true;
    }
    // [LOADBEARING: manual reload revolvers] RG5 (Jay: "The flick for the gate is a wrist rotation, very quick" -> "Controller twist"): THE
    // RIGHT CONTROLLER's twist about its grip axis (controllers.h GetControllerGripPose - the RAW pose, located once a frame; a repeat on
    // the second hook call is not a sample - twist_logic.h). COST: one seqlock pose read + a <= 64-sample push + the window's pair scan
    // (~18 samples at 120 Hz in 0.15 s -> ~150 quaternion products) per update while a gate revolver is held.
    XrPosef gp{};
    const bool haveCtl = GetControllerGripPose(kRightHand, gp);
    if (haveCtl) {
        const float q[4] = { gp.orientation.x, gp.orientation.y, gp.orientation.z, gp.orientation.w };
        d.twist.Push(q, nowS);
    }
    const reload::TwistPeak tp = d.twist.Peak(reload::kGateTwistWindowS);
    in.twistRightDeg = tp.rightDeg;
    in.twistLeftDeg = tp.leftDeg;
    reload::GParams p;
    const bool wasFresh = d.fresh;
    d.fresh = false;
    const bool openBefore = st->gm.open();
    const reload::GOut o = st->gm.Update(in, p);
    if (o.toggled) { d.twist.Clear(); }   // one twist never counts twice
    // a twist of 30 deg+ the way the gate would go that did NOT toggle it: once per twist, with why (RG5 had no record of the misses)
    {
        const float want = openBefore ? tp.leftDeg : tp.rightDeg;
        const float need = openBefore ? p.closeTwistDeg : p.openTwistDeg;
        if (!o.toggled && want >= 30.0f && !d.twistMissSaid && RvLine()) {
            d.twistMissSaid = true;
            // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=yes
            LOG("reload: [REVOLVER] GATE TWIST MISSED - a twist %s of %.0f deg in %.2f s (needs %.0f) with the gate %s: %s | the gun %.0f deg "
                "from gate-up, its roll %+.0f deg/s.", openBefore ? "LEFT" : "RIGHT", want, reload::kGateTwistWindowS, need,
                openBefore ? "open" : "shut", want < need ? "too small" : !in.onSide ? "the gun not on its side (gate within 130 deg of up)"
                : (!openBefore && !reload::GateCanLoad(in.rounds)) ? "the gun is full - 6 rounds (N4)"
                : "inside the 0.4 s lockout", std::acos(gateUp > 1.0f ? 1.0f : (gateUp < -1.0f ? -1.0f : gateUp)) * 180.0f / 3.14159265358979f,
                roll);
        }
        if (tp.rightDeg < 15.0f && tp.leftDeg < 15.0f) { d.twistMissSaid = false; }
    }
    if (!haveCtl && !d.twistNoCtlSaid && RvRefuseLine()) {
        d.twistNoCtlSaid = true;
        // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [REVOLVER] GATE - *** NO RIGHT CONTROLLER GRIP POSE *** (GetControllerGripPose false): the gate cannot be flicked.");
    }
    d.canPushPrev = o.gateOpen && !o.loading && o.hand > 0 && o.ch[o.cylinderIndex] == reload::DChamber::Empty;   // RG3: next snap test
    if (d.snap.snapped && o.loading) { d.snap.pushed = true; }
    int rAfter = rounds;
    bool called = true;
    if (o.callAddBullet) {
        called = RvAddOne(ctrl, st, false, &rAfter);   // ClipReload(false) on these (spec §8.1): the game's add is one round
        if (!called) { st->gm.AddFailed(o.addChamber); }
    }
    if (o.haptic && HolsterHaptic > 0.0f) {
        PulseController(o.Has(GEvent::GateOpen) || o.Has(GEvent::GateClose) ? kRightHand : kLeftHand, HolsterHaptic, kPulseMs);
    }
    for (int k = 0; k < o.nFx; ++k) {   // the click has no slot: silent
        RvPlayFx(o.fx[k] == reload::GFx::Open ? reload::Fx::MagGrab : o.fx[k] == reload::GFx::Seat ? reload::Fx::Seat
                 : o.fx[k] == reload::GFx::Close ? reload::Fx::RackDone : reload::Fx::None);
    }
    if (o.ev != GEvent::None && o.ev != GEvent::Shot && RvLine()) {   // RG3 fix: count EVENTS, not updates
        // [PROBE: REVOLVER] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [REVOLVER] GATE %s - gate %s, chamber at the gate %d, hand %d, rounds %d -> %d (spare %d)%s, the gun's roll %+.0f deg/s"
            " | controller twist right %.0f / left %.0f deg in %.2f s (open %.0f, close %.0f), the gate up %.0f deg from world up (on its "
            "side, within the window: %s).", o.Has(GEvent::GateOpen) ? "OPEN"
            : o.Has(GEvent::GateClose) ? "CLOSE" : o.Has(GEvent::Take) ? "TAKE"
            : o.Has(GEvent::TakeNoRound) ? "TAKE REFUSED" : o.Has(GEvent::LoadStart) ? "PUSH START" : o.Has(GEvent::RoundIn) ? "ROUND IN"
            : o.Has(GEvent::Index) ? "CLICK" : o.Has(GEvent::CaseOut) ? "CASE OUT" : o.Has(GEvent::HandCleared) ? "HAND CLEARED" : "EVENT",
            o.gateOpen ? "open" : "shut", o.cylinderIndex, o.hand, rounds, rAfter, st->spare, !called ? " *** the add-bullet FAULTED ***" : "",
            roll, tp.rightDeg, tp.leftDeg, reload::kGateTwistWindowS, p.openTwistDeg, p.closeTwistDeg,
            std::acos(gateUp > 1.0f ? 1.0f : (gateUp < -1.0f ? -1.0f : gateUp)) * 180.0f / 3.14159265358979f, in.onSide ? "yes" : "no");
    }
    // DRAW: the gate (+90 about its own z = the top swings out to the right, spec §8.4), the cylinder (60 deg a chamber - its direction
    // is my call), the round on the push / in the hand / at rest
    float rz[12];
    if (o.gateOpen || d.gateOff) {
        RvRotZ(o.gateOpen ? 90.0f : 0.0f, rz);
        if (RvDrawPart(model, d.gate, rz)) { d.gateOff = o.gateOpen; }
    }
    if (o.cylinderIndex != 0 || d.cylOff) {
        RvRotZ(60.0f * static_cast<float>(o.cylinderIndex), rz);
        if (RvDrawPart(model, d.cyl, rz)) { d.cylOff = o.cylinderIndex != 0; }
    }
    const bool loading = o.loading;
    const reload::DbFrame& f = reload::DbFrameAt(la, static_cast<int>(o.loadFrame));
    float w[12];
    if (loading && haveMesh) {
        Mul34(meshW, f.shellM[0], w);
        if (SetWorld(model, d.round.id, w)) { d.roundOff = true; }
    } else if (o.hand > 0 && haveHand) {
        Mul34(lh, reload::DbFrameAt(hold, 1).shellH[0], w);   // RG6: the round in Jay's frame-1 pinch
        if (SetWorld(model, d.round.id, w)) { d.roundOff = true; }
    } else if (d.roundOff && RvDrawPart(model, d.round, nullptr)) {
        d.roundOff = false;
    }
    bool fan = false;
    const bool cockR2 = RvCock(model, st, o.Has(GEvent::ReloadDone), wasFresh, rAfter, havePalm, palmM, haveMesh ? meshW : nullptr, &fan);
    float handM[12];
    // RG3: locked while the push plays AND while snapped (the shotgun's lock): before the push frame 1, after it its last frame
    const reload::DbFrame& lf = loading ? f : reload::DbFrameAt(la, d.snap.pushed ? 99 : 1);
    const bool lock = (loading || d.snap.snapped) && haveMesh && (memcpy(handM, lf.hand, sizeof(handM)), true);
    // RG6 (Jay: "the initial hand pose, with the shell inbetween the thumb & index finger isn't happening" -> "Yes, frame 1 pinch"): the
    // push frames only while it plays / the hand is snapped; holding rounds before that = the HOLD pose (his scene's frame 1)
    const reload::DbAnim an = loading || d.snap.snapped ? la : o.hand > 0 ? hold
                            : fan ? (walker ? reload::DbAnim::FanWalker : reload::DbAnim::FanSix)
                                                          : reload::DbAnim::None;
    RvPublishHand(lock, meshO, lock ? handM : nullptr, an, loading ? o.loadFrame : 1.0f, !loading, now);
    if (wasFresh && RvLine()) {
        // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [REVOLVER] GATE FIRST UPDATE (%s) - rounds %d, mesh frame %s, hand %s.", walker ? "Walker" : "Sixshooter", rounds,
            haveMesh ? "read" : "*** NOT READ ***", haveHand ? "read" : "not read");
    }
    Publish(o.withholdR2 || cockR2, o.gateOpen);
    return o.gateOpen ? "the gate open" : "the gate shut";
}

// One update of the held chainsaw gun (ReloadTick). COST: one rounds call, two world reads, the step machine, <= 2 part writes, a publish.
const char* DriveFuel(uintptr_t ctrl, ItemState* st, void* playerModel, int lHandId, const float* leftPos, const float* leftR9,
                      bool leftAchieved, const float* leftCmdPos, ULONGLONG now)
{
    using reload::FEvent;
    RvDraw& d = g_rv;
    const uintptr_t model = g_gun.model;
    const long long nowQ = PumpClockQ();
    float meshW[12], meshO[12], lh[12];
    const bool haveMesh = RvMeshWorld(model, meshW) && Ortho34(meshW, meshO);
    const bool haveHand = playerModel && lHandId >= 0 && ElemMtx(g_world, reinterpret_cast<uintptr_t>(playerModel), lHandId, lh);
    reload::FInput in{};
    in.manualOn = ManualReload;
    in.isChainsaw = true;
    in.freshDraw = d.fresh;
    in.gripDown = g_gripDown != 0;
    in.inHip = HolsterLeftPouchHeld();
    int rounds = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    in.rounds = rounds;
    in.capacity = 100;   // AmmoCount(100) - OUR Data0.pak inventory_gen_dlc66.scr (spec §8.1)
    const bool takeNow = in.gripDown && in.inHip;
    if (takeNow && !d.takeWas) { st->spare = SpareOf(ctrl, st->item); }
    d.takeWas = takeNow;
    in.spare = st->spare;
    const double nowS = static_cast<double>(nowQ) / 4000.0;
    static double s_lastS = 0.0;
    in.dtS = !d.fresh && s_lastS > 0.0 && nowS > s_lastS ? static_cast<float>(nowS - s_lastS) : 0.0f;   // never across a put-away
    s_lastS = nowS;
    float palmM[3] = {};
    const bool havePalm = haveMesh && RvPalmMesh(meshW, leftPos, leftR9, leftAchieved, leftCmdPos, palmM);
    // the bottle in the hand: its world = the hand x Jay's hold (kBottle shellH); the spout tip + the bottle's up from its REST frame
    float bottleW[12] = {};
    const bool bottleIn = st->fm.bottle() && haveHand;
    if (bottleIn) { Mul34(lh, reload::kBottle[0].shellH[0], bottleW); }
    if (bottleIn && haveMesh) {
        // the spout tip (OUR mesh: vertex 18963 at rest (-0.000071 0.06622 0.239544) = the vertex the game's pour holds at the filler,
        // backups/20261109_gl_chainsaw_reload_renders/work/cs_bottle_f72.obj vs chainsaw_a_bone_clip.obj, 0.0 m) and the bottle's up
        // (mesh +y at rest), both in
        // the bottle's own frame - from its REST world (the mesh file)
        float ri[12], tipL[3], tw[3];
        RigidInv(d.clip.rest->world, ri);
        XformPt(ri, reload::Vec3{ -0.0001f, 0.0662f, 0.2395f }, tipL);
        XformPt(bottleW, reload::Vec3{ tipL[0], tipL[1], tipL[2] }, tw);
        float fw[3];
        XformPt(meshW, reload::Vec3{ kSawFiller[0], kSawFiller[1], kSawFiller[2] }, fw);
        in.spoutAtFiller = Len3(tw[0] - fw[0], tw[1] - fw[1], tw[2] - fw[2]) <= kSawFillerBallM;
        // RG3 (Jay: "need a haptic buzz when the fuel is in the right spot"): the pour spot's ENTER pulse
        if (in.spoutAtFiller && !d.spoutWas && HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
        const float upL[3] = { ri[1], ri[5], ri[9] };   // mesh +y in the bottle's rest frame (R^T column)
        float up[3];
        for (int r = 0; r < 3; ++r) { up[r] = bottleW[4 * r] * upL[0] + bottleW[4 * r + 1] * upL[1] + bottleW[4 * r + 2] * upL[2]; }
        const float l = Len3(up[0], up[1], up[2]);   // from world +Y (world up = Y: STATE U-52)
        in.tiltDeg = l > 1e-6f ? std::acos(up[1] / l > 1.0f ? 1.0f : (up[1] / l < -1.0f ? -1.0f : up[1] / l)) * 180.0f / 3.14159265358979f : 0.0f;
    }
    d.spoutWas = in.spoutAtFiller;
    // the cord: the T-handle at rest (world); a grip within reload_grab_m of it grabs it; the pull = the handle in the hand from here
    float hRest[3] = {};
    if (haveMesh) {
        XformPt(meshW, reload::Vec3{ d.handle.rest->world[3], d.handle.rest->world[7], d.handle.rest->world[11] }, hRest);
    }
    float palmW[3] = {};
    if (havePalm) { XformPt(meshO, reload::Vec3{ palmM[0], palmM[1], palmM[2] }, palmW); }   // back through the frame it came from
    in.nearCord = havePalm && haveMesh && Len3(palmW[0] - hRest[0], palmW[1] - hRest[1], palmW[2] - hRest[2]) <= ReloadGrabM;
    const bool cordEdge = in.gripDown && in.nearCord && !d.gripWas;
    d.gripWas = in.gripDown;
    if (cordEdge) { memcpy(d.cordGrab, palmW, sizeof(d.cordGrab)); }
    // RG3 (Jay: "Hand should be locked to the pullstring for the chainsaw" -> "I want the hand to be locked to the cord whilst holding
    // grip, but I still want it to have free direction to pull, not restricted"): while the cord is held the T-handle is drawn IN THE HAND
    // (l_hand x Jay's hold, kCord's shellH) - the hand stays free; the pull = how far the handle in the hand is from its rest spot, in ANY
    // direction (world).
    float handleW[12] = {};
    const bool handleInHand = haveHand && (d.cordHeld || cordEdge) && in.gripDown && !st->fm.bottle();
    if (handleInHand) { Mul34(lh, reload::kCord[0].shellH[0], handleW); }
    const float cordD = handleInHand && haveMesh ? Len3(handleW[3] - hRest[0], handleW[7] - hRest[1], handleW[11] - hRest[2]) : 0.0f;
    if (cordEdge) { d.cordD0 = cordD; }   // the grab's own offset (up to reload_grab_m) is not pull
    in.cordPullM = cordD > d.cordD0 ? cordD - d.cordD0 : 0.0f;
    const bool wasFresh = d.fresh;
    d.fresh = false;
    const reload::FOut o = st->fm.Update(in);
    d.cordHeld = o.cordHeld;
    int rAfter = rounds;
    bool called = true;
    if (o.callAddBullet) { called = RvAddOne(ctrl, st, false, &rAfter); }   // ClipReload(true): the game's add fills the 100 (R4)
    if (o.haptic && HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
    RvPlayFx(o.fx);
    if (o.ev != FEvent::None && RvLine()) {   // RG3 fix: count EVENTS, not updates
        // [PROBE: CHAINSAW] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [CHAINSAW] %s - rounds %d -> %d (spare %d)%s | bottle %s, tilt %.0f deg, spout at the filler %d, pour %.2f s | cord %s, "
            "pull %.3f m | R2 %s.", o.Has(FEvent::Take) ? "TAKE" : o.Has(FEvent::TakeNone) ? "TAKE REFUSED" : o.Has(FEvent::Filled) ? "FILLED"
            : o.Has(FEvent::BottleAway) ? "BOTTLE AWAY" : o.Has(FEvent::CordGrab) ? "CORD GRAB" : o.Has(FEvent::CordPulled) ? "CORD PULLED"
            : o.Has(FEvent::CordShort) ? "CORD SHORT" : "EVENT", rounds, rAfter, st->spare, !called ? " *** the add-bullet FAULTED ***" : "",
            o.bottleInHand ? "in the hand" : "away", in.tiltDeg, in.spoutAtFiller ? 1 : 0, o.pourS, o.cordHeld ? "held" : "free", in.cordPullM,
            o.withholdR2 ? "withheld" : "free");
    }
    // DRAW: the bottle at the hand / back at rest; the T-handle out along the cord while held / back at rest
    if (o.bottleInHand && haveHand) {
        float bw[12];
        Mul34(lh, reload::kBottle[0].shellH[0], bw);
        if (SetWorld(model, d.clip.id, bw)) { d.clipOff = true; }
    } else if (d.clipOff && RvDrawPart(model, d.clip, nullptr)) {
        d.clipOff = false;
    }
    // the T-handle in the hand while the cord is held (Jay: free direction); back at its rest once let go
    if (o.cordHeld && handleInHand) {
        if (SetWorld(model, d.handle.id, handleW)) { d.handleOff = true; }
    } else if (d.handleOff && RvDrawPart(model, d.handle, nullptr)) {
        d.handleOff = false;
    }
    const reload::DbAnim an = o.bottleInHand ? reload::DbAnim::Bottle : o.cordHeld ? reload::DbAnim::Cord : reload::DbAnim::None;
    RvPublishHand(false, nullptr, nullptr, an, 1.0f, true, now);
    if (wasFresh && RvLine()) {
        // [PROBE: CHAINSAW] gate=manual_reload  site=FLAT  log-only=yes
        LOG("reload: [CHAINSAW] FIRST UPDATE - rounds %d (ready %d), mesh frame %s, hand %s.", rounds, o.ready ? 1 : 0,
            haveMesh ? "read" : "*** NOT READ ***", haveHand ? "read" : "not read");
    }
    Publish(o.withholdR2, o.bottleInHand || o.cordHeld || in.nearCord);   // the cord grab's press is the reload's (not loot / push-grab)
    return o.bottleInHand ? "the bottle in the hand" : o.cordHeld ? "the cord out" : "at rest";
}

// The held model changed: classify it once.
void ClassifyModel(uintptr_t model, uintptr_t player, uintptr_t ctrl, uintptr_t ctrl2)
{
    g_gun = Gun{};
    g_gun.model = model;
    int slot = -1, slot2 = -1;
    ReadInt(ctrl + kCtlSlotOff, &slot);
    if (ctrl2) { ReadInt(ctrl2 + kCtlSlotOff, &slot2); }
    uintptr_t item = 0, rawType = 0;
    char prefix[64] = {};
    bool haveItem = slot >= 0 && CallV1(player, kSelItemVfn, static_cast<unsigned>(slot), &item) && item;
    bool haveType = haveItem && CallV0(item + kItemDescOff, kDescTypeVfn, &rawType);
    bool havePrefix = haveItem && ReadPrefix(item + kItemDescOff, prefix, sizeof(prefix));
    const unsigned type = static_cast<unsigned>(rawType);
    // The mesh name: once per model change (ItemMeshName = the guarded RhMeshName read [TWOHAND] uses - INV-500 named
    // wn_submachine_a with it in play). The SMG is classified by it; the pistol is classified without it, as RUN RL2C verified, but
    // its left-hand poses are per mesh (spec section 9 call 4) - a pistol mesh with no row keeps today's hand.
    char mesh[96] = {};
    const bool haveMesh = ItemMeshName(model, mesh, sizeof(mesh));
    // Rifle plan Task 5: the mag bone is the ROW's (rifles: bone_clip - they have no bone_magazine), so the row is looked up FIRST:
    // a classification that assumes both bones exist names the row (or none); its magBone is then resolved on the model and the
    // real classification below tests it. No row (the pistol, an unknown mesh) -> bone_magazine. Once per model change (two table
    // walks of ~20 rows + two element lookups by name), never per update.
    const bool canClassify = haveItem && haveType && havePrefix;
    const reload::Classified pre = canClassify
        ? reload::Classify(type, prefix, haveMesh ? mesh : nullptr, true, true)
        : reload::Classified{ reload::Family::None, nullptr, nullptr };
    const char* magBone = (pre.row && pre.row->magBone && pre.row->magBone[0]) ? pre.row->magBone : "bone_magazine";
    int magId = -1, chamberId = -1;
    ElemId(model, magBone, &magId);
    ElemId(model, "bone_chamber", &chamberId);
    // Case-INSENSITIVE prefixes (the scripts write "Beretta" / "Submachinegun", the game returns lower case - RUN RL0).
    reload::Classified c = canClassify
        ? reload::Classify(type, prefix, haveMesh ? mesh : nullptr, magId >= 0, chamberId >= 0)
        : reload::Classified{ reload::Family::None, nullptr, nullptr };
    // THE CROSSBOW (crossbow plan Task 5, carry ruling T2-d): Classify cannot see its bones (it answers the row with them assumed), so
    // type 37 is classified again with the three REAL element ids. Once per model change (three lookups by name).
    int xStringId = -1, xRackId = -1, xBoltId = -1;
    if (canClassify && type == reload::kItemTypeCrossbow) {
        ElemId(model, "bone_string", &xStringId);
        ElemId(model, "bone_stringreload", &xRackId);
        ElemId(model, "bolt_pose", &xBoltId);
        c = reload::ClassifyCrossbow(prefix, haveMesh ? mesh : nullptr, xStringId >= 0, xRackId >= 0, xBoltId >= 0);
    }
    // THE PUMP SHOTGUN (pump plan Task 4): Classify answers its row with the two elements assumed, so type 24 is classified again with
    // the REAL ids, resolved by NAME (Task 0: the static node order is never assumed). Once per model change (two lookups by name).
    int pPumpId = -1, pAmmoId = -1, pAmmoBId = -1;
    // 2026-11-09 (plan 2026-11-09-manual-reload-remaining-guns Task 2): the grenade launcher is type 24 too, but its drum is a rifle mag
    // (Classify routed it to ClassifyRifle above) - it is NOT re-classified as a shotgun. One case-blind compare, once per model change.
    const bool isLauncher = havePrefix && reload::guns_detail::EqNoCase(prefix, "grenadelauncher");
    if (canClassify && type == reload::kItemTypeShotgun && !isLauncher) {
        ElemId(model, "bone_reload_shotgun", &pPumpId);
        ElemId(model, "ammo_bullet", &pAmmoId);
        ElemId(model, "ammo_bullet_b", &pAmmoBId);   // the double-barrel's second shell (INV-700); absent on the pumps - not read there
        // 2026-11-07: ClassifyShotgun = the double-barrel wn_shotgun_a (ClassifyDBarrel), else the tube-loaded rows (ClassifyPump)
        c = reload::ClassifyShotgun(prefix, haveMesh ? mesh : nullptr, pPumpId >= 0, pAmmoId >= 0, pAmmoBId >= 0);
    }
    // THE SINGLE-SHOT (2026-11-09, plan 2026-11-09-manual-reload-remaining-guns Task 8a): Classify answered the break-open family with its
    // two elements assumed - re-classified with the REAL ids, by NAME (its hinge, its carried round; no second shell). Once per model
    // change (two lookups by name).
    const bool isSingle = havePrefix && reload::guns_detail::EqNoCase(prefix, "singleshot");
    if (canClassify && type == reload::kItemTypePistol && isSingle) {
        ElemId(model, "bone_reload_schofield", &pPumpId);
        ElemId(model, "bullet_pistol_vancrane", &pAmmoId);
        pAmmoBId = -1;
        c = reload::ClassifySingleShot(prefix, haveMesh ? mesh : nullptr, pPumpId >= 0, pAmmoId >= 0);
    }
    const char* why = !haveItem ? "the firearm controller (+3096) has no selected item"
                    : !haveType ? "the item type read faulted"
                    : !havePrefix ? "the anim prefix read faulted"
                    : c.family == reload::Family::None ? c.why : nullptr;
    if (why) {
        if (EventLine()) {
            // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
            // Rifle plan Task 5: names the mag bone looked up (the row's - bone_clip on a rifle - else bone_magazine) and the family
            // its row says (None = no row), with the mesh - an unknown rifle mesh (AI rifle, DLC skin) is one line naming it.
            LOG("reload: [RELOAD] REFUSED model %p: %s (type %d, prefix '%s', mesh '%s', row family %s, mag bone %s %d, bone_chamber %d; "
                "ctl +3096 slot %d item %p, +3104 slot %d) - not a built family, the game reloads it as today.",
                reinterpret_cast<void*>(model), why, haveType ? static_cast<int>(type) : -1, prefix, haveMesh ? mesh : "(unreadable)",
                FamilyName(pre.family), magBone, magId, chamberId, slot, reinterpret_cast<void*>(item), slot2);
        }
        // [LOADBEARING: manual reload start refusal] B3 (audit S11 11a-1, Jay 2026-11-15 "Yes"; trace 20261115_1330_b3_refused_gun_forget):
        // EVERY refused item is dropped from the manual list, whatever its type - a refusal says "the game reloads it", and the list
        // must agree. Before: only a crossbow / shotgun / single-shot / revolver / chainsaw was forgotten, so a pistol / SMG / rifle (family
        // None) or a faulted type / prefix read left an earlier registration of the same item address in place -> StartHook and
        // AddBulletHook refused the game's reload AND the mod did not drive it. COST: kMaxItems compares, once per model change.
        if (haveItem) { ForgetManualItem(item); }
        return;
    }
    // THE PUMP: its own classification (the row, the elements by name and their parents, the fx row) and its own driver (ReloadTick ->
    // DrivePump) - never the gun Machine below. Refused -> the game reloads it. A shell WE left at the hand on this model goes home.
    if (c.family == reload::Family::Pump) {
        char meshN[96] = {};
        const bool haveNorm = haveMesh && reload::guns_detail::Norm(mesh, meshN, sizeof(meshN));
        const bool okP = ClassifyPumpDraw(model, item, c, haveNorm ? meshN : nullptr, haveMesh ? mesh : "(unreadable)", prefix, pPumpId,
                                          pAmmoId, GetTickCount64());
        // Ruling R3 / fix round 1 (M1): this model's own record, if it has one. A RESUMED draw (M2) keeps drawing its shell - no restore.
        if (!okP || g_pd.fresh) { PumpRestoreShell(model, okP ? "the pump was drawn again" : "the pump was refused this draw"); }
        if (!okP) {
            ForgetManualItem(item);
            return;
        }
        g_gun.built = true;
        g_gun.family = c.family;
        g_gun.item = item;
        g_gun.fx = haveNorm ? reload::FindFxRow(meshN) : nullptr;   // the seat / rack / move sounds (reload_fx_table.inc), once per draw
        StateFor(item, GetTickCount64());   // registers the item for the START / ADD-BULLET refusals (IsManualItem)
        return;
    }
    // THE DOUBLE-BARREL (2026-11-07): its own classification and driver (ReloadTick -> DriveDBarrel). Refused -> the game reloads it.
    if (c.family == reload::Family::DBarrel) {
        char meshN[96] = {};
        const bool haveNorm = haveMesh && reload::guns_detail::Norm(mesh, meshN, sizeof(meshN));
        if (!ClassifyDbDraw(model, item, ctrl, haveNorm ? meshN : nullptr, haveMesh ? mesh : "(unreadable)", prefix, pPumpId, pAmmoId,
                            pAmmoBId)) {
            ForgetManualItem(item);
            return;
        }
        g_gun.built = true;
        g_gun.family = c.family;
        g_gun.item = item;
        g_gun.fx = haveNorm ? reload::FindFxRow(meshN) : nullptr;   // the open / load / close sounds (reload_fx_table.inc), once per draw
        StateFor(item, GetTickCount64());   // registers the item for the START / ADD-BULLET refusals (IsManualItem)
        return;
    }
    // THE REVOLVERS + THE CHAINSAW (2026-11-09, Task 8b-e): their own classification (every part by NAME + its rest row) and drivers
    // (ReloadTick -> DriveBreak6 / DriveGate / DriveFuel). Refused -> the game reloads it. Once per model change.
    if (c.family == reload::Family::Break6 || c.family == reload::Family::Gate || c.family == reload::Family::Fuel) {
        char meshN[96] = {};
        const bool haveNorm = haveMesh && reload::guns_detail::Norm(mesh, meshN, sizeof(meshN));
        if (!ClassifyRvDraw(model, item, c.family, haveNorm ? meshN : nullptr, haveMesh ? mesh : "(unreadable)", prefix)) {
            ForgetManualItem(item);
            return;
        }
        g_gun.built = true;
        g_gun.family = c.family;
        g_gun.item = item;
        g_gun.fx = haveNorm ? reload::FindFxRow(meshN) : nullptr;   // the gun's sounds (reload_fx_table.inc), once per draw
        StateFor(item, GetTickCount64());   // registers the item for the START / ADD-BULLET refusals (IsManualItem)
        return;
    }
    // THE CROSSBOW: its own classification (the row, bone_root, the parts' parents, the hand rows, the bolt object, the detour) and
    // its own driver (ReloadTick -> DriveXbow) - never the gun Machine below (carry ruling T2-d). Refused -> the game reloads it.
    if (c.family == reload::Family::Crossbow) {
        char meshN[96] = {};
        const bool haveNorm = haveMesh && reload::guns_detail::Norm(mesh, meshN, sizeof(meshN));
        if (!ClassifyXbow(model, item, c, haveNorm ? meshN : nullptr, haveMesh ? mesh : "(unreadable)", prefix, xStringId, xRackId,
                          xBoltId)) {
            ForgetManualItem(item);
            // A bolt still on the hand from this same crossbow (a blip with no held model) goes home - it cannot be driven this draw.
            if (g_xbCarry.active && reinterpret_cast<void*>(model) == g_xbCarry.homeModel) {
                HandCarryGiveBack(g_xbCarry, "the crossbow was refused this draw");
            }
            XbowRestoreBolt(model, "the crossbow was refused this draw");   // ruling T5-b: the game will reload and fire it
            return;
        }
        g_gun.built = true;
        g_gun.family = c.family;
        g_gun.item = item;
        g_gun.fx = haveNorm ? reload::FindFxRow(meshN) : nullptr;   // the catch / seat sounds (reload_fx_table.inc), once per draw
        StateFor(item, GetTickCount64());   // registers the item for the START / ADD-BULLET refusals (IsManualItem)
        return;
    }
    g_gun.built = true;
    g_gun.family = c.family;
    g_gun.row = c.row;
    {
        char meshN[96] = {};
        const bool haveNorm = haveMesh && reload::guns_detail::Norm(mesh, meshN, sizeof(meshN));
        g_gun.rackRow = reload::RackRowFor(c.family, haveNorm ? meshN : nullptr, prefix);
        // The Commando's +x handle pose (the gun's LEFT, spec §4): only a row with a second handle has one. Once per classification.
        g_gun.rackRow2 = (c.row && c.row->hasRack2) ? reload::RackRowFor(c.family, haveNorm ? meshN : nullptr, prefix, true) : nullptr;
        g_gun.magRow = reload::MagRowFor(c.family, haveNorm ? meshN : nullptr);
        g_gun.pullRow = reload::PullRowFor(c.family, haveNorm ? meshN : nullptr);
        g_gun.fx = haveNorm ? reload::FindFxRow(meshN) : nullptr;   // once per classification (a table walk of ~11 rows)
        // The well rail: once per classification (a table walk of gunshape::kGunShapeRowCount rows - 30 today - by name; never per frame).
        g_gun.clearM = 0.0f;
        g_gun.haveClear = haveMesh && GunShapeMagClearM(mesh, &g_gun.clearM);
    }
    g_gun.item = item;
    g_gun.magBone = magBone;   // a string literal or a row's (static table) - never dangles
    g_gun.magId = magId;
    g_gun.chamberId = chamberId;
    ElemParent(model, magId, &g_gun.magParent);
    ElemParent(model, chamberId, &g_gun.chamberParent);
    StateFor(item, GetTickCount64());   // registers the item for the two detours from now on
    if (EventLine()) {
        // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
        const reload::GunRow* r = g_gun.row;
        // Rifle plan Task 5: the family by name (Rifle included), the mag bone by name, the Commando's second handle + its row.
        // Final fix wave: the second handle is the +x one (spec §4: the gun's LEFT); the row's mag-out / rack axes (parents' frames).
        LOG("reload: [RELOAD] GUN model %p family %s (type %u, prefix '%s', mesh '%s') mag %s %d (parent %d) chamber %d (parent %d) | "
            "grab spots %s mag (%+.4f %+.4f %+.4f) rack (%+.4f %+.4f %+.4f) +x handle (gun's left) %s (%+.4f %+.4f %+.4f) bone-local; "
            "axes in the parents' frames: mag out (%+.4f %+.4f %+.4f) rack back (%+.4f %+.4f %+.4f)%s; bolt "
            "travel %.4f m, rack done at %.4f m, rack grab radius %.3f m (mag grab %.3f m) | ctl "
            "+3096 %p slot %d item %p, +3104 %p slot %d (G6: +3096 should be the RIGHT hand's) | left hand: rack row %s (+x "
            "handle %s), mag row %s, pull row %s | well rail: %s %.4f m.",
            reinterpret_cast<void*>(model),
            FamilyName(g_gun.family), type, prefix, haveMesh ? mesh : "(unreadable)", magBone, magId,
            g_gun.magParent, chamberId, g_gun.chamberParent, r ? "(Jay's)" : "(the joints)", r ? r->magSpot.x : 0.0f,
            r ? r->magSpot.y : 0.0f, r ? r->magSpot.z : 0.0f, r ? r->rackSpot.x : 0.0f, r ? r->rackSpot.y : 0.0f,
            r ? r->rackSpot.z : 0.0f, (r && r->hasRack2) ? "YES" : "none", (r && r->hasRack2) ? r->rackSpot2.x : 0.0f,
            (r && r->hasRack2) ? r->rackSpot2.y : 0.0f, (r && r->hasRack2) ? r->rackSpot2.z : 0.0f,
            r ? r->magOutAxis.x : 0.0f, r ? r->magOutAxis.y : 0.0f, r ? r->magOutAxis.z : 1.0f, r ? r->rackAxis.x : 1.0f,
            r ? r->rackAxis.y : 0.0f, r ? r->rackAxis.z : 0.0f, r ? " (the row's)" : " (no row: the parents' +Z / +X)",
            r ? r->boltTravelM : 0.0368f, reload::RackPullFor(g_gun.family, r, ReloadRackPullM),
            reload::RackGrabFor(g_gun.family, ReloadGrabM) + reload::kGrabWidenM, ReloadGrabM + reload::kGrabWidenM,   // as the grabs use them
            reinterpret_cast<void*>(ctrl), slot, reinterpret_cast<void*>(item), reinterpret_cast<void*>(ctrl2), slot2,
            g_gun.rackRow ? g_gun.rackRow->key : "none - today's hand", g_gun.rackRow2 ? g_gun.rackRow2->key : "none",
            g_gun.magRow ? g_gun.magRow->key : "none - today's hand",
            g_gun.pullRow ? g_gun.pullRow->key : "none - the mag row pulls",
            g_gun.haveClear ? "ON, the mag clears the well at" : "NONE (no well row for this mesh - the mag is free from the grab)",
            g_gun.clearM);
        // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes - the gun's fx row (solid guns plan Task 3): racking + sounds
        const reload::FxRow* fx = g_gun.fx;
        if (fx) {
            LOG("reload: [RELOAD] FX mesh '%s': row %s (prefix %s), racks %s, sounds mag out %d / seat %d / rack %d (first: %s / %s / %s).",
                haveMesh ? mesh : "(unreadable)", fx->mesh, fx->prefix, fx->racks ? "yes" : "no", fx->nMagOut, fx->nSeat, fx->nRack,
                fx->nMagOut ? fx->magOut[0].name : "-", fx->nSeat ? fx->seat[0].name : "-", fx->nRack ? fx->rack[0].name : "-");
        } else {
            LOG("reload: [RELOAD] FX mesh '%s': NO FX ROW (not in reload_fx_table.inc) - this gun does NOT rack and plays no reload "
                "sounds.", haveMesh ? mesh : "(unreadable)");
        }
    }
    // The element list, once per pistol model (G5: which element carries the magazine's mesh - names + hierarchy, structural).
    if (g_elemModels < kMaxElemModels) {
        ++g_elemModels;
        int n = 0;
        const bool okN = ElemCount(model, &n);
        if (g_nElem < kMaxElemLines) {
            // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [RELOAD] ELEMENTS model %p: %d element(s)%s (flags = GetElementFlags raw - meaning not established).",
                reinterpret_cast<void*>(model), okN ? n : -1, okN ? "" : " - GetElementsNumber FAULTED");
            ++g_nElem;
        }
        const int shown = (okN && n > 0) ? (n < kMaxElemsPerModel ? n : kMaxElemsPerModel) : 0;
        for (int i = 0; i < shown && g_nElem < kMaxElemLines; ++i, ++g_nElem) {
            const char* nm = nullptr;
            int parent = -1, bone = 0, hidden = 0;
            unsigned flags = 0;
            if (ElemFacts(model, i, &nm, &parent, &bone, &flags, &hidden)) {
                // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
                LOG("reload: [RELOAD] ELEMENT %d '%s' parent %d bone %d flags 0x%X hidden %d", i, nm ? nm : "(null)", parent, bone,
                    flags, hidden);
            } else {
                // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
                LOG("reload: [RELOAD] ELEMENT %d - a read FAULTED", i);
            }
        }
        if (okN && n > kMaxElemsPerModel && g_nElem < kMaxElemLines) {
            // [PROBE: RELOAD] gate=manual_reload  site=FLAT  log-only=yes
            LOG("reload: [RELOAD] ELEMENTS model %p: %d more not shown (cap %d).", reinterpret_cast<void*>(model),
                n - kMaxElemsPerModel, kMaxElemsPerModel);
            ++g_nElem;
        }
    }
}

// The rest latch (INV-4: read before any write - ours or the animation's): the game not reloading (+600 == -1) and both parts'
// locals unchanged for kStillMs. The MAG's rest latches with any round count (it does not move while the clip is 0 - G7: only
// bone_chamber animates in shootempty) and is kept per ITEM, so a re-drawn empty pistol mid-reload still has its seat; the
// CHAMBER's (logged only) needs rounds > 0 (an empty clip draws the slide LOCKED back). Only while the machine is Idle (no write).
void TryLatch(uintptr_t ctrl, ItemState* st, ULONGLONG now)
{
    float lm[12], lc[12] = {};
    // Rifle plan Task 5: a gun with no bone_chamber (the Railgun - its row has no rack handle; classified without it) latches its
    // mag's rest alone: the chamber's local is then a constant zero, so "still" is the mag's alone. COST: unchanged (one read fewer).
    const bool noChamber = g_gun.chamberId < 0;
    if (!ElemMtx(g_local, g_gun.model, g_gun.magId, lm) ||
        (!noChamber && !ElemMtx(g_local, g_gun.model, g_gun.chamberId, lc))) {
        g_gun.stillSince = 0;
        return;
    }
    int idx = 0, rounds = 0;
    const bool idle = ReadInt(ctrl + kCtlReloadIdxOff, &idx) && idx == -1 && CallCtlInt(g_rounds, ctrl, &rounds);
    const bool still = g_gun.haveLast && SameMtx(lm, g_gun.lastMag) && SameMtx(lc, g_gun.lastChamber);
    memcpy(g_gun.lastMag, lm, sizeof(lm));
    memcpy(g_gun.lastChamber, lc, sizeof(lc));
    g_gun.haveLast = true;
    if (!idle || !still) { g_gun.stillSince = 0; return; }
    if (g_gun.stillSince == 0) { g_gun.stillSince = now; return; }
    if (now - g_gun.stillSince < kStillMs) { return; }
    const bool newMag = !st->haveMagRest;
    if (newMag) {
        memcpy(st->magRest, lm, sizeof(lm));
        st->haveMagRest = true;
    }
    if (rounds > 0 && !noChamber) {
        memcpy(g_gun.restChamber, lc, sizeof(lc));
        g_gun.haveRest = true;
    }
    if ((newMag || g_gun.haveRest) && EventLine()) {
        // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("reload: [RELOAD] REST LATCHED model %p item %p (not reloading, %d round(s), parts still %llu ms): %s local t "
            "(%+.4f %+.4f %+.4f)%s, bone_chamber local t (%+.4f %+.4f %+.4f)%s - the clip reads chamber x %.4f at rest.",
            reinterpret_cast<void*>(g_gun.model), reinterpret_cast<void*>(st->item), rounds,
            static_cast<unsigned long long>(kStillMs), g_gun.magBone, st->magRest[3], st->magRest[7], st->magRest[11],
            newMag ? "" : " (kept for this item)", lc[3], lc[7], lc[11],
            noChamber ? " (NO bone_chamber on this gun - none latched)"
                      : rounds > 0 ? "" : " (NOT latched: 0 rounds = locked back)",
            kChamberClipRest);
    }
    if (rounds > 0 || st->haveMagRest) { g_gun.stillSince = 0; }
}

// 2026-11-08 log trim (Jay: timer-driven summaries out of BETA, on demand in DEV): the STATE line is a LOG_WINDOW - the gun's
// FIRST one always prints, the timer repeats only with diag_periodic_logs (never in BETA). Set by the caller.
bool g_stateLineFirst = false;
void StateLine(uintptr_t player, uintptr_t ctrl, const ItemState* st, const float* hand, bool achieved, const char* drew,
               const float* cmdPos, ULONGLONG now)
{
    // The left hand's poses (plan 2026-11-01-reload-hand-poses Task 4 READ AFTER: the lock's t and position). lockGap = the locked
    // hand BONE from the controller's grip point (it includes the authored ~0.10 m hand -> handholder offset; it is NOT an error).
    const float lockT = ClockT(g_lockClock, now), fingerT = ClockT(g_fingerClock, now);
    float lockPos[3] = {}, lockR[9] = {};
    const bool haveLock = lockT > 0.0f && g_lockRow && g_havePart && reload::LockedHand(g_partLast, *g_lockRow, lockPos, lockR);
    const float lockGap = (haveLock && cmdPos)
        ? Len3(lockPos[0] - cmdPos[0], lockPos[1] - cmdPos[1], lockPos[2] - cmdPos[2]) : -1.0f;
    int rounds = -1, maxR = -1, spare = -1, idx = -2, slot = -1;
    const bool okR = CallCtlInt(g_rounds, ctrl, &rounds);
    const bool okM = CallCtlInt(g_max, ctrl, &maxR);
    ReadInt(ctrl + kCtlReloadIdxOff, &idx);
    uintptr_t item = 0, ctlPlayer = 0;
    bool okS = false;
    if (ReadInt(ctrl + kCtlSlotOff, &slot) && slot >= 0 && CallV0(ctrl, kCtlPlayerVfn, &ctlPlayer) && ctlPlayer &&
        CallV1(ctlPlayer, kSelItemVfn, static_cast<unsigned>(slot), &item) && item) {
        okS = CallSpare(ctlPlayer, item, &spare);
    }
    float mw[12], pw[12], cl[12];
    const bool haveMw = ElemMtx(g_world, g_gun.model, g_gun.magId, mw);
    const bool havePw = g_gun.magParent >= 0 && ElemMtx(g_world, g_gun.model, g_gun.magParent, pw);
    const bool haveCl = ElemMtx(g_local, g_gun.model, g_gun.chamberId, cl);
    float seat[12] = {};
    const bool haveSeat = st->haveMagRest && havePw;
    if (haveSeat) { Mul34(pw, st->magRest, seat); }
    float toMag = -1.0f, toWell = -1.0f, outAlong = 0.0f, offSeat = -1.0f, align = -1.0f;
    if (hand && haveMw) {
        float g[3];
        XformPt(mw, g_gun.row ? g_gun.row->magSpot : reload::Vec3{}, g);   // the grab spot (pistol: the joint)
        toMag = Len3(hand[0] - g[0], hand[1] - g[1], hand[2] - g[2]);
    }
    if (hand && haveSeat) { toWell = Len3(hand[0] - seat[3], hand[1] - seat[7], hand[2] - seat[11]); }
    if (haveSeat && haveMw) {
        const float dx = mw[3] - seat[3], dy = mw[7] - seat[7], dz = mw[11] - seat[11];
        offSeat = Len3(dx, dy, dz);
        // the well's OUT axis: the row's magOutAxis in the parent frame (final fix wave C1; SMG (0 0 1) = the parent's +Z, G7), the
        // pistol (no row) the parent's +Z. Once per STATE line.
        float ax[3];
        const bool okAx = g_gun.row ? AxisOf(pw, g_gun.row->magOutAxis, ax) : Axis(pw, 2, ax);
        if (okAx) { outAlong = dx * ax[0] + dy * ax[1] + dz * ax[2]; }
        align = RotAngleDeg(mw, seat);
    }
    // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=yes
    LOG_WINDOW(g_stateLineFirst, "reload: [RELOAD] STATE step %s (drew: %s; START refused x%lld, ADD-BULLET refused x%lld) | rounds %d / max %d / spare %d%s | "
        "reload idx %d | hand %s: ->mag %.3f ->well %.3f m | mag (as read) vs seat: out-along-well %+.4f m, off %.4f m, align %.1f deg | "
        "slide/bolt local x %+.4f (rest %s %+.4f; pistol clip 0.1182 rest / 0.1550 locked; SMG clips 0.1199 / 0.1172 rest, +0.0826 / "
        "+0.0461 back) | mag rest %s | player %s | left pose: fingers %s t %.2f, lock %s t %.2f hand bone (%+.3f %+.3f %+.3f) "
        "%.3f m from the controller grip | mag in hand: %s | APPLIED since the last STATE: lock rot %ld pos %ld solves, finger passes "
        "%ld (%ld pins), finger row not found %ld",
        StepName(st->machine.step()), drew, static_cast<long long>(g_startRefused), static_cast<long long>(g_addRefused), rounds,
        maxR, spare, (okR && okM && okS) ? "" : " (a read FAILED: -1)", idx,
        hand ? (achieved ? "LEFT achieved" : "LEFT commanded") : "NONE (no left grip pose published)", toMag, toWell, outAlong,
        offSeat, align, haveCl ? cl[3] : 0.0f, g_gun.haveRest ? "latched" : "not latched",
        g_gun.haveRest ? g_gun.restChamber[3] : 0.0f, st->haveMagRest ? "LATCHED" : "NOT LATCHED (distances to the well = -1)",
        ctlPlayer == player ? "= ctl vfunc +32" : "*** ctl vfunc +32 differs from vis +0x50 ***",
        g_fingerRow ? g_fingerRow->key : "none", fingerT, g_lockRow ? g_lockRow->key : "none", lockT, lockPos[0], lockPos[1],
        lockPos[2], lockGap, !st->relRow ? "as latched at the grab (no row / no achieved hand)"
                             : st->relRow == g_gun.pullRow ? "the pull row" : "the mag row",
        static_cast<long>(InterlockedExchange(&g_cntLockRot, 0)), static_cast<long>(InterlockedExchange(&g_cntLockPos, 0)),
        static_cast<long>(InterlockedExchange(&g_cntFingerPasses, 0)), static_cast<long>(InterlockedExchange(&g_cntFingerPins, 0)),
        static_cast<long>(InterlockedExchange(&g_cntFingerNoRow, 0)));
}

// Task 5: one step of the machine for the held pistol, and what it draws. Returns what was drawn (for the STATE line).
const char* DriveSteps(uintptr_t ctrl, ItemState* st, const float* hand, const float* handR9, bool achieved, const float* cmdPos,
                       const float* holderT, ULONGLONG now)
{
    // The rack pull on a gun with a rack row is read from the COMMANDED hand (the controller's grip point) from the rack grab on:
    // while the lock holds the DRAWN hand on the slide / bolt, the achieved hand would measure the part it is locked to (spec
    // section 3 "Read while planning" (b)) - and latching the grab from the achieved hand, then reading the commanded one, would
    // add the solve's own shortfall (median 31 / 41 mm, [SOLVE PEAK]) to the pull at once. No rack row / no commanded hand: today's.
    const float* pullHand = (g_gun.rackRow && cmdPos) ? cmdPos : hand;
    float pw[12], mw[12], cw[12], cpw[12];
    const bool havePw = g_gun.magParent >= 0 && ElemMtx(g_world, g_gun.model, g_gun.magParent, pw);
    const bool haveMw = ElemMtx(g_world, g_gun.model, g_gun.magId, mw);
    const bool haveCw = ElemMtx(g_world, g_gun.model, g_gun.chamberId, cw);
    const bool haveCpw = g_gun.chamberParent >= 0 && ElemMtx(g_world, g_gun.model, g_gun.chamberParent, cpw);
    float seat[12] = {};
    const bool haveSeat = st->haveMagRest && havePw;
    if (haveSeat) { Mul34(pw, st->magRest, seat); }
    float H[12] = {};
    const bool haveH = hand && handR9 && Hand34(hand, handR9, H);
    // Final fix wave (C1 / C2 ruling): the mag's OUT axis and the rack's BACK axis are the ROW's (reload_guns.h magOutAxis / rackAxis,
    // in their bones' PARENT frames) rotated by the parent's world - an M16's bone_clip parent (bone_root) +Z points along the BARREL
    // and its +X UP, so the parent axes the SMG used were 66 / 106 deg off on every rifle. The SMG rows carry (0 0 1) / (1 0 0) = the
    // parent's +Z / +X exactly as before; the PISTOL (no row) keeps the parent's +Z / +X. COST: two 3x3-by-vector products per update.
    float well[3] = { 0, 0, 1 };
    const bool haveWell = havePw && (g_gun.row ? AxisOf(pw, g_gun.row->magOutAxis, well) : Axis(pw, 2, well));
    float slideAx[3] = { 1, 0, 0 };
    const bool haveSlideAx = haveCpw && (g_gun.row ? AxisOf(cpw, g_gun.row->rackAxis, slideAx) : Axis(cpw, 0, slideAx));

    reload::Input in{};
    in.family = g_gun.family;
    in.racks = g_gun.fx && g_gun.fx->racks;   // racking is the gun's ROW (Jay 2026-11-02), never the family
    // The grab spots: Jay's (SMG, carried on their bones) or the joints themselves (pistol - zero spots, exactly RL2C's measure).
    // COST: two 3x4-by-point products per update.
    const reload::Vec3 kZero{};
    const reload::Vec3& magSpot = g_gun.row ? g_gun.row->magSpot : kZero;
    const reload::Vec3& rackSpot = g_gun.row ? g_gun.row->rackSpot : kZero;
    in.manualOn = ManualReload;
    in.gripDown = g_gripDown != 0;
    in.leftInPouch = HolsterLeftPouchHeld();
    int rounds = 0, maxR = 0;
    CallCtlInt(g_rounds, ctrl, &rounds);
    CallCtlInt(g_max, ctrl, &maxR);
    in.rounds = rounds;
    in.maxRounds = maxR;
    const reload::Step before = st->machine.step();
    if (before == reload::Step::Empty) { st->spare = SpareOf(ctrl, st->item); }   // the pouch reads it - only while the mag is out
    in.spare = st->spare;
    // The mag as WE draw it this update (the game's read is our own write of the previous update while a step draws it).
    float drawn[12] = {};
    bool haveDrawn = false;
    if (before == reload::Step::Idle || before == reload::Step::NeedsRack || before == reload::Step::Racking) {
        if (haveMw) { memcpy(drawn, mw, sizeof(drawn)); haveDrawn = true; }
    } else if ((before == reload::Step::MagInHand || before == reload::Step::FreshInHand) && haveH && st->haveRel) {
        Mul34(H, st->rel, drawn);
        haveDrawn = true;
    }
    if (hand && haveDrawn && before == reload::Step::Idle) {
        float g[3];
        XformPt(drawn, magSpot, g);
        in.handToMagM = Len3(hand[0] - g[0], hand[1] - g[1], hand[2] - g[2]);
        // Review finding 4 (solid guns): the gun now holds the hand's fingertip off its hull, so the ACHIEVED hand may stop short of
        // an SMG's mag spot; the COMMANDED grip (the controller's) still reaches it. The nearer of the two counts - never harder to
        // grab than before.
        if (cmdPos) {
            const float dc = Len3(cmdPos[0] - g[0], cmdPos[1] - g[1], cmdPos[2] - g[2]);
            if (dc < in.handToMagM) { in.handToMagM = dc; }
        }
    }
    // THE WELL RAIL (magrail_logic.h, Jay 2026-11-02 (4)): on a gun with a well row the mag goes "down the well first, then follows
    // my hand" - the pull is the hand's travel ALONG the well's out axis (the row's magOutAxis) since the grab, never sideways; and a fresh mag
    // seats only from the rail (locked on lined up at the open end). No well row: as before (any direction / 3-D distance).
    // COST: a few dot products per update while Pulling / FreshInHand.
    const bool railGun = g_gun.haveClear && haveSeat && haveWell;
    reload::rail::Params rp;
    rp.clearM = g_gun.clearM;
    rp.easeM = kRailEaseM;
    rp.captureM = kRailCaptureM;
    rp.alignDeg = ReloadAlignDeg;
    float railS = 0.0f;                                     // the travel the rail draws at (on the rail: the controller's from the anchor)
    reload::rail::Kind railKind = reload::rail::Kind::Free;
    // RUN MR1: the drawn hand is locked to the mag on the rail, so it would measure itself - the rail's travel is the CONTROLLER's
    // (the rack's rule, reload_hands.h UseCommandedHand). No controller pose -> the drawn hand, and no lock (below).
    const float* railHand = cmdPos ? cmdPos : hand;
    const auto RailTravel = [&]() {
        return st->railS0 + (railHand[0] - st->railCmd0[0]) * well[0] + (railHand[1] - st->railCmd0[1]) * well[1] +
               (railHand[2] - st->railCmd0[2]) * well[2];
    };
    if (before == reload::Step::Pulling && hand) {
        if (railGun && railHand) {
            railS = RailTravel();   // anchored at the Grab (railS0 = 0)
            if (railS < 0.0f) { railS = 0.0f; }
            in.pullFromGrabM = railS;
            railKind = reload::rail::Decide(st->rail, railS, 0.0f, 0.0f, rp);
        } else {
            // The pull-out: the hand's distance from where it grabbed, in ANY direction (Jay after HP2).
            in.pullFromGrabM = Len3(hand[0] - st->grabHand[0], hand[1] - st->grabHand[1], hand[2] - st->grabHand[2]);
        }
    }
    if (before == reload::Step::FreshInHand && haveDrawn && haveSeat) {
        if (railGun && railHand) {
            float lat = 0.0f, ang = 180.0f;
            if (st->rail.onRail) {
                railS = RailTravel();   // on the rail: the controller's travel from where it locked on (lined up by construction)
                ang = 0.0f;
            } else {
                reload::rail::Measure(seat, well, drawn, &railS, &lat, &ang);   // free: the mag in the fingers (hand not locked)
            }
            const bool wasOn = st->rail.onRail;
            railKind = reload::rail::Decide(st->rail, railS, lat, ang, rp);
            if (st->rail.onRail && !wasOn) {
                st->railS0 = railS;     // locked on: anchor the controller travel here
                memcpy(st->railCmd0, railHand, sizeof(st->railCmd0));
            }
            // Seats only on the rail (lined up by construction): its travel out of the seat is the distance to the well.
            in.handToWellM = st->rail.onRail ? (railS < 0.0f ? 0.0f : railS) : 1e9f;
            in.alignDeg = st->rail.onRail ? 0.0f : ang;
        } else {
            in.handToWellM = Len3(drawn[3] - seat[3], drawn[7] - seat[7], drawn[11] - seat[11]);
            const float a = RotAngleDeg(drawn, seat);
            in.alignDeg = a < 0.0f ? 180.0f : a;
        }
    }
    // THE COMMANDO'S TWO HANDLES (rifle plan Task 5): the rack spot is the handle the hand is NEARER to (reload_guns.h NearerRackSpot),
    // judged with the hand in bone_chamber's frame - the controller's grip point when there is one (the hand the rack pull reads),
    // else the drawn hand. Every other gun answers rackSpot. The side is LATCHED once, at the rack grab (RackPull, below); this
    // update's pick only feeds the grab-distance test. COST: Commando only - one 3x3 un-rotate (orthogonal columns, any scale) + two
    // distance compares per update; any other gun one bool test.
    bool rackSideNow2 = false;
    const reload::Vec3* rackSpotNow = &rackSpot;
    if (haveCw && pullHand && g_gun.row && g_gun.row->hasRack2) {
        const float d[3] = { pullHand[0] - cw[3], pullHand[1] - cw[7], pullHand[2] - cw[11] };
        float loc[3] = {};
        bool okLoc = true;
        for (int c = 0; c < 3 && okLoc; ++c) {
            const float l2 = cw[c] * cw[c] + cw[4 + c] * cw[4 + c] + cw[8 + c] * cw[8 + c];
            okLoc = l2 > 1e-12f && std::isfinite(l2);
            if (okLoc) { loc[c] = (cw[c] * d[0] + cw[4 + c] * d[1] + cw[8 + c] * d[2]) / l2; }
        }
        if (okLoc) {
            const reload::Vec3 handLocal{ loc[0], loc[1], loc[2] };
            rackSpotNow = &reload::NearerRackSpot(*g_gun.row, handLocal, &rackSideNow2);
        }
    }
    if (hand && haveCw) {
        float g[3];
        XformPt(cw, *rackSpotNow, g);
        in.handToSlideM = Len3(hand[0] - g[0], hand[1] - g[1], hand[2] - g[2]);
        // 2026-11-02 (the solid HAND, gunshape.cpp): the drawn hand now stops where its fingers / palm meet the slide, which can
        // hold the handholder short of the rack spot - so, as the mag grab above, the nearer of the drawn hand and the controller.
        if (cmdPos) {
            const float dc = Len3(cmdPos[0] - g[0], cmdPos[1] - g[1], cmdPos[2] - g[2]);
            if (dc < in.handToSlideM) { in.handToSlideM = dc; }
        }
    }
    if (before == reload::Step::Racking && pullHand && haveSlideAx) {
        in.slideBackM = (pullHand[0] - st->grabHand[0]) * slideAx[0] + (pullHand[1] - st->grabHand[1]) * slideAx[1] +
                        (pullHand[2] - st->grabHand[2]) * slideAx[2];
    }
    // No seat known yet (the mag's rest not latched for this item): no grab - nothing could be put back.
    if (!haveSeat) { in.handToMagM = 1e9f; }

    reload::Params p;
    p.snapM = ReloadSnapM;
    p.alignDeg = ReloadAlignDeg;
    // The rail: the mag is out (MagInHand) only once it has cleared the well AND eased into the fingers - never inside the well.
    p.pullOutM = railGun ? std::fmax(ReloadPullOutM, g_gun.clearM + kRailEaseM) : ReloadPullOutM;
    p.rackPullM = reload::RackPullFor(g_gun.family, g_gun.row, ReloadRackPullM);   // pistol: its full slide travel
    // + kGrabWidenM (trace 20261116_1126): the drawn palm now sits on the controller - the old union of the two is contained.
    p.grabM = ReloadGrabM + reload::kGrabWidenM;
    p.rackGrabM = reload::RackGrabFor(g_gun.family, ReloadGrabM) + reload::kGrabWidenM;   // pistol slide: 0.12 + 0.04 m
    const reload::Out o = st->machine.Update(in, p);

    // Events: latch what the next steps need.
    // The rail lives only through Pulling / FreshInHand; the grabbed mag starts ON it, seated (no jump into the fingers at the grab).
    if (o.step != reload::Step::Pulling && o.step != reload::Step::FreshInHand) { st->rail = reload::rail::State{}; }
    if (o.ev == reload::Event::PouchTake) { st->rail = reload::rail::State{}; railKind = reload::rail::Kind::Free; }
    if (o.ev == reload::Event::Grab) {
        st->rail.onRail = railGun && railHand;
        railKind = st->rail.onRail ? reload::rail::Kind::Rail : reload::rail::Kind::Free;
        railS = 0.0f;
        st->railS0 = 0.0f;   // the rail's travel is the controller's from here (RUN MR1)
        if (railHand) { memcpy(st->railCmd0, railHand, sizeof(st->railCmd0)); }
    }
    if (o.ev == reload::Event::Grab && hand) {
        memcpy(st->grabHand, hand, sizeof(st->grabHand));
        // The mag is IN THE HAND from the grab (Jay after HP2: "In hand at the grip"), at once: the pulled mag's row (the pistol's pull
        // row, else the mag row - rows from Jay's approved poses, D-302). No row / no achieved hand: the grab's own latch, inv(H) x
        // the mag at the seat (the grab is at a seated mag), so the mag follows the hand from where it sat.
        st->haveRel = false;
        st->relRow = nullptr;
        if (!SetRelRow(st, reload::MagHoldRow(reload::Step::Pulling, g_gun.pullRow, g_gun.magRow), achieved, holderT, haveSeat,
                       seat) && haveH && haveSeat) {
            float Hi[12];
            RigidInv(H, Hi);
            Mul34(Hi, seat, st->rel);
            st->haveRel = true;
        }
    }
    // PullOut: the mag is already in the hand - nothing to re-latch. The fresh mag from the pouch takes the HOLD row at once.
    if (o.ev == reload::Event::PouchTake) {
        SetRelRow(st, reload::MagHoldRow(reload::Step::FreshInHand, g_gun.pullRow, g_gun.magRow), achieved, holderT, haveSeat, seat);
    }
    if (o.ev == reload::Event::RackPull && pullHand) { memcpy(st->grabHand, pullHand, sizeof(st->grabHand)); }
    if (o.ev == reload::Event::RackPull) {
        // SMG: where the bolt IS at the grab (locked back or forward - spec [UNKNOWN] 1), so the drawn bolt never jumps forward of it.
        st->haveBoltGrab = ElemMtx(g_local, g_gun.model, g_gun.chamberId, st->boltGrab);
        // Rifle plan Task 5: the handle this grab took - the side NearerRackSpot picked this update - is latched for the whole rack;
        // its pose is the side's rack row (RackRowFor(..., second)). Once per grab.
        st->rackSecond = rackSideNow2;
        if (g_gun.row && g_gun.row->hasRack2 && EventLine()) {
            const reload::HandRow* sideRow = rackSideNow2 ? g_gun.rackRow2 : g_gun.rackRow;
            // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=yes - which Commando handle the rack grab took
            // Labels (final fix wave ruling): spec §4's frame has x = the gun's LEFT, so rackSpot (the -x dot) is the gun's RIGHT
            // handle and rackSpot2 (the +x dot, the "+x" pose) the gun's LEFT.
            LOG("reload: [RELOAD] RACK SIDE item %p: the %s handle (spot %+.4f %+.4f %+.4f bone-local, hand from it %.3f m) - pose %s.",
                reinterpret_cast<void*>(st->item), rackSideNow2 ? "+x (gun's left)" : "-x (gun's right)", rackSpotNow->x, rackSpotNow->y, rackSpotNow->z,
                in.handToSlideM > 1e8f ? -1.0f : in.handToSlideM,
                sideRow ? sideRow->key : "*** NONE - this side has no pose row, the hand is not locked (today's hand) ***");
        }
    }
    if (o.ev != reload::Event::None && EventLine()) {
        // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=no
        LOG("reload: [RELOAD] EVENT %s -> step %s | item %p rounds %d / %d spare %d | hand ->mag %.3f, pull from grab %.3f, ->well %.3f, "
            "align %.1f, ->slide %.3f, slide back %.3f | hand %s%s | rack pull read from the %s hand | mag in hand: %s",
            EventName(o.ev), StepName(o.step), reinterpret_cast<void*>(st->item), rounds, maxR, in.spare,
            in.handToMagM > 1e8f ? -1.0f : in.handToMagM, in.pullFromGrabM, in.handToWellM > 1e8f ? -1.0f : in.handToWellM,
            in.alignDeg, in.handToSlideM > 1e8f ? -1.0f : in.handToSlideM, in.slideBackM,
            hand ? (achieved ? "achieved" : "commanded") : "NONE", (o.ev == reload::Event::Grab && !st->haveRel)
                ? " - *** no hand basis / seat at the grab: the mag stays seated ***" : "",
            pullHand == cmdPos && cmdPos ? "COMMANDED (controller)" : (achieved ? "achieved" : "commanded"),
            !st->haveRel ? "none" : !st->relRow ? "the grab's latch (no row / no achieved hand)"
                                   : st->relRow == g_gun.pullRow ? "the pull row" : "the mag row");
    }
    if (o.callAddBullet) {
        const int r0 = rounds, s0 = SpareOf(ctrl, st->item);
        const bool called = CallAddBullet(ctrl, st->item);
        int r1 = -1;
        CallCtlInt(g_rounds, ctrl, &r1);
        const int s1 = SpareOf(ctrl, st->item);
        if (EventLine()) {
            // [PROBE: RELOAD] gate=manual_reload  site=HEADSET  log-only=no
            LOG("reload: [RELOAD] SEAT (%s) rounds %d -> %d spare %d -> %d%s", EventName(o.ev), r0, r1, s0, s1,
                called ? "" : " - *** the add-bullet call FAULTED ***");
        }
    }
    // Solid guns plan Task 5 (spec G3 / G4): every step the player DOES (o.fx: mag grab, mag out, pouch take, seat, rack grab, rack
    // done) buzzes the left hand once (holster_haptic = the holsters' own amplitude; 0 = the player turned pulses off - none here
    // either) and plays the gun's own reload sound(s) for that step (reload_fx.h; none for grab / pouch / a gun with no row).
    // COST: per step event only - one pulse + up to 2 sound calls (gunsound.cpp).
    if (o.fx != reload::Fx::None) {
        if (HolsterHaptic > 0.0f) { PulseController(kLeftHand, HolsterHaptic, kPulseMs); }
        int ns = 0;
        const reload::Snd* snd = reload::SoundsFor(g_gun.fx, o.fx, &ns);
        for (int i = 0; i < ns; ++i) { GunSoundPlay(g_curPlayer, snd[i].name, snd[i].vol, snd[i].delayS, snd[i].startS); }
    }

    // Draw the step (bone_magazine's world, written after the game's update this tail - the bow's route).
    const char* drew = "nothing (the game's own pose)";
    bool railLock = false;     // the drawn hand locked to the mag on the well's line this update (RUN MR1)
    float railPart[12] = {};   // the mag as drawn - the lock's part
    float w[12];
    if (railGun && (o.step == reload::Step::Pulling || o.step == reload::Step::FreshInHand) && haveH && st->haveRel) {
        // THE WELL RAIL (Jay 2026-11-02 (4)): on the well's line / easing to the fingers / stopped at the lip / in the fingers.
        float inHand[12];
        Mul34(H, st->rel, inHand);
        reload::rail::Draw(railKind, seat, well, railS, railS, inHand, rp, w);
        const bool pull = o.step == reload::Step::Pulling;
        const char* what = railKind == reload::rail::Kind::Rail ? (pull ? "mag on the well's line (pulling)" : "mag on the well's line (inserting)")
                         : railKind == reload::rail::Kind::Ease ? (pull ? "mag easing to the hand (pulling)" : "mag easing onto the well's line")
                         : railKind == reload::rail::Kind::Lip ? "mag stopped at the well's lip (not lined up)"
                         : (pull ? "mag in the hand (pulling)" : "mag in the hand");
        drew = SetWorld(g_gun.model, g_gun.magId, w) ? what : "mag on the rail - WRITE FAULTED";
        // RUN MR1: the drawn hand LOCKED to the mag while it is on the well's line - only with the controller's pose (the travel
        // reads it; locking without it would let the locked hand measure itself). Not at the lip / easing / free (my call).
        if (railKind == reload::rail::Kind::Rail && cmdPos) {
            memcpy(railPart, w, sizeof(railPart));
            railLock = true;
        }
    } else if (o.step == reload::Step::Pulling && haveH && st->haveRel) {
        Mul34(H, st->rel, w);   // in the hand from the grab
        drew = SetWorld(g_gun.model, g_gun.magId, w) ? "mag in the hand (pulling)" : "mag in the hand (pulling) - WRITE FAULTED";
    } else if (o.step == reload::Step::Pulling && haveSeat && haveWell) {
        // no hand basis at the grab: today's slide along the well
        float along = 0.0f;
        if (hand) {
            along = (hand[0] - st->grabHand[0]) * well[0] + (hand[1] - st->grabHand[1]) * well[1] +
                    (hand[2] - st->grabHand[2]) * well[2];
            if (along < 0.0f) { along = 0.0f; }
        }
        memcpy(w, seat, sizeof(w));
        w[3] += well[0] * along; w[7] += well[1] * along; w[11] += well[2] * along;
        drew = SetWorld(g_gun.model, g_gun.magId, w) ? "mag along the well" : "mag along the well - WRITE FAULTED";
    } else if ((o.step == reload::Step::MagInHand || o.step == reload::Step::FreshInHand) && haveH && st->haveRel) {
        Mul34(H, st->rel, w);
        drew = SetWorld(g_gun.model, g_gun.magId, w) ? "mag in the hand" : "mag in the hand - WRITE FAULTED";
    } else if (o.step == reload::Step::Empty && haveSeat) {
        memcpy(w, seat, sizeof(w));
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) { w[4 * r + c] *= kHideScale; }
        }
        drew = SetWorld(g_gun.model, g_gun.magId, w) ? "mag shrunk (let go)" : "mag shrunk - WRITE FAULTED";
    } else if (o.magAtSeat && haveSeat) {
        // RUN RL2B: the game does not always re-pose bone_magazine after our last write - the mag stayed 5.4 cm out for 6 s after a
        // seat. Drawing the seat (parent x the latched rest local) for reload::kSettleUpdates updates leaves the seated mag drawn.
        drew = SetWorld(g_gun.model, g_gun.magId, seat) ? "mag at its seat (settle)" : "mag at its seat - WRITE FAULTED";
    }
    // (2026-11-02: the pistol has no rack - its fired-empty add-bullet is at the SEAT, which lets the game draw the slide forward.)
    // The PISTOL's slide is not written: with the clip at 0 the game draws it locked back (G7, RL0B 0.1550) and the rack's add-bullet
    // lets the game draw it forward - its rear stop IS the pull, so a drawn travel would show nothing more (Task 5 ruling).
    // The SMG's BOLT is (SMG plan Task 3 step 4): during the rack it follows the hand back along its parent's +X from where it was
    // grabbed (BoltLocalX - never forward of that, never past the gun's own clip travel), then settles at rest (RackDone) or at the
    // grab (a rack let go short) for reload::kSettleUpdates updates - the game may not re-pose a bone after our last write (RL2B).
    // Rest = the latched rest local x (needs a draw with rounds > 0), else the mesh files' 0.1182 (all five SMG meshes, the generator's
    // extract). COST: one parent world read (already made) + one write per update while racking / settling.
    // Rifle plan Task 5: every TABLE gun (SMG + rifles - IsTableGun, a row test, never a racking test: a non-racking row never gets
    // here, its machine never reaches Racking). kChamberClipRest is the SMG meshes' value - an M16's bone_chamber rests at x 0.1551
    // (the row's chamberRest, from the mesh) - so the rest is reload_guns.h BoltRestPoint: latched this draw > the row's mesh rest
    // (x > 0) > (kChamberClipRest, 0, 0) (fix round 1 ruling; never the grab position - an empty M16 may be drawn locked back, [UNKNOWN]).
    // FINAL FIX WAVE (C2 ruling): the bolt moves ALONG THE ROW's rackAxis (bone_chamber's PARENT frame, pointing back): its local t =
    // the grab's + (s - grab.axis) x axis with s = BoltLocalX in the axis coordinate (never forward of the grab, never past the travel);
    // an M16's handle goes straight back along the barrel (its parent's +X is UP - the old x-only draw pushed it up through the
    // receiver and its x DECREASES in the clip, so the rack never completed). The SMG's (1 0 0) gives the old x values bit for bit
    // (reload_guns_test SmgAxesUnchanged). COST: three 3-dots + a few compares per update while racking / settling.
    if (reload::IsTableGun(g_gun.family) && g_gun.row && st->haveBoltGrab && haveCpw &&
        (o.slideFollowsHand || o.slideAtRest || o.slideAtGrab)) {
        const reload::Vec3 latched{ g_gun.restChamber[3], g_gun.restChamber[7], g_gun.restChamber[11] };
        const reload::Vec3 rest = reload::BoltRestPoint(g_gun.haveRest, latched, g_gun.row, kChamberClipRest);
        const reload::Vec3 grabT{ st->boltGrab[3], st->boltGrab[7], st->boltGrab[11] };
        const reload::Vec3& ax = g_gun.row->rackAxis;
        float local[12];
        memcpy(local, st->boltGrab, sizeof(local));
        if (o.slideFollowsHand || o.slideAtRest) {
            const reload::Vec3 t = o.slideFollowsHand
                ? reload::BoltLocalAlong(grabT, rest, ax, g_gun.row->boltTravelM, in.slideBackM)
                : reload::BoltPointAt(grabT, ax, reload::Dot3(rest, ax));
            local[3] = t.x; local[7] = t.y; local[11] = t.z;
        }   // slideAtGrab: the grab's own local, as copied
        float bw[12];
        Mul34(cpw, local, bw);
        const bool ok = SetWorld(g_gun.model, g_gun.chamberId, bw);
        if (ok) { memcpy(cw, bw, sizeof(cw)); }   // the lock follows the bolt as WE draw it this update
        drew = o.slideFollowsHand ? (ok ? "bolt with the hand" : "bolt with the hand - WRITE FAULTED")
             : o.slideAtRest ? (ok ? "bolt at rest (settle)" : "bolt at rest - WRITE FAULTED")
             : (ok ? "bolt at the grab (settle)" : "bolt at the grab - WRITE FAULTED");
    }

    // Solid guns plan Task 5 (G2): the mag IN THE HAND, for the solve's mag stop (gunshape.cpp GunShapeMagStop) - the drawn mag is
    // H x st->rel (H = the ACHIEVED handholder position + the drawn hand basis); published only while a step draws it there from an
    // achieved H (the solve rebuilds H at its own target with the same convention). STAGED here (game thread); ReloadTick's MagPublish
    // guard publishes it under g_magLock for the solves on any thread (RUN SG3).
    g_stageOn = achieved && haveH && st->haveRel &&
                (o.step == reload::Step::Pulling || o.step == reload::Step::MagInHand || o.step == reload::Step::FreshInHand);
    if (g_stageOn) {
        memcpy(g_stageRel, st->rel, sizeof(g_stageRel));
        Mul34(H, st->rel, g_stageDrawn);   // the mag as drawn this tick (= the draw branches' w) - review finding 3
    }

    // For the pad thread: R2 withheld while a step is live; a left press starting now is ours mid-reload or at the mag / slide.
    // Rifle plan Task 5: the same answer also keeps that press from the two-handed hold (ovr2xr.cpp: ReloadPadPoll before TwoHandPoll,
    // reload_logic.h ReloadOwnsLeftPress / TwoHandPressRefused). On the Commando in.handToSlideM is to the NEARER handle.
    const bool atPart = (in.handToMagM <= p.grabM) ||   // the grab's own (widened) zone - trace 20261116_1126
                        (o.step == reload::Step::NeedsRack && in.handToSlideM <= p.rackGrabM);
    Publish(o.withholdR2, reload::ReloadOwnsLeftPress(o.step != reload::Step::Idle, atPart));
    // The left hand's poses for the solve and the finger pass (plan 2026-11-01-reload-hand-poses Task 3 step 1): the rack lock while
    // Racking on a gun with a rack row, locked to bone_chamber as drawn (the pistol's slide is the game's - reload.cpp above; the
    // SMG's bolt is ours); the fingers from the active pose's row. A gun with no row for a pose keeps today's hand for it.
    // The mag hold's row per step: the pistol's PULL row while the pulled mag is held (Pulling / MagInHand), the mag row for the fresh
    // one (reload::MagHoldRow) - the same row the mag's place in the hand was set from.
    const reload::HandPose pose = reload::ActiveHandPose(o.step);
    // Rifle plan Task 5: the rack pose is the grabbed SIDE's (latched at RackPull): the Commando's +x handle (gun's left) -> its "+x" row (null =
    // no row for that side -> no lock, today's hand - never the other side's pose). Every other gun: rackSecond stays false.
    // COST: one bool test.
    const reload::HandRow* rackRowNow = st->rackSecond ? g_gun.rackRow2 : g_gun.rackRow;
    const reload::HandRow* fingerRow = pose == reload::HandPose::Rack ? rackRowNow
                                     : pose == reload::HandPose::MagHold ? reload::MagHoldRow(o.step, g_gun.pullRow, g_gun.magRow)
                                     : nullptr;
    // The lock: the RACK (on bone_chamber, the rack row) or - RUN MR1 - the MAG on the well's line (on bone_magazine as drawn, the
    // mag-hold row; reload_hands.h LockedHand: the rows are the hand bone in the part's frame for both). It eases on / off over
    // kHandBlendMs. The part fed while neither is on must be the LAST lock's part, so the blend-out of a mag lock is not computed
    // on bone_chamber (g_lockIsMag).
    const bool rackLock = pose == reload::HandPose::Rack && rackRowNow != nullptr && haveCw;
    const reload::HandRow* magLockRow = railLock ? reload::MagHoldRow(o.step, g_gun.pullRow, g_gun.magRow) : nullptr;
    const bool magLock = !rackLock && magLockRow != nullptr;
    if (rackLock) { g_lockIsMag = false; }
    if (magLock) { g_lockIsMag = true; }
    const float* part = rackLock ? cw : magLock ? railPart : g_lockIsMag ? nullptr : (haveCw ? cw : nullptr);
    HandsPublish(rackLock || magLock, rackLock ? rackRowNow : magLockRow, part, fingerRow, reload::PoseEases(pose), now);
    return drew;
}

}  // namespace

// ===== WRIST HUD STAGE B (trace docs/traces/20261113_1420_wrist_stageB_gun.md): the right hand's gun FAMILY for the weapon box, read
// whatever manual_reload is - ClassifyModel's own "pre" classification (type + prefix + mesh, the bones assumed), once per held model
// change, on the game thread. NEVER calls Resolve() (it installs game detours) and never writes g_gun. Only while the wrist HUD is on.
// COST: off -> one bool test; on -> one held-model read + compare per update; a change -> ~6 guarded reads + one table walk.
volatile LONG64 g_wfModel = 0;                 // the held model the family below was read for (0 = none)
const char* volatile g_wfName = "None";        // a FamilyName literal (this file)
volatile LONG g_wfType = 0;                    // the item type read with it (0 = not read) - the wrist key "<family>/<type>" (trace 1540)
volatile LONG g_wfSaid = 0;

void WristFamilyTick(void* playerVis)
{
    if (!wristquad::OnThisLaunch()) { return; }
    static uintptr_t s_model = 0, s_inv = 0;
    const uintptr_t ent = BoneProbeCapturedEntity();
    uintptr_t model = 0;
    if (!ent || !ReadPtr(ent + kHeldItemOff, &model) || !model) {
        if (s_model) { s_model = 0; s_inv = 0; InterlockedExchange64(&g_wfModel, 0); g_wfName = "None"; InterlockedExchange(&g_wfType, 0); }
        return;
    }
    const uintptr_t inv = HeldItemInventoryItem(model);
    if (model == s_model && inv == s_inv) { return; }
    s_model = model; s_inv = inv;
    reload::Family fam = reload::Family::None;
    const char* why = nullptr;
    uintptr_t player = 0, ctrl = 0, item = 0, rawType = 0;
    int slot = -1;
    char prefix[64] = {}, mesh[96] = {};
    if (!playerVis || !ReadPtr(reinterpret_cast<uintptr_t>(playerVis) + kVisPlayerOff, &player) || !player ||
        !ReadPtr(player + kFireCtlOff, &ctrl) || !ctrl) { why = "no player / firearm controller"; }
    else if (!ReadInt(ctrl + kCtlSlotOff, &slot) || slot < 0) { why = "the firearm controller has no selected slot"; }
    else if (!CallV1(player, kSelItemVfn, static_cast<unsigned>(slot), &item) || !item) { why = "no selected item"; }
    else if (!CallV0(item + kItemDescOff, kDescTypeVfn, &rawType)) { why = "the item type read faulted"; }
    else if (!ReadPrefix(item + kItemDescOff, prefix, sizeof(prefix))) { why = "the anim prefix read faulted"; }
    else {
        const bool haveMesh = ItemMeshName(model, mesh, sizeof(mesh));
        const reload::Classified c = reload::Classify(static_cast<unsigned>(rawType), prefix, haveMesh ? mesh : nullptr, true, true);
        fam = c.family;
        if (fam == reload::Family::None) { why = c.why; }
    }
    g_wfName = FamilyName(fam);
    // the type only when it was READ (the prefix / mesh / family may still be missing - the wrist key then reads "Other/<type>")
    const bool typeRead = item && rawType != 0;
    InterlockedExchange(&g_wfType, typeRead ? static_cast<LONG>(rawType) : 0);
    InterlockedExchange64(&g_wfModel, static_cast<LONG64>(model));
    // [PROBE: WRISTGUN] gate=ui_wrist_hud (read at launch)  site=HEADSET  log-only=no
    LOG("[WRISTGUN] held model %p (%s): firearm controller item type %u prefix '%s' -> family %s%s%s%s", reinterpret_cast<void*>(model),
        mesh[0] ? mesh : "mesh not read", static_cast<unsigned>(rawType), prefix, FamilyName(fam), why ? " (" : "", why ? why : "",
        why ? ")" : "");
    InterlockedExchange(&g_wfSaid, 1);
}

const char* ReloadWristFamily(uintptr_t* model, int* type)
{
    if (model) { *model = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_wfModel, 0, 0)); }
    if (type) { *type = static_cast<int>(InterlockedCompareExchange(&g_wfType, 0, 0)); }
    return g_wfName;
}

void ReloadTick(void* playerVis, const float* leftPos, const float* leftR9, bool leftAchieved, const float* leftCmdPos,
                const float* leftHolderT, void* playerModel, int lHandId)
{
    g_stageOn = false;   // staged again below only while a step draws the mag in the hand (never stale across a return)
    const MagPublish publishOnReturn;   // RUN SG3: publishes the staged mag-in-hand under g_magLock on EVERY return below
    WristFamilyTick(playerVis);         // WRIST HUD stage B: the held gun's family, whatever manual_reload is (off: one bool test)
    if (!ManualReload) { return; }
    if (!Resolve()) { return; }
    // Fix round 1 (review M2): a pump away > 150 ms is cleared here, whatever is held now. COST: one compare while nothing is away.
    if (g_pumpAwayMs) { PumpAwayTick(GetTickCount64()); }
    if (g_dbAwayMs) { DbAwayTick(GetTickCount64()); }   // the double-barrel's away guard (one compare while nothing is away)
    const uintptr_t ent = BoneProbeCapturedEntity();
    uintptr_t model = 0;
    if (!ent || !ReadPtr(ent + kHeldItemOff, &model) || !model) {
        // pump plan Task 4 / fix round 1 (M2): before g_gun is cleared - the pump is marked AWAY (cleared only after 150 ms). COST: one compare
        PumpLeave("no model in the right hand (put away / a blip)");
        DbLeave("no model in the right hand (put away / a blip)");
        RvLeave("no model in the right hand (put away / a blip)");   // 2026-11-09: the revolvers / the chainsaw
        g_gun.model = 0;
        g_gun.built = false;
        HandsReset();
        XbowDropLive();   // the crossbow: the take / bolt display off; a carried bolt is KEPT (the bow nock's rule for a blip)
        Publish(false, false);
        return;
    }
    // [LOADBEARING: manual_reload] the held model = (pointer, InventoryItem): the game reuses the model address on a swap (a tester,
    // 2026-11-08) - a gun back at a reused address is classified again, as a switch. COST: two guarded reads per update.
    const uintptr_t modelInv = HeldItemInventoryItem(model);
    const bool sameModel = model == g_gun.model && modelInv == g_gun.modelInv;
    if (sameModel && !g_gun.built) { return; }   // the common case with anything but a built gun
    uintptr_t player = 0, ctrl = 0, ctrl2 = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(playerVis) + kVisPlayerOff, &player) || !player ||
        !ReadPtr(player + kFireCtlOff, &ctrl) || !ctrl) {
        return;
    }
    ReadPtr(player + kFireCtl2Off, &ctrl2);
    if (!sameModel) {
        XbowModelChanged(model);   // crossbow plan Task 5: a bolt carried from another model is forgotten; the take / display drop
        PumpLeave("the held model changed (a switch)");   // pump plan Task 4 / fix round 1 (M2): marked AWAY, cleared after 150 ms
        DbLeave("the held model changed (a switch)");     // the double-barrel: the same rule
        RvLeave("the held model changed (a switch)");     // 2026-11-09: the revolvers / the chainsaw
        HandsReset();
        ClassifyModel(model, player, ctrl, ctrl2);
        g_gun.modelInv = modelInv;   // after ClassifyModel (it resets g_gun)
        if (!g_gun.built) { Publish(false, false); return; }
    }
    const ULONGLONG now = GetTickCount64();
    ItemState* st = StateFor(g_gun.item, now);
    // THE CROSSBOW (crossbow plan Task 5, carry ruling T2-d): its own driver - never TryLatch / DriveSteps / the gun STATE line.
    // COST off a crossbow: one compare.
    if (g_gun.family == reload::Family::Crossbow) {
        g_curPlayer = player;
        const char* drewX = DriveXbow(ctrl, st, leftPos, leftR9, leftCmdPos, leftHolderT, playerModel, lHandId, now);
        GunSoundTick(player);
        XbowPeriodic(st, drewX, now);
        return;
    }
    // THE PUMP SHOTGUN (pump plan Task 4): its own driver - never TryLatch / DriveSteps / the gun STATE line. COST off a pump: one compare.
    if (g_gun.family == reload::Family::Pump) {
        g_curPlayer = player;
        const char* drewP = DrivePump(ctrl, st, playerModel, lHandId, now);
        GunSoundTick(player);
        PumpPeriodic(st, drewP, now);
        return;
    }
    // THE DOUBLE-BARREL (2026-11-07): its own driver. COST off it: one compare.
    if (g_gun.family == reload::Family::DBarrel) {
        g_curPlayer = player;
        const char* drewD = DriveDBarrel(ctrl, st, playerModel, lHandId, leftPos, leftR9, leftAchieved, leftCmdPos, now);
        GunSoundTick(player);
        DbPeriodic(st, drewD, now);
        return;
    }
    // THE REVOLVERS + THE CHAINSAW (2026-11-09, Task 8b-e): their own drivers. COST off them: one compare.
    if (g_gun.family == reload::Family::Break6 || g_gun.family == reload::Family::Gate || g_gun.family == reload::Family::Fuel) {
        g_curPlayer = player;
        if (g_gun.family == reload::Family::Break6) {
            DriveBreak6(ctrl, st, playerModel, lHandId, leftPos, leftR9, leftAchieved, leftCmdPos, now);
        } else if (g_gun.family == reload::Family::Gate) {
            DriveGate(ctrl, st, playerModel, lHandId, leftPos, leftR9, leftAchieved, leftCmdPos, now);
        } else {
            DriveFuel(ctrl, st, playerModel, lHandId, leftPos, leftR9, leftAchieved, leftCmdPos, now);
        }
        GunSoundTick(player);
        return;
    }
    // A gun with no bone_chamber (the Railgun) never latches a chamber rest - it stops latching once its mag's rest is in (rifle plan
    // Task 5; COST: one int compare more).
    if (st->machine.step() == reload::Step::Idle && (!st->haveMagRest || (!g_gun.haveRest && g_gun.chamberId >= 0))) {
        TryLatch(ctrl, st, now);
    }
    g_curPlayer = player;
    const char* drew = DriveSteps(ctrl, st, leftPos, leftR9, leftAchieved, leftCmdPos, leftHolderT, now);
    GunSoundTick(player);   // a step's delayed second sound (the Beretta seat's +0.40 s); empty queue = one compare
    if (!g_gun.saidFirstState || now - g_gun.lastStateMs >= kStateEveryMs) {
        g_stateLineFirst = !g_gun.saidFirstState;
        g_gun.saidFirstState = true;   // first sample unconditional
        g_gun.lastStateMs = now;
        StateLine(player, ctrl, st, leftPos, leftAchieved, drew, leftCmdPos, now);
    }
}

bool ReloadMagInHand(float rel12[12], float drawnW12[12])
{
    if (!ManualReload) { return false; }
    AcquireSRWLockShared(&g_magLock);   // RUN SG3: read on the pad-poll thread too
    const bool on = g_magRelOn;
    if (on) {
        memcpy(rel12, g_magRel, sizeof(g_magRel));
        memcpy(drawnW12, g_magDrawn, sizeof(g_magDrawn));
    }
    ReleaseSRWLockShared(&g_magLock);
    return on;
}

bool ReloadLockOn()
{
    HpFrame f{};
    float t = 0.0f;
    return LockT(&f, &t) && !reload::TwoHandPinAllowed(t);
}

bool ReloadPinRot(const float grip9[9], const float post9[9], float cWorld[9])
{
    HpFrame f{};
    float t = 0.0f;
    if (!LockT(&f, &t)) { return false; }
    float pos[3], r9[9];
    if (!reload::LockedHand(f.part, *f.rack, pos, r9)) { return false; }
    // r9 IS the drawn hand bone's world rotation wanted; StandardCWorld with the identity "hand in frame" turns it into the cWorld
    // whose DRAWN hand (cWorld x grip x post) is r9 (INV-506) - the bow's BowDrawPinRot with the frame = the locked hand itself.
    static const float kI[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float cStd[9], blended[9];
    if (!twohand::StandardCWorld(r9, twohand::Mat3ToQ(kI), grip9, post9, cStd)) { return false; }
    twohand::SlerpM3(cWorld, cStd, t, blended);
    for (int i = 0; i < 9; ++i) { if (!std::isfinite(blended[i])) { return false; } }
    for (int i = 0; i < 9; ++i) { cWorld[i] = blended[i]; }
    InterlockedIncrement(&g_cntLockRot);
    return true;
}

bool ReloadPinPos(float handPos[3], float* t)
{
    HpFrame f{};
    float tt = 0.0f;
    if (!LockT(&f, &tt)) { return false; }
    float pos[3], r9[9];
    if (!reload::LockedHand(f.part, *f.rack, pos, r9)) { return false; }
    for (int k = 0; k < 3; ++k) { handPos[k] = pos[k]; }
    *t = tt;
    InterlockedIncrement(&g_cntLockPos);
    return true;
}

bool ReloadFingerPose(float* t, int* rowIndex)
{
    if (!ManualReload || !g_hpAny) { return false; }
    const LONG64 now = static_cast<LONG64>(GetTickCount64());
    HpFrame f{};
    if (!HpRead(&f, now) || !f.finger) { return false; }
    const float tt = reload::HandBlendT(f.fingerOn != 0, static_cast<long long>(now - f.fingerTick));
    if (!(tt > 0.0f)) { return false; }
    // The row's index in THIS file's copy of the tables (racks, then mags, then pulls) - the caller indexes its own copy.
    int ri = -1;
    if (f.finger >= reload::kRackRows && f.finger < reload::kRackRows + reload::kRackRowsCount) {
        ri = static_cast<int>(f.finger - reload::kRackRows);
    } else if (f.finger >= reload::kMagRows && f.finger < reload::kMagRows + reload::kMagRowsCount) {
        ri = reload::kRackRowsCount + static_cast<int>(f.finger - reload::kMagRows);
    } else if (f.finger >= reload::kPullRows && f.finger < reload::kPullRows + reload::kPullRowsCount) {
        ri = reload::kRackRowsCount + reload::kMagRowsCount + static_cast<int>(f.finger - reload::kPullRows);
    } else if (f.finger >= reload::kCarryRows && f.finger < reload::kCarryRows + reload::kCarryRowsCount) {
        // crossbow plan Task 5: the bolt fist (boneblend.cpp RlfRow indexes racks, mags, pulls, then carries - the same order)
        ri = reload::kRackRowsCount + reload::kMagRowsCount + reload::kPullRowsCount + static_cast<int>(f.finger - reload::kCarryRows);
    }
    if (ri < 0) {
        InterlockedIncrement(&g_cntFingerNoRow);
        return false;
    }
    *t = tt;
    *rowIndex = ri;
    return true;
}

void ReloadNoteFingerPass(int pinsWritten)
{
    InterlockedIncrement(&g_cntFingerPasses);
    InterlockedExchangeAdd(&g_cntFingerPins, pinsWritten);
}

void ReloadPadPoll(bool vrGripOn, bool& withholdL1)
{
    withholdL1 = false;
    if (!ManualReload) { return; }
    g_vrGripSeen = vrGripOn ? 1 : 0;   // the crossbow's ARMED / TAKE OFF lines name vr_grip=0 (carry ruling T4 M4). One store.
    float sq = 0.0f;
    const bool down = vrGripOn && GetControllerSqueeze(kLeftHand, sq) && sq >= kGripThreshold;
    InterlockedExchange(&g_gripDown, down ? 1 : 0);
    const bool fresh = static_cast<LONG64>(GetTickCount64()) - g_publishMs <= static_cast<LONG64>(kPublishFreshMs);
    if (down && !g_padPressWas) { g_padPressOwned = fresh && g_ownsLeft != 0; }   // decided once, at the press edge
    if (!down) { g_padPressOwned = false; }
    g_padPressWas = down;
    // [LOADBEARING: manual reload L1 withhold] (manual_reload) a grab of the mag / slide is the left GRIP, which is also the game's
    //      L1 - withheld for the whole press, so the grab does not also fire the game's left-hand action.
    withholdL1 = g_padPressOwned;
}

void ReloadOffTick()
{
    // manual_reload is 0 (boneblend.cpp calls this instead of ReloadTick). Nothing of the crossbow / pump live = ~11 loads.
    const bool pumpLive = g_pumpSilencedSet || g_pumpAwayMs || PumpShellAny();
    // THE DOUBLE-BARREL (2026-11-07): the finger word drops, the away record clears, and the hinge / shells we moved go back - only on
    // the model the right hand still holds (never a write to one that may be gone); its next driven update is a fresh draw.
    const bool dbLive = g_dbAnimWord != 0 || g_dbAwayMs || g_db.hingeOff || g_db.moved[0] || g_db.moved[1];
    if (dbLive) {
        InterlockedExchange64(&g_dbAnimWord, 0);
        g_dbAwayItem = 0;
        g_dbAwayMs = 0;
        uintptr_t dm = 0;
        const uintptr_t de = BoneProbeCapturedEntity();
        if (de && ReadPtr(de + kHeldItemOff, &dm) && dm && dm == g_dbLastModel && dm == g_gun.model) {
            DbRestore(dm, "manual_reload switched off");
        }
        g_db.fresh = true;
        if (!g_pumpSilencedSet) { InterlockedExchange(&g_pumpWantCtl, 0); }
    }
    // Review N1 (2026-11-10; Jay: "Fix N1"): THE REVOLVERS + THE CHAINSAW - the held gun's moved parts (gate, hammer, cylinder, round,
    // bottle, T-handle, the Schofield's barrel + chambers) go back at rest, from its own record or a parked one, only while the right hand
    // holds THAT gun (model + InventoryItem - never a write to a model that may be gone); a parked gun not held waits for its next draw.
    // COST while nothing is moved or parked: a few compares; else one entity read + two guarded reads per update.
    if (RvMoved(g_rv) || g_nRvPark > 0) {
        uintptr_t rm = 0;
        const uintptr_t re = BoneProbeCapturedEntity();
        if (re && ReadPtr(re + kHeldItemOff, &rm) && rm) {
            const uintptr_t ri = HeldItemInventoryItem(rm);
            const bool cur = rm == g_rv.model && ri == g_rv.inv;
            const int pk = cur ? -1 : RvParkFind(rm, ri);
            RvDraw* rec = cur ? &g_rv : pk >= 0 ? &g_rvPark[pk] : nullptr;
            if (rec && RvMoved(*rec)) {
                RvRestore(rm, *rec);
                static bool s_failSaid = false;   // a failed write retries every update: its line once (D-339 - the cap is for events)
                const bool failed = RvMoved(*rec);
                if ((!failed || !s_failSaid) && RvLine()) {
                    s_failSaid = s_failSaid || failed;
                    // [PROBE: REVOLVER] gate=manual_reload  site=FLAT  log-only=yes
                    LOG("reload: [REVOLVER] manual_reload switched off - model %p (gun %d, %s): its parts back at rest%s.",
                        reinterpret_cast<void*>(rm), static_cast<int>(rec->gun), cur ? "the held gun" : "a parked gun",
                        RvMoved(*rec) ? " - *** SOME NOT WRITTEN (still flagged, tried again next update) ***" : "");
                }
            }
            if (cur) {
                g_rv.fresh = true;   // its next driven update is a fresh draw (the double-barrel's rule)
                g_rv.snap = RvSnap{};
                if (!g_pumpSilencedSet) { InterlockedExchange(&g_pumpWantCtl, 0); }
            }
            if (pk >= 0 && !RvMoved(g_rvPark[pk])) { RvParkDrop(pk); }
        }
    }
    if (!g_xbCarry.active && !g_xbTakeLiveSet && !g_xbowShowVis && !g_xbHandFree && !g_xbBoltMovedModel && !pumpLive) { return; }
    // THE PUMP (pump plan Task 4): cleared at once (no away guard - manual_reload is off): the hand empty, the game's rack sound back,
    // nothing published; a shell we left at the hand goes home only while the right hand still holds THAT pump (ruling R3, per model - fix
    // round 1 M1; never a write to a model that may be gone; otherwise its record waits for that model's next draw).
    // COST while a shell record is kept and that pump is not held: one entity read + a few compares per update.
    if (pumpLive) {
        if (g_pumpSilencedSet) { PumpLeave("manual_reload switched off"); }
        if (g_pumpAwayMs || g_pumpSilencedSet) { PumpClearAway(); }
        uintptr_t pm = 0;
        const uintptr_t pe = BoneProbeCapturedEntity();
        if (pe && ReadPtr(pe + kHeldItemOff, &pm) && pm && pm == g_gun.model && PumpShellFor(pm)) {
            PumpRestoreShell(pm, "manual_reload switched off");
        }
        if (!g_xbCarry.active && !g_xbTakeLiveSet && !g_xbowShowVis && !g_xbHandFree && !g_xbBoltMovedModel) { return; }
    }
    XbowDropLive();
    uintptr_t model = 0;
    const uintptr_t ent = BoneProbeCapturedEntity();
    const bool held = ent && ReadPtr(ent + kHeldItemOff, &model) && model;
    if (g_xbCarry.active) {
        // Home only while the right hand still holds the bolt's own crossbow - else forget it (never a call on a model that may be gone).
        if (held && reinterpret_cast<void*>(model) == g_xbCarry.homeModel) {
            HandCarryGiveBack(g_xbCarry, "manual_reload switched off");
        } else {
            HandCarryForget(g_xbCarry, "manual_reload switched off and the crossbow is not held");
        }
    }
    // Fix round 1 ruling T5-b: bolt_pose (the aim point) back at rest - the game reloads and fires this crossbow from here on. Only while
    // the right hand still holds that model (never a write to one that may be gone); otherwise it stays flagged for its next draw.
    if (held && g_xbBoltMovedModel == model && model == g_gun.model) { XbowRestoreBolt(model, "manual_reload switched off"); }
}

bool ReloadPumpAnim(int* cycle, float* frame)
{
    if (!ManualReload) { return false; }
    const LONG64 w = g_pumpAnimWord;   // one volatile load
    if (!w) { return false; }
    return reload::PumpAnimUnpack(w, PumpClockQ(), reload::kPumpCycleS, cycle, frame);   // PS2 smoothing: advanced to now
}

bool ReloadPumpStroke(float* m, float axisHolderl[3])
{
    if (!ManualReload) { return false; }
    AcquireSRWLockShared(&g_pumpLock);
    const bool on = g_pumpStrokeOn && GetTickCount64() - g_pumpStrokeMs <= kPumpPublishFreshMs;
    if (on) {
        *m = g_pumpStrokeM;
        for (int k = 0; k < 3; ++k) { axisHolderl[k] = g_pumpAxisHl[k]; }
    }
    ReleaseSRWLockShared(&g_pumpLock);
    return on;
}

void ReloadNoteController(bool isLeft, const float gripTrk[3])
{
    // [LOADBEARING: manual reload pump] fix round 1 (C1) + final fix wave (I1, R17): the rack stroke's ONLY source - each controller's grip
    // point in tracking space, before the two-hand pin moves the wrist target. COST: off a pump one bool test + one volatile load; driven
    // one SRW acquire + a 12-byte copy.
    if (!ManualReload || !g_pumpWantCtl) { return; }
    for (int k = 0; k < 3; ++k) { if (!std::isfinite(gripTrk[k])) { return; } }
    const int s = isLeft ? 1 : 0;
    AcquireSRWLockExclusive(&g_lcLock);
    memcpy(g_lcPos[s], gripTrk, sizeof(g_lcPos[s]));
    g_lcMs[s] = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lcLock);
}

void ReloadNotePumpFingerPass(int pinsWritten)
{
    InterlockedIncrement64(&g_pumpFingerPasses);
    InterlockedExchangeAdd64(&g_pumpFingerPins, pinsWritten);
}

bool ReloadDbAnim(int* anim, float* frame)
{
    if (!ManualReload) { return false; }
    const LONG64 w = g_dbAnimWord;   // one volatile load
    if (!w) { return false; }
    reload::DbAnim a = reload::DbAnim::None;
    float f = 1.0f;
    if (!reload::DbAnimUnpack(w, PumpClockQ(), &a, &f)) { return false; }
    *anim = static_cast<int>(a);
    *frame = f;
    return true;
}

void ReloadNoteDbFingerPass(int pinsWritten)
{
    InterlockedIncrement64(&g_dbFingerPasses);
    InterlockedExchangeAdd64(&g_dbFingerPins, pinsWritten);
}

bool ReloadXbowHandFree()
{
    // carry ruling T2 M-4: the carried / laid bolt is exempt from the solid-gun stop, as a grip on the gun is.
    return ManualReload && g_xbHandFree != 0;
}

bool ReloadWithholdsL1()
{
    if (!ManualReload) { return false; }
    // [LOADBEARING: crossbow L1 withhold] (manual_reload) Jay after RUN XB1: while the crossbow is not loaded the left grip is never
    //      the game's L1 (an accidental press near the quiver fired the game's left-hand action). Pad thread; COST: two loads + a tick read.
    const bool fresh = static_cast<LONG64>(GetTickCount64()) - g_xbL1Ms <= static_cast<LONG64>(kPublishFreshMs);
    return fresh && g_xbL1Block != 0;
}

bool ReloadWithholdsR2()
{
    if (!ManualReload) { return false; }
    // [LOADBEARING: manual reload R2 withhold] (manual_reload) the gun does not fire until the reload is complete (spec goal 8).
    const bool fresh = static_cast<LONG64>(GetTickCount64()) - g_publishMs <= static_cast<LONG64>(kPublishFreshMs);
    return fresh && g_withholdR2 != 0;
}

}  // namespace dlvr
