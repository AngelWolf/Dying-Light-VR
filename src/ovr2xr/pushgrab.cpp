// pushgrab.cpp - the open-hand push on live enemies (plan docs/superpowers/plans/2026-10-31-open-hand-push-and-grab.md Task 3; spec
// docs/superpowers/specs/2026-10-31-open-hand-push-and-grab-design.md). Everything sent to the game is a call the game itself makes
// (RESEARCH-ENEMY-GRAB §7 / §9, INV-666):
//   - the shove = +DC7190 (the player's melee controller's "hit this AI with this item from this point" - the game's own body bump,
//     MoveController's crowd collision) with the item Melee_CrowdBump (KICK, Damage 0, Force 0, severity Medium in our three
//     inventory paks), from an origin placed so the hit's direction is the hand's;
//   - the knock-down (a really hard shove) = the same hit + SetShouldFall(ai, 1, 0) when the enemy's own influence for PUSH (20) is
//     non-zero - the two calls OUR HumanLogicModule +2ABE70 makes for a PUSH (ledger ruling, plan Task 3).
// The hit (+C4EA30 inside +DC7190) requests co-op ownership of the target itself. Only targets the game's own body bump would hit:
// the AI's vfunc +336 with the player (the bump's gate, the Mac IsEnemy position - §9 (f)).
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pushgrab.h"
#include "pushgrab_logic.h"
#include "handcoll.h"
#include "bodygrab.h"
#include "looting.h"
#include "controllers.h"
#include "breakout.h"
#include "headpose.h"       // GetHeadPose - the arm-only shove speed (hand minus head)
#include "boneblend.h"      // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: the gamedll rvas below are family 14 of tools/egl_allowlist.tsv. The DEV tree has no buildrva.h: Steam only.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_PG_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_PG_GD_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: enemy_push_grab] required-for=the open-hand push on a live enemy (EnemyPushGrab) - spec 2026-10-31-open-hand-push-and-grab.
//      tree=INERT at 0: PushGrabTick returns at its first test; nothing of the game's is read or called.
//      HEADSET: needs hand_collision=1 (the contact - its own HEADSET chain: the tail solve, controller poses), body_grab=1 and
//      loot_by_hand=1 (the grab's spring - BodyGrabTick runs only with loot by hand) - READY refuses naming each; vr_grip (the grab's
//      squeeze); the game's own single-player test (the grab's, 2026-10-31 (5): GameDI -> +0x540 the session, its vftable one of
//      the three Session classes, dword +0xD0 == 0 - 16 sites in OUR gamedll); the Steam gamedll 97046bba or the Epic build
//      (buildrva family 14) - READY byte-checks every called function; any other build REFUSES. FLAT: READY only.
//      Was compiled default 0 - Tier C until Jay verifies it in the headset.
//      SHIPS ON since 2026-11-03 (Jay picked "Enemy push + grab" for the tester package; parked 10-31).
bool  EnemyPushGrab = true;
// [DEP: push_min_mps] required-for=the ARM speed below which a contact does not push (PushMinMps) - since 2026-10-31 (6) the hand minus
//      the head, 3D, over 50-150 ms (ArmSpeedMeter; was the controller's horizontal per-update speed). Start value 1.2 m/s -
//      Claude's call, Tier D. tree=INERT at enemy_push_grab=0.
float PushMinMps = 1.2f;
// [DEP: push_knockdown_mps] required-for=the hand speed at or above which a shove also knocks down (PushKnockdownMps). Start value
//      3.5 m/s - Claude's call, Tier D. tree=INERT at enemy_push_grab=0.
float PushKnockdownMps = 3.5f;
// [DEP: push_rearm_s] required-for=the time after which the same contact may push again (PushRearmS). Start value 0.4 s - Claude's
//      call, Tier D. tree=INERT at enemy_push_grab=0.
float PushRearmS = 0.4f;
// [DEP: push_hard_mps] required-for=the arm speed at or above which a shove is HARD - the game's medium hit (Melee_CrowdBump, the
//      kick stumble-back) instead of the light one (Melee_VRShove, our Data3 item, the small front flinch - RESEARCH-ENEMY-GRAB 10q /
//      10r; Jay 2026-10-31 (6) "Yes."). Below it, above push_min_mps = light. Start value 2.5 m/s - Claude's call, Tier D. Needs Data3
//      394248e7 (the light item); without it every shove falls back to Melee_CrowdBump, named once. tree=INERT at enemy_push_grab=0.
float PushHardMps = 2.5f;
// [DEP: player_ai_helper_radius_m] required-for=the horizontal radius of the player's AwayFromAIHelper (PlayerAiHelperRadiusM; INV-669:
//      the game's own collision shape that keeps AIs off the player - a Y-ellipsoid XZ 0.6 / Y 0.4, made by +B9AA70 and 9 controller
//      functions from ONE shared gamedll .data value +1BA9340). Jay 2026-10-31 (6): "There's still a capsule collision around the
//      players body" -> "Yes, build it". Written into that shared value from the pad poll (menus included) while enemy_push_grab=1, so
//      the helper is BUILT smaller; the game's 0.6 is put back when off. A helper already built keeps its size until the game remakes
//      it. Start value 0.30 m - Claude's call, Tier D. tree=LOADBEARING at enemy_push_grab=1 (the gap); INERT at 0 (restored).
float PlayerAiHelperRadiusM = 0.30f;
// [DEP: grab_break_free_s] required-for=how long a grabbed enemy is held before it breaks free (GrabBreakFreeS). Start value 2.5 s -
//      Claude's call (spec §1 7), Tier D. tree=INERT at enemy_push_grab=0.
float GrabBreakFreeS = 2.5f;
// [DEP: grab_drop_mps] required-for=the hand speed at release below which a held enemy is dropped, above which it is thrown
//      (GrabDropMps). Start value 1.0 m/s - Claude's call, Tier D. tree=INERT at enemy_push_grab=0.
float GrabDropMps = 1.0f;
// [DEP: push_grab_intent_squeeze] required-for=the squeeze at or above which a hand that would shove is reaching to grab and does
//      not shove (PushGrabIntentSqueeze; spec 2026-10-31-zombie-close-approach-and-grab-intent §3 - PG2: 2 of 3 grabs began with a
//      shove). Read at the moment PushArm::Fire fires. Start value 0.20 (the grab itself starts at 0.5) - Claude's call, Tier D.
//      tree=INERT at enemy_push_grab=0.
float PushGrabIntentSqueeze = 0.20f;
// [DEP: push_zombie_melee_range_m] required-for=the melee range an ORDINARY zombie (ZombieLogicModule) uses against the player - the
//      zombie walks in to about this before it attacks (PushZombieMeleeRangeM; spec 2026-10-31-zombie-close-approach-and-grab-intent
//      §2, INV-668: the game's own is 1.8 m x AI+2792, PG2 free touches 1.01-1.48 m). Applied by the detour on gamedll +30BBF0; other
//      enemies, other targets and zombie-vs-NPC fights get the game's value. Compiled 0.70 m = Jay's disk, verified 2026-10-31 (6)
//      (PG5, Jay: "That's worked"; was Claude's 0.90 - PG3 free zombies stopped 0.86-0.88 m, still at full arm reach). Tier D.
//      tree=LOADBEARING at enemy_push_grab=1 (the push / grab cannot reach a zombie without it); INERT at 0 (the detour returns the
//      game's value at one bool test). HEADSET: live zombies attacking the player.
float PushZombieMeleeRangeM = 2.5f;   // 2026-11-03: Jay's disk value (Jay: "2.5 (your disk)"; was 0.70, verified 10-31)

