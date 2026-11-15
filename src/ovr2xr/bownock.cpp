// The bow's MANUAL NOCK (Jay 2026-10-17) - docs/superpowers/specs/2026-10-17-bow-manual-nock-design.md, plan
// docs/superpowers/plans/2026-10-17-bow-manual-nock.md Task 3. With a bow out the bow never reloads itself: RT at the right
// shoulder takes an arrow, the right hand carries it, a forgiving zone catches it onto the bow's line (pointing the right way),
// the hand pushes it to the string and it seats; the existing manual draw (bow_manual_draw) then draws it.
//
// THE GAME SIDE (INV-589, static, our gamedll 97046bba): with CanShoot false the bow controller's update FORCES state -1
// (Empty) when the ammo count +D9BF60 reads 0. One byte-verified detour on +D9BF60 returns 0 for OUR bow while it is empty,
// so the game holds Empty by its own rule and hides the arrow by its own predicate. Taking an arrow stops the zero: the game
// enters 6 (PutArrow) by itself, which renders the arrow; we hold 6 by re-stamping its start time ctrl+72 while the hand
// carries arrow_pose, and seating lets 6 end on its own timer (6 -> 0 has no other test). An arrow is consumed only at the
// shot (case 4), so a take / early release / cancel costs nothing.
//
// THREADS. BowNockTick runs on the game thread (BowDrawTick, the player-update tail). The detour runs wherever the game calls
// +D9BF60: the bow's update and CanShoot (nk1: those two callers only; CanShoot itself is also called by the input handler
// and one HUD function, +550FC0). It reads two volatile words written only by the tick.
#include "bownock.h"

#include "boneblend.h"      // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "controllers.h"    // PulseController, GetControllerGripPose
#include "headpose.h"       // GetHeadPose
#include "holster.h"        // HolsterCenterX/Y/Z, HolsterRadius - the right-shoulder zone
#include "holster_logic.h"  // holster::LocalOffset, holster::kExitScale
#include "handcarry.h"      // the carried arrow rides the rendered r_hand (spec 2026-10-22-hand-carried-model-design)
#include "handcarry_logic.h"
#include "nock_logic.h"
#include "boltwheel_logic.h"   // BoltHoldStep / WantFresh - the arrow wheel's quiver hold (same rule as the bolt holster)

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/buildrva.h"   // the bow clock's EGL rva on the Epic build (tier 2); the ammo detour moves itself
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - diag_periodic_logs (the session-totals line only)