namespace {

namespace pg = ::dlvr::pushgrab;

// ---- OUR gamedll 97046bba - RESEARCH-ENEMY-GRAB §9 ----
constexpr uintptr_t kHitRva = 0xDC7190;       // GiveShockWaveDamage(meleeCtl, target, item, strength, origin) (§9 (a))
constexpr uintptr_t kItemByNameRva = 0x6F9250;  // ItemManager::FindDefinition(&ttl string, 1, -1.0f, 0) -> item (§9 (a))
constexpr uintptr_t kShouldFallRva = 0x5E9BA0;  // HumanAI::SetShouldFall(ai, clearHit, force) (§9 (c))
constexpr uintptr_t kInfluenceRva = 0x2C3810;   // logic module: the damage influence of a damage type (INV-666 pass 2)
constexpr unsigned char kHitHead[16] = { 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF };
constexpr unsigned char kItemHead[16] = { 0x44, 0x89, 0x4C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41 };
constexpr unsigned char kFallHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9 };
constexpr unsigned char kInflHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48 };
constexpr size_t kMeleeCtlOff = 3088;          // PlayerDI + 3088 -> the melee controller (+D54FD0 :125)
constexpr size_t kEnemyOff = 336;              // AI vfunc +336(ai, player) = the body bump's target gate (+D550C8, §9 (f))
constexpr size_t kLogicModOff = 3416;          // AI + 3416 -> the logic module (INV-659)
constexpr size_t kModStaleOff = 76;            // module float +76 != 0 = not the live module
constexpr int    kDamagePush = 20;             // EDamageType PUSH in OUR +2A5A00
constexpr float  kOriginBackM = 10.0f;         // the hit's origin this far back along the hand's direction (ledger ruling)
constexpr size_t kAddLinVelOff = 2456;         // HumanAI slot 307 = AddRagdollLinVel(vel, pend) - the ragdoll velocity (RESEARCH 3a)
constexpr double kFallWaitS = 0.5;             // the grab waits this long for the game's knock-down to bring the ragdoll up
constexpr double kContactFreshS = 0.1;         // a published contact older than this is not "in contact" (pad thread)
constexpr float  kGripPress = 0.5f;            // the same squeeze threshold ovr2xr.cpp uses for L1 / R1
// The grab's single-player test (2026-10-31 (5), PG1: IGSObject::IsReplicable refused 4 of 4 grabs in single player - INV-74, it
// passes there). OUR gamedll: 16 sites read [[GameDI]+0x540]+0xD0 == 0 (ida/pg13); the object at +0x540 is built by the SessionDI
// constructor +4B1DE0 (ida/pg14); the Mac SessionDI::IsSinglePlayer is the same test (`*(this+208) == 0`, comparison only).
constexpr uintptr_t kGameDiRva = 0x1C16348;     // qword: GameDI (the camera global, INV-595)
constexpr size_t kSessionOff = 0x540;           // GameDI + 0x540 -> the session
constexpr size_t kSessionSoloOff = 0xD0;        // session dword +0xD0 == 0 = single player
constexpr uintptr_t kSessionVtRva[3] = { 0x14B8BA0, 0x14B5980, 0x14B4AA8 };   // SessionDI, SessionCooperativeDI, SessionCooperative
// The zombie close approach (spec 2026-10-31-zombie-close-approach-and-grab-intent §2, INV-668): the melee range for a target, slot
// 280 of TEN logic modules (Zombie, Thug, Armored, Volatile, Infected, Prime, HiveBrood, Caster, Spitter, BtzSuicider - ida/gz8, gz9);
// the zombie's melee method (+695D50) walks in to range - 0.05 - param 765 of the target, then attacks. Capped for ZombieLogicModule
// targeting the player only (Jay: "Ordinary zombies").
constexpr uintptr_t kRangeRva = 0x30BBF0;
constexpr unsigned char kRangeBytes[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
constexpr uintptr_t kParamRva = 0x233C60;       // AIDataSetWrapper::FindParam(wrapper, id) by shape (INV-668, Mac comparison)
constexpr uintptr_t kZombieModVtRva = 0x146D860; // ZombieLogicModule vftable (RTTI, ida/gz9)
constexpr size_t kModAiOff = 64;               // logic module + 64 -> its AI (+30BBF0)
constexpr size_t kAiDataSetOff = 2496;         // AI + 2496 -> the AI data set wrapper (+30BBF0)
constexpr size_t kAiRangeScaleOff = 2792;      // AI float + 2792 = the scale the range is multiplied by (+30BBF0)
constexpr int    kParamMeleeRange = 53;        // melee_range by inference (INV-668)
constexpr int    kParamReachMargin = 765;      // the margin +695D50 subtracts (name NOT established)
constexpr uintptr_t kHelperSizeRva = 0x1BA9340;  // gamedll .data: the player's AwayFromAIHelper size (XZ, Y, XZ) - INV-669
constexpr long   kMaxEventLines = 200;         // INV-9
constexpr long   kMaxWindowLines = 200;

struct TtlString { const char* p; unsigned size; unsigned cap; };   // ttl::string_base as +D49CA0 passes it to +6F9250

using HitFn = void(__fastcall*)(void* meleeCtl, void* target, void* item, float strength, const float* origin);
using ItemByNameFn = void*(__fastcall*)(const TtlString* name, int a, float b, int c);
using ShouldFallFn = void(__fastcall*)(void* ai, char clearHit, char force);
using InfluenceFn = float(__fastcall*)(void* module, int type);
using EnemyFn = bool(__fastcall*)(void* ai, void* other);
using AddLinVelFn = void(__fastcall*)(void* ai, const float* vel, char pend);

HitFn g_hit = nullptr;
uintptr_t g_gdBase = 0;
ItemByNameFn g_itemByName = nullptr;
ShouldFallFn g_shouldFall = nullptr;
InfluenceFn g_influence = nullptr;
void* g_crowdBump = nullptr;     // the Melee_CrowdBump item, resolved once the ItemManager has it
void* g_lightShove = nullptr;    // Melee_VRShove (our Data3, Light only) - the light shove; null = falls back to g_crowdBump
int g_resolved = 0;              // 0 not yet, 1 READY, -1 REFUSED (named once)
volatile LONG g_nEvent = 0, g_nWindow = 0;

struct ArmState {
    pg::PushArm arm;
    bool wasContact = false;
    double winS = 0.0;
    long contacts = 0, pushes[3] = {}, skips = 0;
    float maxSpeed = 0.0f;
};
ArmState g_a[2];

// ---- the grab (plan Task 4). Arm index = handcoll's: 0 = RIGHT, 1 = LEFT; body grab's hand = 1 - arm. ----
// Game -> pad: this update's contact per arm (seqlocked; the pad thread decides "in contact" from its age).
struct PubContact { void* ai; int id; void* player; float point[3]; double tS; float gapM; };
volatile LONG g_pubSeq[2] = {};
PubContact g_pub[2] = {};
// Pad -> game: 1 = START (with g_startC), 2 = RELEASE, 3 = BREAK-FREE. The game thread takes it with InterlockedExchange.
volatile LONG g_cmd[2] = {};
PubContact g_startC[2] = {};
pushgrab::GrabMachine g_m[2];             // pad thread only
pushgrab::GripLatch g_latch[2];           // pad thread only: withhold from a START until the grip opens (final review I1)
volatile LONG g_padHolding[2] = {};       // pad: the grab owns this grip now (withhold / no push / PushGrabHandActive)
volatile LONG g_canGrab[2] = {};          // pad: a press on this hand could start a grab now (vr_grip on, grip not owned) - final review I3
// Final review I2: the controller's own horizontal speed in its tracking space (game thread), for the push tier.
struct Tracked { bool have = false; pushgrab::ArmSpeedMeter meter; float speed = 0.0f; };
Tracked g_trk[2];
enum class Phase { Idle, Waiting, Holding, ThrowPending };
struct GrabState {                        // game thread only
    Phase phase = Phase::Idle;
    PubContact c = {};
    double startS = 0.0, holdS = 0.0;
    bool seenActive = false, throwIt = false;
    float vel[3] = {};
    const char* endWhy = "";
};
GrabState g_gs[2];
volatile LONG g_gameActive[2] = {};       // game: phase != Idle
int BgHand(int arm) { return 1 - arm; }

double NowS()   // the same clock as handcoll.cpp / looting.cpp NowS
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
bool CapLine(volatile LONG* n, long cap, const char* kind)
{
    const LONG k = InterlockedIncrement(n);
    if (k == cap + 1) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] CAP REACHED - %ld %s lines written; no more of that kind this launch", cap, kind);
    }
    return k <= cap;
}
bool EventLine() { return CapLine(&g_nEvent, kMaxEventLines, "event (PUSH / SKIP)"); }
bool WindowLine() { return CapLine(&g_nWindow, kMaxWindowLines, "1 s"); }
volatile LONG g_nMiss = 0;
constexpr long kMaxMissLines = 200;
const char* ArmName(int a);

// Pad thread: a grip PRESS that found no contact - how near that fingertip was, so push_grab_reach_m can be sized from a run.
void MissLine(int a, double now)
{
    float gap = 0.0f;
    double atS = 0.0;
    const bool have = HandCollisionNearest(a, &gap, &atS);
    if (!CapLine(&g_nMiss, kMaxMissLines, "GRAB MISS")) { return; }
    if (have) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
        LOG("pushgrab: [PUSHGRAB] GRAB MISS %s hand - grip pressed with no contact | nearest body surface %.0f mm (reach %.0f mm), "
            "measured %.0f ms ago", ArmName(a), static_cast<double>(gap * 1000.0f), static_cast<double>(PushGrabReachM * 1000.0f),
            (now - atS) * 1000.0);
    } else {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
        LOG("pushgrab: [PUSHGRAB] GRAB MISS %s hand - grip pressed with no contact | nearest body surface NOT MEASURED (the hand holds "
            "something, no enemy tracked, or no solve yet)", ArmName(a));
    }
}
const char* ArmName(int a) { return a == 0 ? "RIGHT" : "LEFT"; }
const char* TierName(pg::PushTier t) { return t == pg::PushTier::KnockDown ? "KNOCK-DOWN" : t == pg::PushTier::Shove ? "SHOVE" : "none"; }