namespace dlvr {

// [DEP: bow_manual_nock] required-for=the bow's MANUAL NOCK (BowManualNock; spec 2026-10-17-bow-manual-nock-design, Jay
//      2026-10-17 "Approved, start the reads" / "Native approach, go ahead"): the bow does not reload itself; each arrow is taken
//      from the right shoulder with RT, carried, caught onto the bow's line (direction-checked), pushed to the string and seated.
//      Needs bow_manual_draw (BowDrawTick calls it, with the bow's matrices and the drawn right hand: diag_bone_dump + the three
//      master enables, the rig solving the right arm), and fire_reticle_3d or grapple_reticle_3d (the controller walk).
//      tree=INERT at 0: BowNockTick returns at its first test and the +D9BF60 detour is never installed (stock bytes).
//      DEFAULT ON 2026-10-17 - TIER A (Jay, after runs 1-10: "Tier A, ship. Hide the adjusted values aswell, they are not to be
//      touched"; was OFF / Tier C - "Off until verified"). LOCKED ON the same day (S_BOOL_LOCKED - "Yes, hide & lock it too"): not
//      drawn, the file value never read. The carried-arrow offset is DEFERRED (D-238 (2)). HEADSET.
bool  BowManualNock = true;
// [DEP: bow_nock_catch_m] required-for=bow_manual_nock's catch radius round the bow's line (BowNockCatchM); Jay: "extremely
//      lenient", "a forgiving catch". tree=PROBE-ONLY (tuning, Tier D). 0.20 -> 0.10 2026-10-17 after bownock_run1 (Jay: "a little
//      'too generous' ... so it kind of takes over itself" - every catch at 0.18-0.20 m, the zone's edge; then "We'll fix the values
//      so it's less generous first"; 0.10 = Claude's proposal, not objected to).
float BowNockCatchM = 0.10f;
// [DEP: bow_nock_hold_m] required-for=bow_manual_nock: how far off the line a caught arrow may stray before it is back in the
//      hand (BowNockHoldM). tree=PROBE-ONLY (tuning, Tier D). 0.25 -> 0.15 2026-10-17 (the same change).
float BowNockHoldM = 0.15f;
// [DEP: bow_nock_seat_m] required-for=bow_manual_nock: the notch within this of the string seats (BowNockSeatM).
//      tree=PROBE-ONLY (tuning, Tier D). 0.08 -> 0.04 2026-10-17 (the same change).
float BowNockSeatM = 0.04f;
// [DEP: bow_nock_catch_deg] required-for=bow_manual_nock's direction test (BowNockCatchDeg); Jay: "The catch should check the
//      arrows direction however, we don't want to allow someone to just place an arrow backwards & have it work".
//      tree=PROBE-ONLY (tuning, Tier D; a reversed arrow is 180). 60 -> 35 2026-10-17 (the same change).
float BowNockCatchDeg = 35.0f;
// [DEP: bow_nock_catch_len_m] required-for=bow_manual_nock: the latch onto the firing path reaches at most this far in front of
//      the string (BowNockCatchLenM; was the constant kNockCatchLenM 0.75). Jay 2026-10-17: "set a maximum distance for the
//      firing path for the arrow to latch to" -> "set it to 0.3m". tree=PROBE-ONLY (tuning, Tier D).
//      ALL FIVE bow_nock_* values above are LOCKED from 2026-10-17 (S_FLOAT_LOCKED in ovr2xr.cpp: not drawn, file value never read):
//      Jay, "Hide the adjusted values aswell, they are not to be touched" - Tier A-LOCKED in docs/TIERS.md.
//      ALSO READ by the CROSSBOW manual reload (manual_reload; plan 2026-11-03-manual-reload-crossbow Task 5, reload.cpp DriveXbow -
//      spec 4.5 "the bow's locked bow_nock_* values as the start", no new key): catch_m = the lay radius round the groove line,
//      catch_deg = the bolt's direction limit, hold_m = how far off the line a laid bolt may stray, seat_m = the seat distance,
//      catch_len_m = the groove line's length forward of the seat. A change here changes the crossbow's lay / seat too.
float BowNockCatchLenM = 0.30f;
// [DEP: bow_nock_carry_back_m] required-for=bow_manual_nock's CARRIED arrow (BowNockCarryBackM; spec
//      2026-10-22-hand-carried-model-design 5): metres the arrow carried from the quiver sits further back along its own axis than
//      the game's reload pose (m_fpp_bow_stand_fastshot frame 132 holds it ahead of every finger joint, INV-613 A). Jay 2026-10-22:
//      "Tune". 0.05 = Claude's starting value (the nock ~2 cm behind the curled fingertips); range 0.00 - 0.15. Visible, NOT locked.
//      tree=PROBE-ONLY (tuning, Tier D). Applied live (HandCarrySetLocal on a change).
//      ALL FOUR bow_nock_carry_* values LOCKED 2026-10-22 at Jay's tuning (runs 1-3: back 0.048, tilt -12, turn 0, side -0.015;
//      Jay: "I want those values locked") - S_FLOAT_LOCKED in ovr2xr.cpp: not drawn, the file value never read. Tier A-LOCKED.
float BowNockCarryBackM = 0.048f;
// [DEP: bow_nock_carry_tilt_deg] required-for=bow_manual_nock's CARRIED arrow (BowNockCarryTiltDeg): degrees the carried arrow is
//      tilted about the right hand's Y axis (across the hand), about the nock. Jay 2026-10-22 after handcarry_run1: "The Angle & it
//      needs to sit further between the fingers horizontally" -> "Tune in the headset". 0 = the clip's angle (no change); range
//      -60..60. Visible, NOT locked. tree=PROBE-ONLY (tuning, Tier D). Applied live (HandCarrySetLocal on a change).
float BowNockCarryTiltDeg = -12.0f;   // LOCKED 2026-10-22 (Jay's tuning)
// [DEP: bow_nock_carry_turn_deg] required-for=bow_manual_nock's CARRIED arrow (BowNockCarryTurnDeg): degrees the carried arrow is
//      turned about the right hand's Z axis, about the nock (the same ruling). 0 = no change; range -60..60. Visible, NOT locked.
//      tree=PROBE-ONLY (tuning, Tier D). Applied live.
float BowNockCarryTurnDeg = 0.0f;     // LOCKED 2026-10-22 (Jay's tuning)
// [DEP: bow_nock_carry_side_m] required-for=bow_manual_nock's CARRIED arrow (BowNockCarrySideM): metres the carried arrow is slid
//      HORIZONTALLY - along Y x shaft, perpendicular to the knuckle line and the shaft (Jay 2026-10-22 run 2: the first build slid it
//      along the knuckle line, "That was vertical, not horizontal" -> "1": the same key re-aimed). 0 = no change; range -0.05..0.05.
//      Visible, NOT locked. tree=PROBE-ONLY (tuning, Tier D). Applied live.
float BowNockCarrySideM = -0.015f;    // LOCKED 2026-10-22 (Jay's tuning)

namespace {

#include "bow_nock_hand.inc"   // kNockArrowInHandR/T, kNockTipInArrow, kNockRestInRoot - tools/bow_nock_hand.py
#include "bow_stand_pose.inc"  // kStandN / kStandNames / kStandParent / kStandLocal - tools/bow_stand_pose.py

// r_handholder's rest offset in r_hand (the same figure kBdStringInHand in boneblend.cpp is built from): BowDrawRightHand gives
// the achieved HANDHOLDER position and the hand bone's basis, so r_hand's origin = achieved - R x this.
const float kNockHandholderInHand[3] = { 0.100f, 0.0f, -0.025f };

constexpr uintptr_t kAmmoRva = 0xD9BF60;  // WeaponBowController's equipped-arrow count (INV-589 c)
const unsigned char kAmmoBytes[15] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x50, 0x28 };
constexpr uintptr_t kGameTimeRva = 0xCE56E0;   // the controller's vtable slot 6 = IGSObject::GetTime of its player (bow2 :1610)

typedef int (*AmmoFn)(void* ctrl);
typedef float (*TimeFn)(void* ctrl);
typedef void (*SetWorldFn)(void* model, int index, const float* m12);

AmmoFn          g_trueAmmo = nullptr;
volatile LONG   g_nkLive = 0;       // the detour is committed
volatile LONG   g_nkFake = 0;       // 1 = return 0 for g_nkFakeCtrl
void* volatile  g_nkFakeCtrl = nullptr;
volatile LONG   g_nkFaked = 0;      // calls answered 0 (the session-totals line)

// The detour. COST: one volatile load, one pointer compare and two bool loads per call (the bow's update and CanShoot - ~10
// calls per bow update), + one interlocked increment per call answered 0. Both switches are tested HERE too, so turning either
// off releases the bow at once even though BowDrawTick (and with it the tick) stops running when bow_manual_draw is off.
int NockAmmoCount(void* ctrl)
{
    if (nock::ShouldFake(g_nkFake != 0, ctrl == g_nkFakeCtrl, BowManualNock, BowManualDraw)) {
        InterlockedIncrement(&g_nkFaked);
        return 0;
    }
    return g_trueAmmo(ctrl);
}

uintptr_t Gd()
{
    static uintptr_t s = 0;
    if (!s) { s = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll")); }
    return s;
}

// The controller's own clock (vtable slot 6), accepted only if it is the function this was written against.
// kGameTimeRva as the loaded build has it (common/buildrva.h), once. Its own function: TimeFnOf uses __try.
uintptr_t GameTimeRva()
{
    static const uintptr_t s = ::dlvr::buildrva::Gd(kGameTimeRva);
    return s;
}

TimeFn TimeFnOf(uintptr_t ctrl)
{
    const uintptr_t timeRva = GameTimeRva();
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
        const uintptr_t fn = vt ? *reinterpret_cast<const uintptr_t*>(vt + 6 * 8) : 0;
        return (Gd() && fn == Gd() + timeRva) ? reinterpret_cast<TimeFn>(fn) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool CallTime(TimeFn fn, uintptr_t ctrl, float* out)
{
    __try { *out = fn(reinterpret_cast<void*>(ctrl)); return std::isfinite(*out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadByte(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadFloat(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteFloat(uintptr_t a, float v)
{
    __try { *reinterpret_cast<float*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallAmmo(uintptr_t ctrl, int* out)
{
    __try { *out = g_trueAmmo(reinterpret_cast<void*>(ctrl)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallSetWorld(SetWorldFn fn, uintptr_t item, int id, const float* m)
{
    __try { fn(reinterpret_cast<void*>(item), id, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 3x4 row-major, basis in the columns.
void Mul34(const float* a, const float* b, float* o)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
            if (c == 3) { s += a[r * 4 + 3]; }
            o[r * 4 + c] = s;
        }
    }
}

bool Finite(const float* m, int n)
{
    for (int k = 0; k < n; ++k) { if (!std::isfinite(m[k]) || !(m[k] > -1.0e6f && m[k] < 1.0e6f)) { return false; } }
    return true;
}

nock::Vec3 Unit(float x, float y, float z)
{
    const float l = std::sqrt(x * x + y * y + z * z);
    return l > 1.0e-9f ? nock::Vec3{ x / l, y / l, z / l } : nock::Vec3{ 0.0f, 0.0f, 0.0f };
}

const char* EvName(nock::Event e)
{
    switch (e) {
    case nock::Event::Take: return "TAKE";
    case nock::Event::TakeNoArrows: return "TAKE REFUSED - no arrows";
    case nock::Event::Catch: return "CATCH";
    case nock::Event::CatchWrongWay: return "NO CATCH - pointing the wrong way";
    case nock::Event::DropToCarry: return "OFF THE LINE - back in the hand";
    case nock::Event::Seat: return "SEATED";
    case nock::Event::EarlyRelease: return "EARLY RELEASE - arrow back in the quiver";
    case nock::Event::GameMoved: return "THE GAME MOVED THE STATE - arrow back in the quiver";
    case nock::Event::Shot: return "SHOT - the bow is empty";
    case nock::Event::Disabled: return "OFF";
    default: return "none";
    }
}

const char* PhaseName(nock::Phase p)
{
    switch (p) {
    case nock::Phase::Off: return "off";
    case nock::Phase::Empty: return "empty";
    case nock::Phase::Carry: return "carry";
    case nock::Phase::Caught: return "caught";
    case nock::Phase::Seated: return "seated";
    }
    return "?";
}

nock::Memory g_mem;
nock::Machine g_m(false);
uintptr_t g_item = 0;
uintptr_t g_key = 0;             // the memory key of the current bow (nock::MemoryKey: its controller)
bool g_refused = false;          // the detour refused - the feature is off for the session
bool g_saidDrawOff = false, g_saidTime = false;
long g_lastSeq = 0;
bool g_haveSeq = false;
bool g_inShoulder = false;
bool g_qArmed = false;           // the quiver hold for the arrow wheel (BoltHoldStep's "armed") - game thread only
volatile LONG   g_qHold = 0;     // ... published for the pad thread (BowNockQuiverHold) ...
volatile LONG64 g_qStampMs = 0;  // ... with its stamp
long g_n[11] = {};               // event counts, indexed by nock::Event
ULONGLONG g_lastTotals = 0;
// The carried arrow: the game's own arrow object (BowVis+1480, INV-402) on the rendered r_hand (handcarry.h). Never dropped by
// BowNockNoBow (the bow may only be unreadable for an update, e.g. a menu): a carry is forgotten when the live bow or its arrow
// pointer CHANGES (handcarry::Decide's sameObject), and never called on after that.
HandCarry g_carry;
constexpr uintptr_t kBowArrowObjOffset = 1480;   // BowVis+1480 = the nocked-arrow ModelObject (INV-402 / 403)
long g_carryTickSeq = 0, g_carryWatchSeq = 0;    // BowNockTick runs / BowNockCarryWatch's last look (final review (1))

void Install()
{
    static bool s_tried = false;
    if (s_tried) { return; }
    s_tried = true;
    if (!Gd()) {
        g_refused = true;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] REFUSED - gamedll is not mapped; the bow reloads itself (bow_manual_nock=1).");
        return;
    }
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(Gd() + kAmmoRva, kAmmoBytes, sizeof(kAmmoBytes), reinterpret_cast<void*>(&NockAmmoCount),
                                            reinterpret_cast<void**>(&g_trueAmmo), "bow ammo count", &attempts);
    if (ok) {
        InterlockedExchange(&g_nkLive, 1);
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] ARMED (logged unconditionally) - detour gamedll+0x%zX LIVE (%zu bytes stolen, %d commit attempt(s)) | "
            "catch %.3f m within %.1f deg, hold %.3f m, seat %.3f m, capsule %.2f m | the bow now waits for an arrow from the right "
            "shoulder (RT), shoulder zone (%.3f %.3f %.3f) r %.3f.", static_cast<size_t>(kAmmoRva), sizeof(kAmmoBytes), attempts,
            BowNockCatchM, BowNockCatchDeg, BowNockHoldM, BowNockSeatM, BowNockCatchLenM, HolsterCenterX, HolsterCenterY,
            HolsterCenterZ, HolsterRadius);
    } else {
        g_refused = true;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] REFUSED - %s gamedll+0x%zX (%d commit attempt(s)); the bow reloads itself for this session.",
            attempts == 0 ? "the prologue does not match (a different game build?) at" : "the detour could not be committed at",
            static_cast<size_t>(kAmmoRva), attempts);
    }
}

// Jay 2026-10-17 ("Hold the bones ourselves"): while the nock holds the bow in the game's Empty state, its authored Empty clip
// bends the string 74 deg and the limbs 5 deg (INV-589 addendum). Put bone_root, the string and both limb chains back at the
// Stand pose, composed parent-first from the item's Holder, every update (the engine re-drives them each update, INV-542 add. 1).
// Run 2 (Jay: the bow "rotates slightly to the left in the hand" on the take, back on the notch): PutArrow's bone_root is 8.55 deg
// off Stand too, so the same hold covers the carry, the catch and the game's 1.0 s PutArrow -> Stand blend after the seat; the
// catch line is taken from the HELD root so the slide follows the bow as drawn.
// COST while the nock is on: 1 world read + 10 3x4 multiplies per update (ids cached per item); + 10 world writes while held.
struct RestHold { uintptr_t item = 0; uintptr_t inv = 0; int holder = -1; int ids[kStandN] = {}; bool ok = false; long held = 0; bool said = false; };
RestHold g_rh;

// The Stand pose as world matrices this update, parent-first from the item's Holder. False = not available (named once).
bool ComputeHeld(uintptr_t item, float w[kStandN][12])
{
    // [LOADBEARING: bow_manual_nock] ids per (pointer, InventoryItem): the game reuses the model address on a swap (a tester, 2026-11-08).
    // COST: two guarded reads per call.
    const uintptr_t inv = HeldItemInventoryItem(item);
    if (item != g_rh.item || inv != g_rh.inv) {
        g_rh = RestHold{};
        g_rh.item = item;
        g_rh.inv = inv;
        g_rh.holder = BowItemElemId(item, "Holder");
        int missing = g_rh.holder < 0 ? 1 : 0;
        for (int i = 0; i < kStandN; ++i) {
            g_rh.ids[i] = BowItemElemId(item, kStandNames[i]);
            if (g_rh.ids[i] < 0) { ++missing; }
        }
        g_rh.ok = missing == 0;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] EMPTY POSE for bow %p: %s (Holder %d, %d bones, %d missing).", reinterpret_cast<void*>(item),
            g_rh.ok ? "the string, limbs and root are held at the Stand pose while empty, carrying and seating"
                    : "*** NOT HELD - an element is missing; the game's Empty clip shows ***", g_rh.holder, kStandN, missing);
    }
    if (!g_rh.ok) { return false; }
    float holderW[12];
    if (!BowItemElemWorld(item, g_rh.holder, holderW)) {
        if (!g_rh.said) {
            g_rh.said = true;
            // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
            LOG("bownock: [BOWNOCK] EMPTY POSE *** NOT HELD this update *** - the Holder's world read failed or is degenerate.");
        }
        return false;
    }
    for (int i = 0; i < kStandN; ++i) {
        const float* parent = kStandParent[i] < 0 ? holderW : w[kStandParent[i]];
        Mul34(parent, kStandLocal[i], w[i]);
        if (!Finite(w[i], 12)) { return false; }   // nothing half-written: every matrix is checked before any write
    }
    return true;
}

void WriteHeld(uintptr_t item, SetWorldFn setW, const float w[kStandN][12])
{
    if (!setW) { return; }
    for (int i = 0; i < kStandN; ++i) { CallSetWorld(setW, item, g_rh.ids[i], w[i]); }
    if (g_rh.held++ == 0) {
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] EMPTY POSE held (first update, logged once per bow) - %d bones written at the Stand pose.", kStandN);
    }
}

void SetFake(uintptr_t ctrl, bool on)
{
    g_nkFakeCtrl = reinterpret_cast<void*>(ctrl);
    InterlockedExchange(&g_nkFake, on ? 1 : 0);
}

// The right hand at the right shoulder, the holster's zone and its 1.2x exit (swing.cpp's pattern) - independent of holster_enable.
// The zone follows the body when the holster body-lock has it latched (HolsterZoneLocal), else the head as before.
bool HandInShoulder()
{
    XrPosef head{}, grip{};
    if (!GetHeadPose(head) || !GetControllerGripPose(kRightHand, grip)) { g_inShoulder = false; return false; }
    const float hp[3] = { head.position.x, head.position.y, head.position.z };
    const float hq[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
    const float gp[3] = { grip.position.x, grip.position.y, grip.position.z };
    float lv[3];
    HolsterZoneLocal(kHolsterShoulder, kHolsterCallerBow, hp, hq, gp, lv);
    const float dx = lv[0] - HolsterCenterX, dy = lv[1] - HolsterCenterY, dz = lv[2] - HolsterCenterZ;
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float exitR = HolsterRadius * holster::kExitScale;
    if (g_inShoulder) { if (d2 > exitR * exitR) { g_inShoulder = false; } }
    else if (d2 < HolsterRadius * HolsterRadius) { g_inShoulder = true; }
    return g_inShoulder;
}

}  // namespace

namespace {
int g_lastState = -99;   // the bow controller's state at the last tick (the self-check line's label only)
// The arrow-holding finger pose (Jay 2026-10-17: "Use the game's reload pose, build it"): on while carrying / caught, blended 100 ms
// each way like the draw's (BowDrawFingerT). Written by the tick (game thread), read by BowFingersApply on the pin pass.
volatile LONG   g_nkFingerOn = 0;
volatile LONG64 g_nkFingerTick = 0;
}  // namespace

bool BowNockFingerT(float* t)
{
    if (!BowManualNock) { return false; }   // COST off: one bool test
    const LONG64 since = static_cast<LONG64>(GetTickCount64()) - g_nkFingerTick;
    const float u = since <= 0 ? 0.0f : (since >= 100 ? 1.0f : static_cast<float>(since) / 100.0f);
    const float v = g_nkFingerOn ? u : 1.0f - u;
    if (g_nkFingerTick == 0 || v <= 0.0f) { return false; }
    *t = v;
    return true;
}

bool BowNockFixedRel(uintptr_t item, bool haveLive, float relR[9], float relT[3])
{
    // Is this item a bow? (BowDrawTick's own test: bone_root + arrow_pose + bone_string resolve.) Cached per item pointer.
    static uintptr_t s_item = 0;
    static uintptr_t s_inv = 0;   // s_item's InventoryItem - a reused model address is another item (a tester, 2026-11-08)
    static bool s_isBow = false;
    static unsigned s_seen = 0;   // bit (state + 1): the self-check line printed for that state
    static long s_used = 0;
    if (!BowManualNock) { return false; }   // COST off: one bool test
    const uintptr_t inv = HeldItemInventoryItem(item);   // COST: two guarded reads
    if (item != s_item || inv != s_inv) {
        s_item = item;
        s_inv = inv;
        s_isBow = BowItemElemId(item, "bone_root") >= 0 && BowItemElemId(item, "arrow_pose") >= 0
                  && BowItemElemId(item, "bone_string") >= 0;
    }
    if (!nock::UseFixedBowRel(BowManualNock, BowManualDraw, g_nkLive != 0 && !g_refused, true, s_isBow)) { return false; }
    // THE SELF-CHECK (a positive control on the convention, pre-registered 2026-10-17): the LIVE rel against the fixed one, once
    // per bow state - idle (0) should read ~7.5 deg / ~0.015 m, the draw (1 / 3) and PutArrow (6) ~0 (INV-590's table). A transposed
    // matrix would not read ~0 in the draw.
    if (haveLive) {
        const int st = g_lastState;
        const unsigned bit = (st >= -1 && st <= 6) ? (1u << (st + 1)) : (1u << 9);
        if (!(s_seen & bit)) {
            s_seen |= bit;
            float tr = 0.0f;
            for (int r = 0; r < 3; ++r) { for (int k = 0; k < 3; ++k) { tr += relR[r * 3 + k] * kBowFixedRelR[r * 3 + k]; } }
            const float c = (tr - 1.0f) * 0.5f;
            const float deg = std::acos(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c)) * 57.29578f;
            const float dm = std::sqrt((relT[0] - kBowFixedRelT[0]) * (relT[0] - kBowFixedRelT[0])
                                       + (relT[1] - kBowFixedRelT[1]) * (relT[1] - kBowFixedRelT[1])
                                       + (relT[2] - kBowFixedRelT[2]) * (relT[2] - kBowFixedRelT[2]));
            // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
            LOG("bownock: [BOWNOCK] FIXED REL - state %d: the live grip rel is %.2f deg / %.4f m from the fixed (draw) rel | expected "
                "idle (0) ~7.5 deg / ~0.015 m, draw (1/3) and PutArrow (6) ~0 (INV-590) | bow %p, fixed rel used %ld times so far.",
                st, deg, dm, reinterpret_cast<void*>(item), s_used);
        }
    }
    for (int k = 0; k < 9; ++k) { relR[k] = kBowFixedRelR[k]; }
    for (int k = 0; k < 3; ++k) { relT[k] = kBowFixedRelT[k]; }
    ++s_used;
    return true;
}

bool BowNockQuiverHold()
{
    return boltwheel::WantFresh(g_qHold != 0, static_cast<unsigned long long>(g_qStampMs), GetTickCount64());
}

void BowNockNoBow()
{
    if (g_qArmed || g_qHold) {   // no bow: no quiver hold (COST: two loads)
        g_qArmed = false;
        InterlockedExchange(&g_qHold, 0);
    }
    if (!BowManualNock && !g_nkFake && g_item == 0) { return; }   // COST off: one bool test (two loads if it was ever on)
    if (g_item) { g_mem.Store(g_key, g_m.phase()); }
    g_item = 0;
    SetFake(0, false);
    if (g_nkFingerOn) {   // no bow: the carried arrow is gone, so are its fingers
        InterlockedExchange(&g_nkFingerOn, 0);
        InterlockedExchange64(&g_nkFingerTick, static_cast<LONG64>(GetTickCount64()));
    }
}

void BowNockCarryWatch(void* ent)
{
    if (!g_carry.active) { return; }   // COST when not carrying: one bool test
    const long seq = g_carryTickSeq;
    const bool missed = handcarry::TickMissed(true, seq, g_carryWatchSeq);
    g_carryWatchSeq = seq;
    if (!missed) { return; }
    // BowNockTick did not run during the last update (bow_manual_draw off, the string probe on, the controller unreadable, no bow):
    // the arrow must not stay on the hand. Home only if the live left item is still its bow with that arrow - else forget (F7).
    uintptr_t item = 0, obj = 0;
    if (ent) { ReadPtr(reinterpret_cast<uintptr_t>(ent) + 0x1028, &item); }
    if (item) { ReadPtr(item + kBowArrowObjOffset, &obj); }
    if (handcarry::SameObject(reinterpret_cast<void*>(item), reinterpret_cast<void*>(obj), g_carry.homeModel, g_carry.obj)) {
        HandCarryGiveBack(g_carry, "the bow's tick stopped");
    } else {
        HandCarryForget(g_carry, "the bow's tick stopped and the left item changed");
    }
}

void BowNockTick(const BowNockCtx& c)
{
    static bool s_everOn = false;   // until the feature has been on once, off is this one test - nothing read, nothing logged
    const bool want = BowManualNock && BowManualDraw;
    if (!want && !s_everOn) { return; }   // COST off: one bool test (BowManualNock false short-circuits)
    if (!want && g_m.phase() == nock::Phase::Off && !g_nkFake) {
        if (BowManualNock && !BowManualDraw && !g_saidDrawOff) {
            g_saidDrawOff = true;
            // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
            LOG("bownock: [BOWNOCK] OFF - bow_manual_nock=1 needs bow_manual_draw=1 (the seated arrow is drawn by it). The bow reloads itself.");
        }
        return;
    }
    if (want) { s_everOn = true; Install(); }
    const bool live = g_nkLive != 0 && !g_refused;

    // A different bow: remember the old one's arrow, start this one from its memory (Jay: "Unless nocked prior").
    // [LOADBEARING: bow_manual_nock] a different bow = another pointer OR another InventoryItem at the same pointer (the game reuses the
    // model address on a swap, a tester 2026-11-08) - the same event as a switch. COST: two guarded reads per tick.
    static uintptr_t s_itemInv = 0;
    const uintptr_t itemInv = HeldItemInventoryItem(c.item);
    if (c.item != g_item || (g_item && itemInv != s_itemInv)) {
        if (g_item) { g_mem.Store(g_key, g_m.phase()); }
        g_item = c.item;
        s_itemInv = itemInv;
        g_key = nock::MemoryKey(c.item, c.ctrl);   // the controller: the game makes a new item object on every equip
        g_m = nock::Machine(g_mem.Recall(g_key));
        g_haveSeq = false;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] bow %p (controller %p, state %d) - starts %s.", reinterpret_cast<void*>(c.item),
            reinterpret_cast<void*>(c.ctrl), c.state, PhaseName(g_m.phase()));
    }

    g_lastState = c.state;
    nock::Input in{};
    in.enabled = want && live;
    in.gameState = c.state;
    unsigned char spawned = 0;
    in.spawned = ReadByte(c.ctrl + 188, &spawned) && spawned != 0;
    in.trueCount = 0;
    if (live) { CallAmmo(c.ctrl, &in.trueCount); }
    in.trigDown = c.trigDown;
    in.tMs = static_cast<double>(GetTickCount64());
    in.pressEdge = g_haveSeq && c.pressSeq != g_lastSeq;
    g_lastSeq = c.pressSeq;
    g_haveSeq = true;
    in.handInShoulder = HandInShoulder();
    // The arrow wheel from the quiver (2026-11-11, Jay: "We need to have the arrow selection on the right shoulders quiver for the
    // regular bow, same as we do the crossbow" -> "Yes."): the hold STARTS with an RT press in the shoulder zone - the take's own press,
    // in any phase (a seated arrow's type changes too: the bow's +D9DAD0 re-enters PutArrow and re-applies it, ida/awh2) - and ENDS as
    // the bolt holster's does (boltwheel::BoltHoldStep, tested): RT up, the nock not live, or the hand leaving the zone before the wheel
    // is open. The take itself is untouched. COST: a few bool tests + two interlocked stores per tick.
    g_qArmed = boltwheel::BoltHoldStep(g_qArmed, in.pressEdge && in.handInShoulder && in.enabled, in.trigDown, in.enabled,
                                       in.handInShoulder, HolsterArrowWheelOpen());
    InterlockedExchange64(&g_qStampMs, static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange(&g_qHold, g_qArmed ? 1 : 0);

    // The bow's line: arrow_pose at rest in the world (the live latch, else the clip's rest), its tip direction.
    float restW[12] = {};
    // The bow as it will be DRAWN: the held Stand pose when it is available (the hold covers empty, carry, catch and the seat's
    // blend), else the engine's root this update.
    float held[kStandN][12];
    const bool haveHeld = in.enabled && ComputeHeld(c.item, held);
    Mul34(haveHeld ? held[0] : c.rootW, c.restNockInRoot ? c.restNockInRoot : kNockRestInRoot, restW);
    in.lineOrigin = { restW[3], restW[7], restW[11] };
    in.lineTip = Unit(restW[0] * kNockTipInArrow[0] + restW[1] * kNockTipInArrow[1] + restW[2] * kNockTipInArrow[2],
                      restW[4] * kNockTipInArrow[0] + restW[5] * kNockTipInArrow[1] + restW[6] * kNockTipInArrow[2],
                      restW[8] * kNockTipInArrow[0] + restW[9] * kNockTipInArrow[1] + restW[10] * kNockTipInArrow[2]);

    // The carried arrow, on the RENDERED r_hand (spec 2026-10-22-hand-carried-model-design 4.4): its local in r_hand = the clip's
    // frame-132 relation slid back along the arrow by bow_nock_carry_back_m (INV-613 A); the catch tests the nock that is drawn.
    const float d = BowNockCarryBackM;
    const float inHand[12] = { kNockArrowInHandR[0], kNockArrowInHandR[1], kNockArrowInHandR[2], kNockArrowInHandT[0],
                               kNockArrowInHandR[3], kNockArrowInHandR[4], kNockArrowInHandR[5], kNockArrowInHandT[1],
                               kNockArrowInHandR[6], kNockArrowInHandR[7], kNockArrowInHandR[8], kNockArrowInHandT[2] };
    float local[12];   // the relation handcarry composes with the arrow object's own local L (final review 2026-10-22 (4))
    float slid[12];
    handcarry::SlideBack(inHand, d, slid);
    // Jay 2026-10-22 "Tune in the headset": tilt / turn about the nock, then a slide across the hand - all zero = the slid relation.
    handcarry::AdjustAboutNock(slid, BowNockCarryTiltDeg, BowNockCarryTurnDeg, BowNockCarrySideM, local);
    // The catch tests what is drawn: r_hand . relation . L, L = the arrow's own local saved at the take (identity before one).
    const float kIdent[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float drawnLocal[12];
    handcarry::ComposeLocal(local, g_carry.active ? g_carry.homeLocal : kIdent, drawnLocal);
    float handArrow[12] = {};
    bool haveHand = false;
    if (c.rHandW && Finite(c.rHandW, 12)) {
        Mul34(c.rHandW, drawnLocal, handArrow);
        haveHand = Finite(handArrow, 12);
    }
    if (haveHand) {
        in.notch = { handArrow[3], handArrow[7], handArrow[11] };
        in.tip = Unit(handArrow[0] * kNockTipInArrow[0] + handArrow[1] * kNockTipInArrow[1] + handArrow[2] * kNockTipInArrow[2],
                      handArrow[4] * kNockTipInArrow[0] + handArrow[5] * kNockTipInArrow[1] + handArrow[6] * kNockTipInArrow[2],
                      handArrow[8] * kNockTipInArrow[0] + handArrow[9] * kNockTipInArrow[1] + handArrow[10] * kNockTipInArrow[2]);
    }
    in.haveHand = haveHand;   // no hand: the machine holds its phase (no catch / drop / seat from a stand-in point)

    const nock::Params p = { BowNockCatchM, BowNockHoldM, BowNockSeatM, BowNockCatchDeg, BowNockCatchLenM };
    const nock::Output o = g_m.Step(in, p);

    // ---- apply ----
    SetFake(c.ctrl, o.fakeEmpty);
    if (o.ev == nock::Event::Take || o.ev == nock::Event::Catch || o.ev == nock::Event::Seat) {
        PulseController(kRightHand, 0.6f, 40);   // the draw's grab pulse (Jay: "Haptic pulse on each, same as the draw")
    }
    if (o.holdPutArrow || o.endPutArrow) {
        const TimeFn tf = TimeFnOf(c.ctrl);
        float now = 0.0f, dur6 = 0.0f;
        if (!tf || !CallTime(tf, c.ctrl, &now)) {
            if (!g_saidTime) {
                g_saidTime = true;
                // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
                LOG("bownock: [BOWNOCK] *** CANNOT HOLD PutArrow *** - the controller's vtable slot 6 is not gamedll+0x%zX (a "
                    "different game build?) or its clock is unreadable; the game will finish PutArrow by itself.",
                    static_cast<size_t>(kGameTimeRva));
            }
        } else if (o.holdPutArrow) {
            WriteFloat(c.ctrl + 72, now);                                    // state 6 never runs out while the hand carries
        } else if (ReadFloat(c.ctrl + 76 + 4 * 6, &dur6)) {
            WriteFloat(c.ctrl + 72, now - dur6 - 0.05f);                     // the game's own 6 -> 0 fires at its next update
        }
    }
    if ((g_nkFingerOn != 0) != o.holdFingers) {   // the arrow-holding fingers: start the 100 ms blend on a change
        InterlockedExchange(&g_nkFingerOn, o.holdFingers ? 1 : 0);
        InterlockedExchange64(&g_nkFingerTick, static_cast<LONG64>(GetTickCount64()));
    }
    // THE CARRY (handcarry.h, INV-613): while the hand carries, the game's own arrow object rides r_hand - the engine moves it, we
    // write nothing per frame; anything else gives it back to arrow_pose exactly as found. Before the arrow_pose writes below, so a
    // catch's give-back lands on the bone the on-line write then moves. COST: one pointer read per update; + one export call per
    // update while carrying; + 3-4 export calls per take / give-back / re-take.
    {
        ++g_carryTickSeq;   // BowNockCarryWatch: the tick ran this update
        uintptr_t obj = 0;
        ReadPtr(c.item + kBowArrowObjOffset, &obj);
        handcarry::In hc{};
        // No hand matrix needed: once attached the engine carries it (final review (3) - a missed r_hand read must not snap it back).
        hc.wantCarry = in.enabled && o.phase == nock::Phase::Carry && (obj || g_carry.active) && c.playerModel && c.rHandId >= 0
                       && c.nockId >= 0;
        hc.active = g_carry.active;
        hc.sameObject = handcarry::SameObject(reinterpret_cast<void*>(c.item), reinterpret_cast<void*>(obj), g_carry.homeModel,
                                              g_carry.obj);
        hc.bodyChanged = g_carry.active && c.playerModel != g_carry.onModel;
        hc.stillOn = !(hc.active && hc.sameObject && hc.wantCarry && !hc.bodyChanged) || HandCarryStillOn(g_carry);
        hc.relChanged = g_carry.active && HandCarryRelChanged(g_carry, local);
        switch (handcarry::Decide(hc)) {
        case handcarry::Act::Take:
            HandCarryTake(g_carry, reinterpret_cast<void*>(obj), reinterpret_cast<void*>(c.item), c.nockId, c.playerModel, c.rHandId,
                          local, d);
            break;
        case handcarry::Act::Retake: HandCarryReattach(g_carry, local); break;
        case handcarry::Act::SetLocal: HandCarrySetLocal(g_carry, local); break;
        case handcarry::Act::GiveBack: HandCarryGiveBack(g_carry, hc.bodyChanged ? "the body changed" : PhaseName(o.phase)); break;
        case handcarry::Act::Forget: HandCarryForget(g_carry, "the bow or its arrow changed"); break;
        default: break;
        }
    }
    // PARENT FIRST, CHILD LAST (INV-591): SetElementWorldMatrix's world is re-expressed as a LOCAL against the parent at the parent's
    // next write, so the bow's own bones (bone_root among them) are written BEFORE arrow_pose, or the root's change drags the arrow
    // (Jay run 9: "The arrows still floating infront of the hand" - the arrow had been written first, then bone_root by the hold).
    // The in-hand arrow_pose write is GONE (Jay 2026-10-22: "it'll be overriding the behavour") - the carried arrow is the object above.
    if (o.holdRestPose && haveHeld) { WriteHeld(c.item, reinterpret_cast<SetWorldFn>(c.setWorld), held); }
    if (o.writeArrow && o.onLine && c.setWorld && c.nockId >= 0) {
        float m[12];
        memcpy(m, restW, sizeof(m));
        m[3] = in.lineOrigin.x + in.lineTip.x * o.along;
        m[7] = in.lineOrigin.y + in.lineTip.y * o.along;
        m[11] = in.lineOrigin.z + in.lineTip.z * o.along;
        if (Finite(m, 12)) {
            CallSetWorld(reinterpret_cast<SetWorldFn>(c.setWorld), c.item, c.nockId, m);
        }
    }
    if (o.holdArrowAtRest && haveHeld && c.setWorld && c.nockId >= 0 && Finite(restW, 12)) {
        CallSetWorld(reinterpret_cast<SetWorldFn>(c.setWorld), c.item, c.nockId, restW);   // the seated arrow on the held bow
    }

    // ---- log ----
    if (o.ev != nock::Event::None) {
        const int ei = static_cast<int>(o.ev);
        if (ei >= 0 && ei < 11) { ++g_n[ei]; }
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("bownock: [BOWNOCK] %s #%ld - value %.3f | now %s, game state %d, arrows %d | notch along the line %.3f m, hand %s.",
            EvName(o.ev), ei >= 0 && ei < 11 ? g_n[ei] : -1L, o.evValue, PhaseName(o.phase), c.state, in.trueCount,
            o.onLine ? o.along : -1.0f, haveHand ? "read (rendered r_hand)" : "*** NOT read (rendered r_hand unavailable) ***");
    }
    const ULONGLONG t = GetTickCount64();
    if (t - g_lastTotals >= 5000) {
        g_lastTotals = t;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        // The self-check (spec 2026-10-22 6): the drawn arrow's origin (its object, element 0) against the nock the catch tests -
        // 0.0 mm when the catch tests what is drawn; -1 = not carrying now.
        float origin[3];
        float gapMm = -1.0f;
        if (g_carry.active && haveHand && HandCarryObjectOrigin(g_carry, origin)) {
            const float gx = origin[0] - handArrow[3], gy = origin[1] - handArrow[7], gz = origin[2] - handArrow[11];
            gapMm = 1000.0f * std::sqrt(gx * gx + gy * gy + gz * gz);
        }
        LOG_PERIODIC("bownock: [BOWNOCK] 5 s: phase %s, state %d | take %ld (no arrows %ld), catch %ld (wrong way %ld), off the line %ld, "
                     "seat %ld, early release %ld, game moved %ld, shot %ld | count answered 0 %ld times | carried in the hand: "
                     "takes %ld, give-backs %ld, forgets %ld, taken back by the game %ld, back %.4f m, tilt %.1f deg, turn %.1f deg, "
                     "side %+.4f m, drawn vs catch nock %.1f mm.",
                     PhaseName(g_m.phase()), c.state, g_n[1], g_n[2], g_n[3], g_n[4], g_n[5], g_n[6], g_n[7], g_n[8], g_n[9],
                     static_cast<long>(g_nkFaked), g_carry.takes, g_carry.giveBacks, g_carry.forgets, g_carry.retakes, d,
                     BowNockCarryTiltDeg, BowNockCarryTurnDeg, BowNockCarrySideM, gapMm);
    }
}

}  // namespace dlvr