// ---- SEH-guarded game reads and calls (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HeadMatches(uintptr_t a, const unsigned char* want)
{
    __try { return std::memcmp(reinterpret_cast<const void*>(a), want, 16) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallItemByName(const char* name, void** out)
{
    TtlString s = { name, static_cast<unsigned>(std::strlen(name)), static_cast<unsigned>(std::strlen(name)) };
    __try { *out = g_itemByName(&s, 1, -1.0f, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 1 = an enemy of the player, 0 = not, -1 = the call faulted (fails closed: not pushed).
int CallIsEnemy(void* ai, void* player)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ai);
        const auto fn = reinterpret_cast<EnemyFn>(*reinterpret_cast<const uintptr_t*>(vt + kEnemyOff));
        return fn(ai, player) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool CallHit(void* meleeCtl, void* ai, void* item, const float origin[3])
{
    __try { g_hit(meleeCtl, ai, item, 1.0f, origin); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The influence of PUSH for this AI (its logic module). -1 = not readable (the knock-down is then not made).
float CallInfluence(void* ai)
{
    __try {
        const uintptr_t mod = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(ai) + kLogicModOff);
        if (!mod || *reinterpret_cast<const float*>(mod + kModStaleOff) != 0.0f) { return -1.0f; }
        return g_influence(reinterpret_cast<void*>(mod), kDamagePush);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}
bool CallShouldFall(void* ai)
{
    __try { g_shouldFall(ai, 1, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 1 = single player, 0 = not (*flag = the dword), -1 = not readable (no GameDI / session, a fault, or a vftable that is not one of
// the three Session classes - *vt says which): fails closed, refused. COST (per grab press): four reads.
int SessionSolo(uintptr_t* vt, int* flag)
{
    *vt = 0;
    *flag = -1;
    __try {
        const uintptr_t gdi = *reinterpret_cast<const uintptr_t*>(g_gdBase + DLVR_PG_GD_RVA(kGameDiRva));
        if (!gdi) { return -1; }
        const uintptr_t s = *reinterpret_cast<const uintptr_t*>(gdi + kSessionOff);
        if (!s) { return -1; }
        *vt = *reinterpret_cast<const uintptr_t*>(s);
        bool known = false;
        for (uintptr_t r : kSessionVtRva) { known = known || *vt == g_gdBase + DLVR_PG_GD_RVA(r); }
        if (!known) { return -1; }
        *flag = *reinterpret_cast<const int*>(s + kSessionSoloOff);
        return *flag == 0 ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool CallAddLinVel(void* ai, const float vel[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ai);
        reinterpret_cast<AddLinVelFn>(*reinterpret_cast<const uintptr_t*>(vt + kAddLinVelOff))(ai, vel, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the zombie close approach (spec §2) ----
using RangeFn = float(__fastcall*)(void* module, void* target);
using ParamFn = unsigned char*(__fastcall*)(void* wrapper, int id);
using ParamValueFn = const float*(__fastcall*)(void* param);
RangeFn g_rangeTrue = nullptr;          // the trampoline (written by the install before the jump goes live)
ParamFn g_param = nullptr;
uintptr_t g_zombieModVt = 0;
int g_rangeState = 0;                   // 0 not tried, 1 live, -1 refused (named once)
volatile LONG g_rngFirst = 0, g_rngCalls = 0, g_rngPlayer = 0, g_rngCapped = 0;
// Final review I2: the first sample waits for a published player (one line says so meanwhile); the first kIdChecks zombie calls whose
// target is NOT our PlayerDI compare ids, so "the game passes a different pointer for the player" is said, not inferred.
constexpr LONG kIdChecks = 64;
volatile LONG g_rngNoPlayerSaid = 0, g_rngIdChecks = 0, g_rngIdSaid = 0;
// Final review I1: the range line has its own budget - it must never silence the per-hand 1 s lines.
volatile LONG g_nRange = 0;
constexpr long kMaxRangeLines = 300;
volatile LONG g_rngMinBits = 0x7F7FFFFF, g_rngMaxBits = 0;   // player-target originals, float bits (positive floats order as ints)

void BitsMin(volatile LONG* b, float v)
{
    LONG nv = 0;
    std::memcpy(&nv, &v, sizeof(nv));
    for (LONG cur = *b; nv < cur; cur = *b) { if (InterlockedCompareExchange(b, nv, cur) == cur) { break; } }
}
void BitsMax(volatile LONG* b, float v)
{
    LONG nv = 0;
    std::memcpy(&nv, &v, sizeof(nv));
    for (LONG cur = *b; nv > cur; cur = *b) { if (InterlockedCompareExchange(b, nv, cur) == cur) { break; } }
}
float FromBits(LONG b)
{
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}
// The value the game's own getter gives a param of this module's AI (the +30BBF0 pattern). *set = false: the data does not set it
// (the game then uses its default). False = a fault or no data set.
bool ReadAiParam(void* module, int id, float* out, bool* set)
{
    __try {
        const uintptr_t ai = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(module) + kModAiOff);
        const uintptr_t wr = ai ? *reinterpret_cast<const uintptr_t*>(ai + kAiDataSetOff) : 0;
        unsigned char* p = wr ? g_param(reinterpret_cast<void*>(wr), id) : nullptr;
        if (!p) { return false; }
        *set = (p[24] & 1) != 0;
        if (*set) {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
            *out = *reinterpret_cast<ParamValueFn>(*reinterpret_cast<const uintptr_t*>(vt + 40))(p);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadAiFloat(void* module, size_t off, float* out)
{
    __try {
        const uintptr_t ai = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(module) + kModAiOff);
        if (!ai) { return false; }
        *out = *reinterpret_cast<const float*>(ai + off);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Once, on the first call for an ordinary zombie with a target - logged unconditionally (spec §2 "Logging").
void RangeFirstSample(void* module, void* target, void* player, float orig)
{
    float range = 0.0f, margin = 0.0f, scale = 0.0f;
    bool rangeSet = false, marginSet = false;
    const bool okRange = ReadAiParam(module, kParamMeleeRange, &range, &rangeSet);
    const bool okMargin = ReadAiParam(module, kParamReachMargin, &margin, &marginSet);
    const bool okScale = ReadAiFloat(module, kAiRangeScaleOff, &scale);
    int tid = 0, pid = 0;
    const bool okT = HandCollisionObjectId(target, &tid);
    const bool okP = player && HandCollisionObjectId(player, &pid);
    char sRange[32] = "NOT READ", sMargin[32] = "NOT READ", sScale[32] = "NOT READ", sT[16] = "NOT READ", sP[16] = "NOT READ";
    if (okRange) { std::snprintf(sRange, sizeof(sRange), "%.3f%s", static_cast<double>(range), rangeSet ? "" : " (not set - default)"); }
    if (okMargin) { std::snprintf(sMargin, sizeof(sMargin), "%.3f%s", static_cast<double>(margin), marginSet ? "" : " (not set - default)"); }
    if (okScale) { std::snprintf(sScale, sizeof(sScale), "%.3f", static_cast<double>(scale)); }
    if (okT) { std::snprintf(sT, sizeof(sT), "%d", tid); }
    if (okP) { std::snprintf(sP, sizeof(sP), "%d", pid); }
    const char* const verdict = !player ? "the player is not published yet - no cap applied until it is"
                              : target == player ? "the target IS the PlayerDI we hold - the cap applies"
                              : "the target is NOT the PlayerDI we hold - no cap applied (it may be another target; see the ids)";
    // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
    LOG("pushgrab: [PUSHGRAB] ZOMBIE RANGE first sample (logged unconditionally) - ZombieLogicModule %p, target %p, player %p | "
        "original %.3f m, cap %.3f m | param 53 (melee_range) %s, param 765 (the reach margin) %s, AI+2792 (scale) %s | %s | "
        "target id %s, player id %s | thread %lu",
        module, target, player, static_cast<double>(orig), static_cast<double>(PushZombieMeleeRangeM), sRange, sMargin, sScale,
        verdict, sT, sP, GetCurrentThreadId());
}

// The detour. COST per call (AI goal evaluation, not per bone): the original, one guarded vftable read, two pointer compares, a float
// compare and interlocked counters; the first-sample read happens once. Off / not READY: the original only.
float __fastcall ZombieRangeHook(void* module, void* target)
{
    const float orig = g_rangeTrue(module, target);
    if (!EnemyPushGrab || g_resolved != 1) { return orig; }
    InterlockedIncrement(&g_rngCalls);
    uintptr_t vt = 0;
    const bool zombie = module && ReadPtr(reinterpret_cast<uintptr_t>(module), &vt) && vt == g_zombieModVt;
    void* const player = HandCollisionPlayer();
    const bool isPlayer = target && player && target == player;
    if (zombie && target && !player && InterlockedExchange(&g_rngNoPlayerSaid, 1) == 0) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
        LOG("pushgrab: [PUSHGRAB] ZOMBIE RANGE - an ordinary zombie asked its melee range before the PlayerDI was published (hand "
            "collision not ticking yet, or hand_collision=0) - no cap until it is; the first sample waits for it (logged once)");
    }
    if (zombie && target && player && InterlockedExchange(&g_rngFirst, 1) == 0) { RangeFirstSample(module, target, player, orig); }
    if (zombie && target && player && !isPlayer && g_rngIdSaid == 0 && InterlockedIncrement(&g_rngIdChecks) <= kIdChecks) {
        int tid = 0, pid = 0;
        if (HandCollisionObjectId(target, &tid) && HandCollisionObjectId(player, &pid) && tid == pid &&
            InterlockedExchange(&g_rngIdSaid, 1) == 0) {
            // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
            LOG("pushgrab: [PUSHGRAB] ZOMBIE RANGE - the game passes a DIFFERENT POINTER for the player: target %p has the player's id "
                "%d, our PlayerDI is %p - NO CAP APPLIED (logged once)", target, tid, player);
        }
    }
    if (zombie && isPlayer) {
        InterlockedIncrement(&g_rngPlayer);
        BitsMin(&g_rngMinBits, orig);
        BitsMax(&g_rngMaxBits, orig);
    }
    const float out = pg::CapZombieMeleeRange(orig, zombie, isPlayer, PushZombieMeleeRangeM);
    if (out < orig) { InterlockedIncrement(&g_rngCapped); }
    return out;
}

// Once, from Resolve after its byte checks. A refusal keeps the push and grab running without the cap, named.
// [LOADBEARING: zombie close approach] (enemy_push_grab) the cap IS the gap fix - without it ordinary zombies stop at their own
//      ~1.0-1.5 m melee distance (PG2, INV-668) and the hand cannot reach them.
void InstallRange(uintptr_t base, char* status, size_t n)
{
    g_param = reinterpret_cast<ParamFn>(base + DLVR_PG_GD_RVA(kParamRva));
    g_zombieModVt = base + DLVR_PG_GD_RVA(kZombieModVtRva);
    int attempts = 0;
    const bool ok = InstallSingleGameDetour(base + kRangeRva, kRangeBytes, sizeof(kRangeBytes), reinterpret_cast<void*>(&ZombieRangeHook),
                                            reinterpret_cast<void**>(&g_rangeTrue), "zombie melee range", &attempts);
    g_rangeState = ok ? 1 : -1;
    if (ok) {
        std::snprintf(status, n, "LIVE (gamedll+%llX Steam rva, %d attempt(s))", static_cast<unsigned long long>(kRangeRva), attempts);
    } else {
        std::snprintf(status, n, "REFUSED (gamedll+%llX Steam rva: the first 15 bytes differ from the Steam / Epic builds, or the "
                      "write was refused; %d attempt(s)) - push and grab run WITHOUT it", static_cast<unsigned long long>(kRangeRva),
                      attempts);
    }
}

// Final review I4: the three switches are checked EVERY update (one line when the answer changes), never latched - they may be
// switched on in any order in the menu. Only a binary / export refusal below is for the launch.
bool SwitchesOn()
{
    static int s_said = -1;   // -1 never, 0 said missing, 1 said all on
    const bool ok = HandCollision && BodyGrab && LootByHand;
    if (static_cast<int>(ok) != s_said) {
        s_said = ok ? 1 : 0;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] %s - hand_collision=%s, body_grab=%s, loot_by_hand=%s (the push needs the hand collision's contact, "
            "the grab body grab's spring, which runs only with loot by hand)", ok ? "SWITCHES ON" : "WAITING FOR SWITCHES",
            HandCollision ? "1" : "0", BodyGrab ? "1" : "0", LootByHand ? "1" : "0");
    }
    return ok;
}

bool Resolve()
{
    if (g_resolved != 0) { return g_resolved == 1; }
    HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd) { return false; }   // not loaded yet: retried next update
    const uintptr_t base = reinterpret_cast<uintptr_t>(gd);
    struct Site { const char* name; uintptr_t rva; const unsigned char* head; };
    const Site sites[] = {
        { "GiveShockWaveDamage +DC7190", DLVR_PG_GD_RVA(kHitRva), kHitHead },
        { "ItemManager::FindDefinition +6F9250", DLVR_PG_GD_RVA(kItemByNameRva), kItemHead },
        { "SetShouldFall +5E9BA0", DLVR_PG_GD_RVA(kShouldFallRva), kFallHead },
        { "damage influence +2C3810", DLVR_PG_GD_RVA(kInfluenceRva), kInflHead },
    };
    for (const Site& s : sites) {
        if (!HeadMatches(base + s.rva, s.head)) {
            g_resolved = -1;
            // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
            LOG("pushgrab: [PUSHGRAB] REFUSED - %s at gamedll+%llX does not start with the Steam 97046bba bytes (not a known build, or "
                "no Epic table entry): enemy push and grab off for this launch", s.name, static_cast<unsigned long long>(s.rva));
            return false;
        }
    }
    g_hit = reinterpret_cast<HitFn>(base + sites[0].rva);
    g_itemByName = reinterpret_cast<ItemByNameFn>(base + sites[1].rva);
    g_shouldFall = reinterpret_cast<ShouldFallFn>(base + sites[2].rva);
    g_influence = reinterpret_cast<InfluenceFn>(base + sites[3].rva);
    g_gdBase = base;
    char rangeStatus[224] = "";
    InstallRange(base, rangeStatus, sizeof(rangeStatus));
    g_resolved = 1;
    // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
    LOG("pushgrab: [PUSHGRAB] READY (logged unconditionally with enemy_push_grab=1) - 4 gamedll functions byte-checked (hit +%llX, "
        "item lookup +%llX, should-fall +%llX, influence +%llX), single-player test GameDI+0x540 -> +0xD0 | push: min %.2f m/s, knock-down %.2f "
        "m/s, re-arm %.2f s, grab intent at squeeze %.2f, reach %.0f mm | grab: break-free %.2f s, drop below %.2f m/s, waits %.2f s for the game's "
        "knock-down, single-player only | zombie range cap %.2f m %s | thread %lu", static_cast<unsigned long long>(sites[0].rva),
        static_cast<unsigned long long>(sites[1].rva), static_cast<unsigned long long>(sites[2].rva),
        static_cast<unsigned long long>(sites[3].rva), static_cast<double>(PushMinMps), static_cast<double>(PushKnockdownMps),
        static_cast<double>(PushRearmS), static_cast<double>(PushGrabIntentSqueeze), static_cast<double>(PushGrabReachM * 1000.0f),
        static_cast<double>(GrabBreakFreeS),
        static_cast<double>(GrabDropMps), kFallWaitS, static_cast<double>(PushZombieMeleeRangeM), rangeStatus, GetCurrentThreadId());
    return true;
}

// The item, once the ItemManager has it (the main menu may not). Logged on the first success and on the first failure.
bool CrowdBumpItem()
{
    if (g_crowdBump) { return true; }
    static bool s_saidFail = false;
    void* it = nullptr;
    const bool ok = CallItemByName("Melee_CrowdBump", &it);
    if (ok && it) {
        g_crowdBump = it;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] item Melee_CrowdBump resolved (logged unconditionally) - %p", it);
        return true;
    }
    if (!s_saidFail) {
        s_saidFail = true;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] item Melee_CrowdBump NOT resolved yet (%s) - no push until it is; retried every push", ok ? "lookup returned null" : "lookup faulted");
    }
    return false;
}

// The light shove item (our Data3 Melee_VRShove). Looked up until found or the lookup answers null twice after the crowd bump resolved
// (= not in this Data3); named once either way.
void* LightShoveItem()
{
    if (g_lightShove) { return g_lightShove; }
    static int s_misses = 0;
    static bool s_said = false;
    if (s_misses >= 2) { return nullptr; }
    void* it = nullptr;
    const bool ok = CallItemByName("Melee_VRShove", &it);
    if (ok && it) {
        g_lightShove = it;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] item Melee_VRShove resolved (logged unconditionally) - %p: shoves under %.2f m/s are LIGHT", it,
            static_cast<double>(PushHardMps));
        return it;
    }
    if (++s_misses >= 2 && !s_said) {
        s_said = true;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=yes
        LOG("pushgrab: [PUSHGRAB] item Melee_VRShove NOT FOUND (%s) - this Data3 is not 394248e7; every shove uses Melee_CrowdBump "
            "(medium)", ok ? "lookup returned null" : "lookup faulted");
    }
    return nullptr;
}

void Skip(int a, const HandContact& c, const char* why)
{
    ++g_a[a].skips;
    if (EventLine()) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
        LOG("pushgrab: [PUSHGRAB] SKIP %s hand - enemy %p id %d: %s", ArmName(a), c.ai, c.aiId, why);
    }
}

// haveSq / sq: that hand's squeeze at the moment the push fired (logged on the PUSH line - spec §3).
void Push(int a, const HandContact& c, const pg::PushResult& r, bool haveSq, float sq)
{
    if (!HandCollisionRevalidate(c)) { Skip(a, c, "no longer the live, standing enemy the engine knows by its id"); return; }
    const int enemy = CallIsEnemy(c.ai, c.player);
    if (enemy != 1) { Skip(a, c, enemy < 0 ? "the enemy test (vfunc +336) faulted" : "not an enemy of the player (the body bump's own gate)"); return; }
    if (!CrowdBumpItem()) { Skip(a, c, "the Melee_CrowdBump item is not resolved"); return; }
    uintptr_t meleeCtl = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(c.player) + kMeleeCtlOff, &meleeCtl) || !meleeCtl) {
        Skip(a, c, "the player's melee controller (+3088) is unreadable");
        return;
    }
    float origin[3] = {};
    pg::OriginForDirection(c.point, r.dir, kOriginBackM, origin);
    // a normal shove is LIGHT (the small flinch), a hard one or a knock-down the game's medium body bump (RESEARCH 10q / 10r)
    void* const light = (r.tier == pg::PushTier::Shove && r.speed < PushHardMps) ? LightShoveItem() : nullptr;
    void* const item = light ? light : g_crowdBump;
    const bool hitOk = CallHit(reinterpret_cast<void*>(meleeCtl), c.ai, item, origin);
    float infl = -2.0f;
    bool fell = false;
    if (hitOk && r.tier == pg::PushTier::KnockDown) {
        infl = CallInfluence(c.ai);
        fell = infl > 0.0f && CallShouldFall(c.ai);
    }
    ++g_a[a].pushes[static_cast<int>(r.tier)];
    if (EventLine()) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=no
        LOG("pushgrab: [PUSHGRAB] PUSH %s hand - enemy %p id %d, body %d | %s (%s) at %.2f m/s (arm: hand minus head, 3D), dir (%.2f %.2f %.2f) | hit %s | knock-down: %s"
            " | squeeze %.2f%s | reach gap %.0f mm (0 = touching)",
            ArmName(a), c.ai, c.aiId, c.bodyIndex, TierName(r.tier), light ? "light - Melee_VRShove" : "medium - Melee_CrowdBump",
            static_cast<double>(r.speed), static_cast<double>(r.dir[0]),
            static_cast<double>(r.dir[1]), static_cast<double>(r.dir[2]), hitOk ? "sent" : "FAULTED",
            r.tier != pg::PushTier::KnockDown ? "no (not that hard)" : !hitOk ? "no (the hit faulted)"
            : infl < 0.0f ? "no (influence unreadable)" : infl == 0.0f ? "no (this enemy's PUSH influence is 0)"
            : fell ? "SetShouldFall sent" : "SetShouldFall FAULTED",
            static_cast<double>(haveSq ? sq : -1.0f), haveSq ? "" : " (not read)", static_cast<double>(c.gapM * 1000.0f));
    }
}

void GrabLine(int a, const char* what, const PubContact& c, const char* detail)
{
    if (EventLine()) {
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=no
        LOG("pushgrab: [PUSHGRAB] GRAB %s %s hand - enemy %p id %d | %s", what, ArmName(a), c.ai, c.id, detail);
    }
}

void GrabIdle(int a)
{
    g_gs[a] = GrabState{};
    InterlockedExchange(&g_gameActive[a], 0);
}

// A grip press on a contact: re-check, the co-op and enemy tests, then ask the game to knock the enemy down. COST (per press): the
// enemy state read, two virtual calls, one game call.
void GrabStart(int a, const PubContact& c, double now)
{
    GrabState& s = g_gs[a];
    if (s.phase != Phase::Idle) { GrabLine(a, "REFUSED", c, "this hand is already grabbing"); return; }
    if (BreakoutPlayerGrabbed()) { GrabLine(a, "REFUSED", c, "the player is being grabbed - that is the breakout's job (spec 1.10)"); return; }
    const int st = HandCollisionAiState(c.ai, c.id, c.player);
    if (st != 0) {
        GrabLine(a, "REFUSED", c, st < 0 ? "no longer the enemy the engine knows by its id" : st == 1 ? "already down (standing only)"
                                                                                            : "dead");
        return;
    }
    const int enemy = CallIsEnemy(c.ai, c.player);
    if (enemy != 1) { GrabLine(a, "REFUSED", c, enemy < 0 ? "the enemy test faulted" : "not an enemy of the player"); return; }
    uintptr_t sessVt = 0;
    int solo = 0;
    const int sp = SessionSolo(&sessVt, &solo);
    if (sp != 1) {
        char why[192];
        if (sp < 0) {
            std::snprintf(why, sizeof(why), "the single-player test could not read the session (GameDI+0x540, vftable gamedll+%llX) - "
                          "fails closed", static_cast<unsigned long long>(sessVt ? sessVt - g_gdBase : 0));
        } else {
            std::snprintf(why, sizeof(why), "co-op: session +0xD0 = %d, not 0 - the grab is single-player only (Jay: \"Solo only, "
                          "say why\")", solo);
        }
        GrabLine(a, "REFUSED", c, why);
        return;
    }
    if (!CallShouldFall(c.ai)) { GrabLine(a, "REFUSED", c, "SetShouldFall faulted"); return; }
    s.phase = Phase::Waiting;
    s.c = c;
    s.startS = now;
    InterlockedExchange(&g_gameActive[a], 1);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "single player (session +0xD0 = 0) - SetShouldFall sent - waiting for the game's knock-down to "
                  "bring the ragdoll up | reach gap %.0f mm (0 = touching)", static_cast<double>(c.gapM * 1000.0f));
    GrabLine(a, "START", c, detail);
}

void GrabEnd(int a, bool release, double now)
{
    GrabState& s = g_gs[a];
    if (s.phase == Phase::Waiting) {
        GrabLine(a, "END", s.c, release ? "let go before the ragdoll came up (the enemy still falls)" : "broke free before the ragdoll came up");
        GrabIdle(a);
        return;
    }
    if (s.phase != Phase::Holding) { return; }
    float v[3] = {};
    const bool haveV = HandCollisionArmVelocity(a, v);
    s.throwIt = release && haveV && pushgrab::IsThrow(v, GrabDropMps);
    for (int i = 0; i < 3; ++i) { s.vel[i] = v[i]; }
    s.endWhy = release ? (s.throwIt ? "thrown" : "dropped (let go slowly)") : "broke free (the break-free timer)";
    BodyGrabEnd(BgHand(a), release ? "the open-hand grab let go" : "the open-hand grab's break-free timer");
    s.phase = Phase::ThrowPending;   // the velocity goes in once body grab has let go (its next update) - no spring fights it
    s.holdS = now;
}

// Per update while grabbing. COST: the enemy state read; a velocity call at the release.
void GrabTick(int a, double now)
{
    GrabState& s = g_gs[a];
    char buf[160];
    if (s.phase == Phase::Waiting) {
        const int st = HandCollisionAiState(s.c.ai, s.c.id, s.c.player);
        if (st == 1) {
            float at[3] = { s.c.point[0], s.c.point[1], s.c.point[2] };
            HandCollisionArmTip(a, at);   // the hand NOW (the body has moved since the press); the press point if none
            if (BodyGrabBeginLive(BgHand(a), s.c.ai, at)) {
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "the ragdoll came up %.0f ms after the press - body grab holds it at the hand",
                            (now - s.startS) * 1000.0);
                GrabLine(a, "HOLD", s.c, buf);
                s.phase = Phase::Holding;
                s.holdS = now;
                s.seenActive = false;
            } else {
                GrabLine(a, "END", s.c, "body grab refused the live grab at the call (body_grab off or bad arguments)");
                GrabIdle(a);
            }
        } else if (st < 0 || st == 2) {
            GrabLine(a, "END", s.c, st < 0 ? "the enemy is gone (id lookup / class)" : "the enemy died before its ragdoll came up");
            GrabIdle(a);
        } else if (now - s.startS > kFallWaitS) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "no ragdoll within %.2f s - the game did not knock it down (its state or data)",
                        kFallWaitS);
            GrabLine(a, "END", s.c, buf);
            GrabIdle(a);
        }
        return;
    }
    if (s.phase == Phase::Holding) {
        const bool active = BodyGrabHandActive(BgHand(a));
        if (active) { s.seenActive = true; return; }
        if (!s.seenActive && now - s.holdS < 0.1) { return; }   // body grab takes the queued START on its next update
        GrabLine(a, "END", s.c, s.seenActive ? "body grab let go by itself (the enemy got up, died or despawned - see body grab's END line)"
                                             : "body grab refused the live grab (see body grab's REFUSED line)");
        GrabIdle(a);
        return;
    }
    if (s.phase == Phase::ThrowPending) {
        if (BodyGrabHandActive(BgHand(a)) && now - s.holdS < 0.1) { return; }
        bool sent = false;
        if (s.throwIt && HandCollisionAiState(s.c.ai, s.c.id, s.c.player) == 1) { sent = CallAddLinVel(s.c.ai, s.vel); }
        const float sp = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s | hand %.2f m/s (%.2f %.2f %.2f) | ragdoll velocity %s", s.endWhy,
                    static_cast<double>(sp), static_cast<double>(s.vel[0]), static_cast<double>(s.vel[1]), static_cast<double>(s.vel[2]),
                    !s.throwIt ? "not sent" : sent ? "sent (HumanAI slot 307)" : "NOT sent (the enemy is no longer down, or faulted)");
        GrabLine(a, "END", s.c, buf);
        GrabIdle(a);
    }
}

}  // namespace

// F7 (docs/AUDIT-COOP-PER-PLAYER-2026-11-13.md, trace 20261113_1810): the same single-player test the grab uses, for pumpsound's
// co-op fallback. 1 = single player (session +0xD0 == 0, measured in play, STATE 7595), 0 = co-op, -1 = unreadable (incl. before
// this file resolved the gamedll base). COST: four guarded reads.
int PushGrabSessionSolo()
{
    uintptr_t vt = 0;
    int flag = -1;
    return SessionSolo(&vt, &flag);
}

// COST per player update: off = one bool test. On: 2 contact copies; per contact one classification (a few float ops); per push
// event (human rate) the id lookup + live check, one virtual call (the enemy test), one game call (the hit), on a knock-down two
// more (the influence, SetShouldFall). Nothing resolved, no RTTI walk, no level query per update.
void PushGrabTick()
{
    if (!EnemyPushGrab || !SwitchesOn()) {
        for (int a = 0; a < 2; ++a) {
            g_a[a].wasContact = false;
            g_trk[a].have = false;
            g_trk[a].meter.Reset();
            if (g_gs[a].phase == Phase::Holding) { BodyGrabEnd(BgHand(a), "enemy_push_grab or a needed switch went off"); }
            if (g_gs[a].phase != Phase::Idle) { GrabIdle(a); }
            InterlockedExchange(&g_cmd[a], 0);
        }
        return;
    }
    if (!Resolve()) { return; }
    const double now = NowS();
    const pg::PushParams p = { PushMinMps, PushKnockdownMps, PushRearmS };
    // Final review I3: while a zombie holds the player, its shove is the breakout's (spec 1.10) - no push, no new grab.
    const bool playerGrabbed = BreakoutPlayerGrabbed();
    for (int a = 0; a < 2; ++a) {
        ArmState& s = g_a[a];
        // The ARM's own speed (2026-10-31 (6), Jay: "it was also factoring in the player running towards a zombie as arm
        // acceleration"; PG3-PG5 13 of 18 shoves had hand-minus-head < 1.2 m/s): the hand MINUS the head in the tracking space, 3D,
        // new poses only, over 50-150 ms (pushgrab_logic.h ArmSpeedMeter) - body moves, turns, roomscale and recentres cancel.
        // COST: two lock-free pose reads, a 32-entry ring and a few float ops per arm.
        Tracked& tk = g_trk[a];
        XrPosef pose{}, headPose{};
        if (GetControllerPose(a == 0 ? kRightHand : kLeftHand, pose) && GetHeadPose(headPose)) {
            const float rel[3] = { pose.position.x - headPose.position.x, pose.position.y - headPose.position.y,
                                   pose.position.z - headPose.position.z };
            tk.meter.Add(now, rel);
            tk.speed = tk.meter.Speed(now);
            tk.have = true;
        } else {
            tk.meter.Reset();
            tk.have = false;
            tk.speed = 0.0f;
        }
        HandContact c{};
        const bool in = HandCollisionContact(a, &c);
        if (in) {   // for the pad thread's grab machine
            InterlockedIncrement(&g_pubSeq[a]);
            g_pub[a] = PubContact{ c.ai, c.aiId, c.player, { c.point[0], c.point[1], c.point[2] }, c.tS, c.gapM };
            InterlockedIncrement(&g_pubSeq[a]);
        }
        // the grab: the pad thread's command first, then this update's step
        const LONG cmd = InterlockedExchange(&g_cmd[a], 0);
        if (cmd == 1) { GrabStart(a, g_startC[a], now); }
        else if (cmd == 2 || cmd == 3) { GrabEnd(a, cmd == 2, now); }
        GrabTick(a, now);
        // a hand that is grabbing (or whose grip the grab owns) does not push
        const bool grabbing = g_gs[a].phase != Phase::Idle || g_padHolding[a] != 0 || playerGrabbed;
        pg::PushResult r{ pg::PushTier::None, 0.0f, { 0.0f, 0.0f, 0.0f } };
        if (in) {
            r = pg::ClassifyPushTracked(c.realVel, tk.speed, c.outwardNormal, p);
            if (!s.wasContact) { ++s.contacts; }
            if (r.speed > s.maxSpeed) { s.maxSpeed = r.speed; }
        }
        s.wasContact = in;
        if (s.arm.Fire(in, !grabbing && r.tier != pg::PushTier::None, now, PushRearmS)) {
            // Spec §3: a hand already closing when it would shove is reaching to grab. Fire has spent the arm either way, so this
            // contact does not push later. COST: one global read, only when a push fires.
            float sq = 0.0f;
            const bool haveSq = GetControllerSqueeze(a == 0 ? kRightHand : kLeftHand, sq);
            if (pg::GrabIntent(g_canGrab[a] != 0, haveSq, sq, PushGrabIntentSqueeze)) {
                char why[96];
                std::snprintf(why, sizeof(why), "grip closing, squeeze %.2f >= %.2f - a grab, not a push", static_cast<double>(sq),
                              static_cast<double>(PushGrabIntentSqueeze));
                Skip(a, c, why);
            } else {
                Push(a, c, r, haveSq, sq);
            }
        }
        if (now - s.winS >= 1.0) {
            if (s.contacts && WindowLine()) {
                // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
                LOG("pushgrab: [PUSHGRAB] 1 s %s hand | contacts %ld | pushes: shove %ld, knock-down %ld | skips %ld | max speed %.2f m/s",
                    ArmName(a), s.contacts, s.pushes[1], s.pushes[2], s.skips, static_cast<double>(s.maxSpeed));
            }
            s.winS = now;
            s.contacts = s.skips = 0;
            s.pushes[0] = s.pushes[1] = s.pushes[2] = 0;
            s.maxSpeed = 0.0f;
        }
    }
    // The zombie range window (spec §2): what the detour saw in the last second, only when it saw anything.
    static double s_rngWinS = 0.0;
    if (g_rangeState == 1 && now - s_rngWinS >= 1.0) {
        s_rngWinS = now;
        const LONG calls = InterlockedExchange(&g_rngCalls, 0);
        const LONG onPlayer = InterlockedExchange(&g_rngPlayer, 0);
        const LONG capped = InterlockedExchange(&g_rngCapped, 0);
        const float lo = FromBits(InterlockedExchange(&g_rngMinBits, 0x7F7FFFFF));
        const float hi = FromBits(InterlockedExchange(&g_rngMaxBits, 0));
        if (onPlayer && CapLine(&g_nRange, kMaxRangeLines, "zombie range 1 s")) {
            // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=yes
            LOG("pushgrab: [PUSHGRAB] zombie range 1 s | calls %ld, ordinary zombie -> player %ld, capped %ld | original %.2f-%.2f m "
                "(zombie -> player only) | cap %.2f m", calls, onPlayer, capped, static_cast<double>(onPlayer ? lo : 0.0f),
                static_cast<double>(hi), static_cast<double>(PushZombieMeleeRangeM));
        }
    }
}

namespace {
bool ReadF3(uintptr_t a, float out[3])
{
    __try { for (int i = 0; i < 3; ++i) { out[i] = reinterpret_cast<const float*>(a)[i]; } return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteF3(uintptr_t a, const float v[3])
{
    __try { for (int i = 0; i < 3; ++i) { reinterpret_cast<float*>(a)[i] = v[i]; } return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The player's AwayFromAIHelper size (INV-669). Pad thread only (one caller - no race). COST per poll: one module handle lookup until
// found, then 3 float reads and compares; a write only when the wanted size changes.
void ApplyHelperSize()
{
    static uintptr_t s_at = 0;
    static float s_ours[3] = {};
    static bool s_haveOurs = false, s_refused = false;
    static int s_lastSaid = -2;   // the last state logged: 1 shrunk, 0 the game's, -1 refused
    if (s_refused) { return; }
    if (!s_at) {
        const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
        if (!gd) { return; }
        s_at = reinterpret_cast<uintptr_t>(gd) + DLVR_PG_GD_RVA(kHelperSizeRva);
    }
    float cur[3] = {}, out[3] = {}, kHelperOrig[3] = {};
    pushgrab::HelperGameSize(kHelperOrig);   // the game's exact bits (RUN PG7: 0.6f / 0.4f never matched them)
    if (!ReadF3(s_at, cur)) { return; }
    const bool on = EnemyPushGrab;
    const int plan = pushgrab::HelperPlan(on, PlayerAiHelperRadiusM, cur, kHelperOrig, s_haveOurs ? s_ours : nullptr, out);
    if (plan < 0) {
        s_refused = true;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=no
        LOG("pushgrab: [PUSHGRAB] AI HELPER REFUSED - gamedll+%llX reads (%.3f %.3f %.3f), neither the game's (0.6 0.4 0.6) nor ours: not the "
            "Steam / Epic build INV-669 was read from - the player's AwayFromAIHelper is left as the game has it",
            static_cast<unsigned long long>(kHelperSizeRva), static_cast<double>(cur[0]), static_cast<double>(cur[1]),
            static_cast<double>(cur[2]));
        return;
    }
    bool wrote = false;
    if (plan == 1) {
        wrote = WriteF3(s_at, out);
        if (wrote) {
            for (int i = 0; i < 3; ++i) { s_ours[i] = out[i]; }
            s_haveOurs = true;
        }
    }
    const int state = (out[0] < kHelperOrig[0]) ? 1 : 0;
    if (plan == 1 || state != s_lastSaid) {
        s_lastSaid = state;
        // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=FLAT  log-only=no
        // [LOADBEARING: player_ai_helper_radius_m] the gap - the player's own AI keep-off shape, built smaller (INV-669)
        LOG("pushgrab: [PUSHGRAB] AI HELPER %s (logged on change) - gamedll+%llX (%.3f %.3f %.3f) -> (%.3f %.3f %.3f)%s | "
            "enemy_push_grab=%d, player_ai_helper_radius_m %.2f | a helper the game has already built keeps its size until it is remade",
            state ? "SHRUNK" : "THE GAME's SIZE", static_cast<unsigned long long>(kHelperSizeRva), static_cast<double>(cur[0]),
            static_cast<double>(cur[1]), static_cast<double>(cur[2]), static_cast<double>(out[0]), static_cast<double>(out[1]),
            static_cast<double>(out[2]), plan == 1 ? (wrote ? " written" : " WRITE FAULTED") : " (already)", on ? 1 : 0,
            static_cast<double>(PlayerAiHelperRadiusM));
    }
}
}  // namespace

// COST per pad poll: off / not READY = one bool test; on = 2 squeeze reads, 2 seqlocked contact copies, the machine (a few compares).
void PushGrabPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1)
{
    withholdL1 = false;
    withholdR1 = false;
    ApplyHelperSize();   // INV-669 - every poll, menus included, so the helper is built at the wanted size
    if (!EnemyPushGrab || g_resolved != 1 || !vrGrip || !HandCollision || !BodyGrab || !LootByHand) {
        for (int a = 0; a < 2; ++a) {
            InterlockedExchange(&g_padHolding[a], 0);
            InterlockedExchange(&g_canGrab[a], 0);
            g_latch[a].Update(false, false);   // nothing withheld past a switch-off
        }
        return;
    }
    static bool s_saidWithhold[2] = {};
    const double now = NowS();
    for (int a = 0; a < 2; ++a) {
        const bool owned = a == 0 ? rightOwned : leftOwned;
        InterlockedExchange(&g_canGrab[a], owned ? 0 : 1);
        float sq = 0.0f;
        const bool grip = GetControllerSqueeze(a == 0 ? kRightHand : kLeftHand, sq) && sq >= kGripPress;
        PubContact c = {};
        bool fresh = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG s0 = g_pubSeq[a];
            if (s0 & 1) { continue; }
            c = g_pub[a];
            if (g_pubSeq[a] == s0) { fresh = c.ai && now - c.tS <= kContactFreshS; break; }
        }
        // a press another feature owns is never a grab; once holding, the grip is ours until it opens
        const bool gripDown = g_m[a].Holding() ? grip : (grip && !owned);
        static bool s_prevDown[2] = {};
        if (gripDown && !s_prevDown[a] && !fresh && !g_m[a].Holding()) { MissLine(a, now); }
        s_prevDown[a] = gripDown;
        const pushgrab::GrabEvent e = g_m[a].Update(gripDown, fresh, now, GrabBreakFreeS);
        if (e == pushgrab::GrabEvent::Start) {
            g_startC[a] = c;
            InterlockedExchange(&g_cmd[a], 1);
        } else if (e == pushgrab::GrabEvent::Release) {
            InterlockedExchange(&g_cmd[a], 2);
        } else if (e == pushgrab::GrabEvent::BreakFree) {
            InterlockedExchange(&g_cmd[a], 3);
        }
        // Final review I1: withheld from the START until the grip opens (break-free and a refused start included), not only while held
        const bool hold = g_latch[a].Update(e == pushgrab::GrabEvent::Start, grip);
        InterlockedExchange(&g_padHolding[a], hold ? 1 : 0);
        if (hold) {
            if (a == 0) { withholdR1 = true; } else { withholdL1 = true; }
            if (!s_saidWithhold[a]) {
                s_saidWithhold[a] = true;
                // [PROBE: PUSHGRAB] gate=enemy_push_grab  site=HEADSET  log-only=no
                LOG("pushgrab: [PUSHGRAB] WITHHOLD %s grip (logged once per hand, unconditionally) - the press that started a grab is not "
                    "sent to the game as %s while the grab holds", ArmName(a), a == 0 ? "R1" : "L1");
            }
        }
    }
}

bool PushGrabHandActive(int bodyGrabHand)
{
    if (bodyGrabHand < 0 || bodyGrabHand > 1) { return false; }
    const int a = 1 - bodyGrabHand;
    return g_gameActive[a] != 0 || g_padHolding[a] != 0;
}

}  // namespace dlvr
