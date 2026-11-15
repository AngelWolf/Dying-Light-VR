// camogesture.cpp - the camouflage gesture: wipe an infected corpse (or a limb attached to / cut from one) held by body grab ACROSS
// the face -> the game's own camouflage starts, without the Camouflage skill, once per corpse. Spec
// docs/superpowers/specs/2026-11-07-camouflage-gesture-design.md, plan docs/superpowers/plans/2026-11-07-camouflage-gesture.md
// Task 2, facts INV-704 (+ its Task 0 addendum). The rules (the wipe, the decision, the limb record) are camo_logic.h, unit-tested.
//
// WHAT IS CALLED (OUR gamedll 97046bba; the Epic build's rvas from buildrva_table.h family 15 in the BETA tree; any other build: every
// function's head is compared before anything is called - a mismatch REFUSES by name):
//   +CDD020 PlayerCamouflageModule::Start(module, float dur, char 0, char 0) - no skill test (INV-704)
//   +30C160 the duration (player -> float seconds; its first argument is unused in our build)
//   +BDE2F0 CanUseCamouflage(player, 1) ONLY - argument 0 tests the skill and is never passed here (anti-goal)
//   +45DC10 the trigger offer's GameDI gate (GameDI = *(gamedll + 0x1C16348))
//   +BE4F60 OnSkillUsed(player, 16) after a start, as the game's action-53 handler +30C060 does (Task 0 ruling)
//   engine IModelObject::IsObjectDissolved / IsRenderable on the corpse (the game's own conditions, +30BE80)
// WHAT IS DETOURED: the relict spawner +21B480 (14 head bytes, no rip-relative): its callers are AIVis methods, a1 - 3488 = the Vis,
//   *(vis + 80) = the AI, accepted only when the AI's slot 194 (+191C50, the vis getter) returns that same vis; the relicts one call
//   makes are the entries appended to the list at a1 + 224 (count a1 + 232). Our relicts carry no owner (INV-704), so this is the
//   only place the cut limb -> its corpse link exists.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"
#include "bodygrab.h"      // BodyGrab, BodyGrabHandActive, BodyGrabHeldObject, BodyGrabObjectId / Alive, BodyGrabPlayer
#include "boneblend.h"     // InstallSingleGameDetour - the shared verify / suspend / commit path
#include "camo_logic.h"
#include "camogesture.h"
#include "headpose.h"      // GetHeadPose
#include "looting.h"       // LootByHand, LootInfectedModule

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_CAMO_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_CAMO_BUILD_NAME() ::dlvr::buildrva::BuildName(::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll))
#else
#define DLVR_CAMO_GD_RVA(rva) (rva)
#define DLVR_CAMO_BUILD_NAME() "STEAM (DEV tree: Steam rvas only)"
#endif

namespace dlvr {

// [DEP: camo_gesture] required-for=the camouflage gesture master (CamoGesture): the wipe detector (CamoGesturePadPoll), the limb
//      detour (+21B480) and the start (CamoGestureTick). Needs body_grab=1 + loot_by_hand=1 (both ship ON). Compiled ON - Jay
//      2026-11-07 after RUN CG1 (INV-705, "bodies do work"): "Ship on." tree=INERT at 0 (no detour installed, one bool test per poll /
//      update).
bool  CamoGesture = true;
// [DEP: camo_wipe_fwd_m] required-for=the face zone's depth in front of the head (camo::WipeParams.fwdM). Tier D. tree=INERT at
//      camo_gesture=0. Compiled = Jay's disk tuning (2026-11-07 RUN CG2, "Much better"; was 0.25).
float CamoWipeFwdM = 0.35f;
// [DEP: camo_wipe_radius_m] required-for=the face zone's half-width / half-height (camo::WipeParams.radiusM). Tier D. tree=INERT at
//      camo_gesture=0. Compiled = Jay's disk tuning (2026-11-07 RUN CG2, "Much better"; was 0.15).
float CamoWipeRadiusM = 0.20f;
// [DEP: camo_wipe_sweep_m] required-for=the sideways travel inside the zone that makes a wipe (camo::WipeParams.sweepM). Tier D.
//      tree=INERT at camo_gesture=0. Compiled = Jay's disk tuning (2026-11-07 RUN CG2, "Much better"; was 0.12).
float CamoWipeSweepM = 0.08f;
// [DEP: camo_wipe_max_s] required-for=the time from entering the zone within which the travel must happen (camo::WipeParams.maxS).
//      Tier D. tree=INERT at camo_gesture=0. Compiled = Jay's disk tuning (2026-11-07 RUN CG2, "Much better"; was 1.0).
float CamoWipeMaxS = 1.5f;

namespace {

constexpr uintptr_t kSpawnRva     = 0x21B480;    // relict spawner (detoured)
constexpr uintptr_t kStartRva     = 0xCDD020;    // PlayerCamouflageModule::Start
constexpr uintptr_t kDurRva       = 0x30C160;    // the camouflage duration
constexpr uintptr_t kModeRva      = 0xBDE2F0;    // CanUseCamouflage - called with 1 only
constexpr uintptr_t kGateRva      = 0x45DC10;    // the trigger offer's GameDI gate
constexpr uintptr_t kSkillUsedRva = 0xBE4F60;    // OnSkillUsed
constexpr uintptr_t kGameDiRva    = 0x1C16348;   // GameDI* global
constexpr uintptr_t kVisGetRva    = 0x191C50;    // HumanAI slot 194, the vis getter (compare)
constexpr uintptr_t kCamoModVtRva = 0x1729338;   // PlayerCamouflageModule vftable (compare, cf3)
constexpr size_t    kVisGetSlot   = 194;
constexpr size_t    kVisOwnerOff  = 80;          // vis + 80 = its AI (bodygrab.cpp kVisOwnerOff)
constexpr uintptr_t kA1ToVis      = 3488;        // the spawner's a1 = vis + 3488
constexpr size_t    kRelListOff   = 224;         // a1 + 224 = the relict list's data, + 232 = its count
constexpr size_t    kRelCountOff  = 232;
constexpr size_t    kCamoModOff   = 5032;        // player + 5032 = the PlayerCamouflageModule (+30C060, +BDE2F0)
constexpr size_t    kOnceOff      = 5713;        // AI + 5713 = the once-per-corpse flag (+30BE80 tests, +30C060 clears)
constexpr size_t    kCamoTimeOff  = 408;         // infected module + 408 > 1.5 (+30BE80)
constexpr int       kSkillIdx     = 16;          // +30C060: OnSkillUsed(player, 53 - 37)
constexpr unsigned  kMaxPerCut    = 64;          // more relicts than this from one call = a misread count (refused, counted)
constexpr long      kMaxEventLines = 200;        // INV-9 cap on per-event lines
const char* const kExpDissolved  = "?IsObjectDissolved@IModelObject@@QEBA_NXZ";   // engine +148670
const char* const kExpRenderable = "?IsRenderable@IModelObject@@QEBA_NXZ";        // engine +142980

// the spawner's 14 head bytes: mov r11,rsp / mov [r11+20h],r9d / mov [r11+8],rcx / push rbp / push r13 (cf11)
const unsigned char kSpawnHead[14] = { 0x4C, 0x8B, 0xDC, 0x45, 0x89, 0x4B, 0x20, 0x49, 0x89, 0x4B, 0x08, 0x55, 0x41, 0x55 };

// 32 head bytes of each called function (cf11, OUR gamedll 97046bba); a mask bit 0 = a rip-relative / call displacement, not compared.
struct Head {
    const char* name;
    uintptr_t steamRva;
    unsigned char b[32];
    uint32_t mask;
};
const Head kHeads[5] = {
    { "Start +CDD020", kStartRva,
      { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48,
        0x8B, 0x05, 0x12, 0x93, 0xF3, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x41, 0x0F }, 0xFFC3FFFFu },
    { "duration +30C160", kDurRva,
      { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA0,
        0x00, 0x00, 0x00, 0x44, 0x0F, 0x29, 0x58, 0x98, 0x45, 0x0F, 0x57, 0xDB, 0x48, 0x8B, 0xDA, 0x48 }, 0xFFFFFFFFu },
    { "CanUseCamouflage +BDE2F0", kModeRva,
      { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D, 0x48, 0x80, 0x03, 0x01,
        0x48, 0x8B, 0x81, 0x50, 0x0C, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x18, 0x48, 0x8B, 0x40, 0x50 }, 0xFFFF0FFFu },
    { "offer gate +45DC10", kGateRva,
      { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x89, 0x70, 0x0C, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x10,
        0xE8, 0x2B, 0x30, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x07, 0xB0, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3 }, 0xFFE1FFFFu },
    { "OnSkillUsed +BE4F60", kSkillUsedRva,
      { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
        0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xB9, 0x80, 0x13, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x48, 0x8B }, 0xFFFFFFFFu },
};

using StartFn   = void(__fastcall*)(void* module, float dur, char a3, char a4);
using DurFn     = float(__fastcall*)(void* unused, void* player);
using ModeFn    = char(__fastcall*)(void* player, char a2);
using GateFn    = bool(__fastcall*)(void* gameDi);
using SkillFn   = void(__fastcall*)(void* player, int idx);
using VisGetFn  = void*(__fastcall*)(void* ai);
using ModelQFn  = bool(__fastcall*)(const void* model);
using SpawnFn   = __int64(__fastcall*)(__int64 a1, void* a2, __int64 a3, int a4, float* a5, unsigned a6, __int64 a7, char a8,
                                       char a9, char a10, float a11);

uintptr_t g_gd = 0;
uintptr_t g_rStart = 0, g_rDur = 0, g_rMode = 0, g_rGate = 0, g_rSkill = 0, g_rGameDi = 0, g_rVisGet = 0, g_rCamoVt = 0;
ModelQFn  g_dissolved = nullptr, g_renderable = nullptr;
SpawnFn   g_spawnTrue = nullptr;
LONG      g_state = 0;          // 0 not tried, 1 live, -1 refused (game thread only)
bool      g_detourLive = false;

// pad thread -> game thread
volatile LONG g_wipeSeq[2] = {};
volatile LONG g_wipeSpanUm[2] = {};   // the wipe's sideways span, micrometres (for the line)
camo::WipeState g_wipe[2] = {};       // pad thread only
volatile LONG g_firstPad = 0;
// game thread only
LONG g_seen[2] = {};
// the limb record: written by the detour (whichever thread the game cuts on), read by the game thread
SRWLOCK g_limbLock = SRWLOCK_INIT;
camo::LimbOwners g_limbs;
// tallies
volatile LONG g_nCuts = 0, g_nLimbs = 0, g_nLimbsInfected = 0, g_nCutRefused = 0, g_nCutFault = 0, g_firstCut = 0;
long g_nEvents = 0, g_nStarted = 0, g_nRefused[8] = {}, g_nLines = 0;
long g_lastSumEvents = -1, g_lastSumLimbs = -1;
ULONGLONG g_lastSumMs = 0;

bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadU32(uintptr_t a, uint32_t* out)
{
    __try { *out = *reinterpret_cast<const uint32_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadF32(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadU8(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteU8(uintptr_t a, unsigned char v)
{
    __try { *reinterpret_cast<unsigned char*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadHead(uintptr_t a, unsigned char* out32)
{
    __try { memcpy(out32, reinterpret_cast<const void*>(a), 32); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallVisGet(void* ai, void** vis)
{
    __try { *vis = reinterpret_cast<VisGetFn>(g_gd + g_rVisGet)(ai); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallModelQ(ModelQFn fn, const void* model, bool* out)
{
    __try { *out = fn(model); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallMode(void* player, bool* out)
{
    __try { *out = reinterpret_cast<ModeFn>(g_gd + g_rMode)(player, 1) != 0; return true; }   // argument 1 ONLY (no skill test)
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallGate(void* gameDi, bool* out)
{
    __try { *out = reinterpret_cast<GateFn>(g_gd + g_rGate)(gameDi); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallDur(void* player, float* out)
{
    __try { *out = reinterpret_cast<DurFn>(g_gd + g_rDur)(nullptr, player); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallStart(void* module, float dur)
{
    __try { reinterpret_cast<StartFn>(g_gd + g_rStart)(module, dur, 0, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSkillUsed(void* player)
{
    __try { reinterpret_cast<SkillFn>(g_gd + g_rSkill)(player, kSkillIdx); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The AI a spawner call's a1 belongs to: vis = a1 - 3488, AI = *(vis + 80), accepted only when the AI's slot 194 is the vis getter
// (+191C50) and it returns that same vis. 0 = not proven (the limb is then not recorded). COST: three reads + one game call, per cut.
uintptr_t AiOfSpawner(uintptr_t a1)
{
    if (a1 <= kA1ToVis) { return 0; }
    const uintptr_t vis = a1 - kA1ToVis;
    uintptr_t ai = 0, vt = 0, fn = 0;
    void* back = nullptr;
    if (!ReadPtr(vis + kVisOwnerOff, &ai) || !ai || !ReadPtr(ai, &vt) || !ReadPtr(vt + 8 * kVisGetSlot, &fn) ||
        fn != g_gd + g_rVisGet || !CallVisGet(reinterpret_cast<void*>(ai), &back)) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(back) == vis ? ai : 0;
}

// COST per call of the game's relict spawner (a cut / gore burst, not per frame): two count reads, the call-through, and per new relict
// one record under the lock; once per call the owner proof (AiOfSpawner) + the infected test (LootInfectedModule) + one engine id read.
__int64 __fastcall SpawnHook(__int64 a1, void* a2, __int64 a3, int a4, float* a5, unsigned a6, __int64 a7, char a8, char a9, char a10,
                             float a11)
{
    const uintptr_t base = static_cast<uintptr_t>(a1);
    uint32_t before = 0;
    const bool okBefore = CamoGesture && ReadU32(base + kRelCountOff, &before);
    const __int64 r = g_spawnTrue(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    if (!CamoGesture) { return r; }
    InterlockedIncrement(&g_nCuts);
    uint32_t after = 0;
    uintptr_t list = 0;
    if (!okBefore || !ReadU32(base + kRelCountOff, &after) || !ReadPtr(base + kRelListOff, &list) || after < before ||
        after - before > kMaxPerCut || (after > before && !list)) {
        InterlockedIncrement(&g_nCutFault);
        return r;
    }
    const uintptr_t ai = after > before ? AiOfSpawner(base) : 0;
    uintptr_t module = 0;
    const bool infected = ai && LootInfectedModule(ai, &module);
    int id = 0;
    const bool haveId = ai && BodyGrabObjectId(reinterpret_cast<void*>(ai), &id);
    int recorded = 0;
    if (after > before && !ai) {
        InterlockedIncrement(&g_nCutRefused);
    } else {
        for (uint32_t i = before; i < after; ++i) {
            uintptr_t rel = 0;
            if (!ReadPtr(list + 8 * static_cast<uintptr_t>(i), &rel) || !rel) { InterlockedIncrement(&g_nCutFault); continue; }
            // final review I2: the limb's OWN id - a later object at this address has another id, so the record is not inherited.
            // No id = the limb can never be matched again (not recorded, counted).
            int limbId = 0;
            if (!BodyGrabObjectId(reinterpret_cast<void*>(rel), &limbId)) { InterlockedIncrement(&g_nCutFault); continue; }
            AcquireSRWLockExclusive(&g_limbLock);
            // no corpse id = the corpse cannot be re-validated later: recorded with id -1, so its limbs keep their own single use
            g_limbs.Record(reinterpret_cast<void*>(rel), limbId, reinterpret_cast<void*>(ai), haveId ? id : -1, infected);
            ReleaseSRWLockExclusive(&g_limbLock);
            ++recorded;
            InterlockedIncrement(&g_nLimbs);
            if (infected) { InterlockedIncrement(&g_nLimbsInfected); }
        }
    }
    if (InterlockedCompareExchange(&g_firstCut, 1, 0) == 0) {
        // [PROBE: CAMOWIPE] gate=camo_gesture  site=FLAT  log-only=no
        LOG("camogesture: [CAMOWIPE] FIRST RELICT SPAWN (logged unconditionally) - list count %u -> %u, owner AI %p (%s), infected %d, "
            "engine id %s %d, %d limb(s) recorded | thread %lu",
            before, after, reinterpret_cast<void*>(ai), after == before ? "no new relict" : ai ? "proven: its vis getter returns the vis"
            : "*** NOT PROVEN - not recorded ***", infected ? 1 : 0, haveId ? "read" : "NOT READ", id, recorded, GetCurrentThreadId());
    }
    return r;
}

const char* Resolve()   // nullptr = live; else why it refused
{
    if (!BodyGrab || !LootByHand) { return "body_grab and loot_by_hand must both be 1 (the gesture reads body grab's hold)"; }
    HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return "gamedll / engine not loaded"; }
    g_rStart = DLVR_CAMO_GD_RVA(kStartRva);
    g_rDur = DLVR_CAMO_GD_RVA(kDurRva);
    g_rMode = DLVR_CAMO_GD_RVA(kModeRva);
    g_rGate = DLVR_CAMO_GD_RVA(kGateRva);
    g_rSkill = DLVR_CAMO_GD_RVA(kSkillUsedRva);
    g_rGameDi = DLVR_CAMO_GD_RVA(kGameDiRva);
    g_rVisGet = DLVR_CAMO_GD_RVA(kVisGetRva);
    g_rCamoVt = DLVR_CAMO_GD_RVA(kCamoModVtRva);
    g_gd = reinterpret_cast<uintptr_t>(gd);
    const uintptr_t rvas[5] = { g_rStart, g_rDur, g_rMode, g_rGate, g_rSkill };
    for (int i = 0; i < 5; ++i) {
        unsigned char got[32] = {};
        bool ok = ReadHead(g_gd + rvas[i], got);
        for (int k = 0; ok && k < 32; ++k) {
            if (((kHeads[i].mask >> k) & 1u) && got[k] != kHeads[i].b[k]) { ok = false; }
        }
        if (!ok) {
            // [PROBE: CAMOWIPE] gate=camo_gesture  site=FLAT  log-only=no
            LOG("camogesture: [CAMOWIPE] REFUSED - %s at gamedll +%llX: its 32 head bytes do not match OUR gamedll 97046bba's (build %s); "
                "nothing is called or patched, the gesture stays off this launch.", kHeads[i].name,
                static_cast<unsigned long long>(rvas[i]), DLVR_CAMO_BUILD_NAME());
            return "a called function's head bytes did not match (named above)";
        }
    }
    g_dissolved = reinterpret_cast<ModelQFn>(GetProcAddress(en, kExpDissolved));
    g_renderable = reinterpret_cast<ModelQFn>(GetProcAddress(en, kExpRenderable));
    if (!g_dissolved || !g_renderable) { return "the engine does not export IModelObject::IsObjectDissolved / IsRenderable"; }
    int attempts = 0;
    const uintptr_t at = g_gd + DLVR_CAMO_GD_RVA(kSpawnRva);
    g_detourLive = InstallSingleGameDetour(at, kSpawnHead, sizeof(kSpawnHead), reinterpret_cast<void*>(&SpawnHook),
                                           reinterpret_cast<void**>(&g_spawnTrue), "camouflage relict spawner", &attempts);
    // [PROBE: CAMOWIPE] gate=camo_gesture  site=FLAT  log-only=no
    LOG("camogesture: [CAMOWIPE] LIVE (logged unconditionally at camo_gesture=1) - build %s | start +%llX, duration +%llX, mode +%llX "
        "(argument 1 only), offer gate +%llX, skill-used +%llX, camouflage module vftable +%llX: heads MATCH | relict spawner detour "
        "+%llX %s (%d attempt(s)) | zone fwd %.2f m, radius %.2f m, sweep %.2f m within %.2f s | body_grab %d, loot_by_hand %d | "
        "thread %lu",
        DLVR_CAMO_BUILD_NAME(), static_cast<unsigned long long>(g_rStart), static_cast<unsigned long long>(g_rDur),
        static_cast<unsigned long long>(g_rMode), static_cast<unsigned long long>(g_rGate), static_cast<unsigned long long>(g_rSkill),
        static_cast<unsigned long long>(g_rCamoVt), static_cast<unsigned long long>(at - g_gd),
        g_detourLive ? "LIVE" : "*** REFUSED - cut limbs will read \"not recorded\" (the corpse wipe still works) ***", attempts,
        CamoWipeFwdM, CamoWipeRadiusM, CamoWipeSweepM, CamoWipeMaxS, BodyGrab ? 1 : 0, LootByHand ? 1 : 0, GetCurrentThreadId());
    return nullptr;
}

// The player's camouflage module, proven by its vftable. 0 = not proven.
uintptr_t CamoModuleOf(uintptr_t player)
{
    uintptr_t mod = 0, vt = 0;
    if (!player || !ReadPtr(player + kCamoModOff, &mod) || !mod || !ReadPtr(mod, &vt) || vt != g_gd + g_rCamoVt) { return 0; }
    return mod;
}

void Judge(int h)
{
    ++g_nEvents;
    bool relict = false;
    void* obj = BodyGrabHeldObject(h, &relict);
    const uintptr_t player = reinterpret_cast<uintptr_t>(BodyGrabPlayer());
    const uintptr_t camoMod = CamoModuleOf(player);
    camo::Facts f{};
    f.holding = obj != nullptr;
    f.readable = camoMod != 0;
    uintptr_t ai = 0;
    const char* via = "corpse";
    bool recorded = false, ownerGone = false;
    const void* owner = nullptr;
    int ownerId = 0, limbId = 0;
    bool recInf = false, recUsed = false;
    if (obj && relict) {
        AcquireSRWLockShared(&g_limbLock);
        recorded = g_limbs.Find(obj, &limbId, &owner, &ownerId, &recInf, &recUsed);
        ReleaseSRWLockShared(&g_limbLock);
        // final review I2: the entry is this limb only if the engine still knows this object by the id it had when it was cut
        if (recorded && !BodyGrabObjectAlive(obj, limbId)) {
            recorded = false;
            via = "limb NOT RECORDED (a different object now at a recorded limb's address)";
        } else if (!recorded) {
            via = "limb NOT RECORDED (cut before the detour, or its corpse not proven)";
        } else if (owner && ownerId >= 0 && BodyGrabObjectAlive(const_cast<void*>(owner), ownerId)) {
            ai = reinterpret_cast<uintptr_t>(owner);
            via = "limb -> its corpse";
        } else {
            ownerGone = true;
            via = "limb, its corpse gone (own use)";
        }
    } else if (obj) {
        ai = reinterpret_cast<uintptr_t>(obj);
    }
    uintptr_t module = 0;
    float camoTime = 0.0f;
    unsigned char once = 0;
    bool dissolved = false, renderable = true;
    if (ai) {
        f.infected = LootInfectedModule(ai, &module);
        if (f.infected) {
            const bool okTime = ReadF32(module + kCamoTimeOff, &camoTime);
            const bool okOnce = ReadU8(ai + kOnceOff, &once);
            f.readable = f.readable && okTime && okOnce;
            f.used = once == 0;
            bool okVis = true;
            if (!relict) {   // the game's own dissolved / renderable tests are on the corpse; a held limb skips them
                okVis = CallModelQ(g_dissolved, reinterpret_cast<void*>(ai), &dissolved) &&
                        CallModelQ(g_renderable, reinterpret_cast<void*>(ai), &renderable);
                f.readable = f.readable && okVis;
            }
            f.ready = camoTime > 1.5f && !dissolved && renderable;
        }
    } else if (ownerGone) {
        f.infected = recInf;
        f.used = recUsed;
        f.ready = true;
    }
    float dur = 0.0f;
    if (f.holding && f.readable && f.infected && !f.used && f.ready) {
        uintptr_t gameDi = 0;
        bool mode = false, gate = true;
        const bool okMode = CallMode(reinterpret_cast<void*>(player), &mode);
        const bool okGate = ReadPtr(g_gd + g_rGameDi, &gameDi) && gameDi && CallGate(reinterpret_cast<void*>(gameDi), &gate);
        const bool okDur = CallDur(reinterpret_cast<void*>(player), &dur);
        f.readable = okMode && okGate && okDur;
        f.modeOk = mode && !gate;
        f.durationS = dur;
    }
    const camo::Outcome o = camo::Decide(f);
    bool started = false, cleared = false, skillOk = false;
    if (o == camo::Outcome::Start) {
        if (ai) {
            cleared = WriteU8(ai + kOnceOff, 0);   // the game clears it before it starts (+30C060) - one use, shared with flat R1
        } else {
            AcquireSRWLockExclusive(&g_limbLock);
            g_limbs.MarkOwnUsed(obj);
            ReleaseSRWLockExclusive(&g_limbLock);
            cleared = true;
        }
        started = cleared && CallStart(reinterpret_cast<void*>(camoMod), dur);
        skillOk = started && CallSkillUsed(reinterpret_cast<void*>(player));
        if (started) { ++g_nStarted; }
    } else {
        ++g_nRefused[static_cast<int>(o) & 7];
    }
    if (g_nLines < kMaxEventLines) {
        ++g_nLines;
        // [PROBE: CAMOWIPE] gate=camo_gesture  site=HEADSET  log-only=no
        LOG("camogesture: [CAMOWIPE] WIPE %s hand (span %.3f m) - %s: %s %p, corpse %p | infected %d, once-flag %u, camo time %.2f, "
            "dissolved %d, renderable %d, readable %d | %s%s | duration %.1f s%s | thread %lu",
            h == 0 ? "LEFT" : "RIGHT", static_cast<double>(g_wipeSpanUm[h]) * 1e-6, camo::OutcomeName(o), via, obj,
            reinterpret_cast<void*>(ai), f.infected ? 1 : 0, static_cast<unsigned>(once), camoTime, dissolved ? 1 : 0, renderable ? 1 : 0,
            f.readable ? 1 : 0, o == camo::Outcome::Start ? (started ? "STARTED" : "*** START FAULTED ***") : "not started",
            o == camo::Outcome::Start ? (skillOk ? ", skill-used recorded" : ", skill-used NOT recorded") : "", dur,
            g_nLines == kMaxEventLines ? " (last event line - cap reached)" : "", GetCurrentThreadId());
    }
}

void Summary()
{
    if (!BodyGrabHandActive(0) && !BodyGrabHandActive(1)) { return; }
    const ULONGLONG ms = GetTickCount64();
    if (ms - g_lastSumMs < 1000) { return; }
    const long limbs = g_nLimbs;
    if (g_nEvents == g_lastSumEvents && limbs == g_lastSumLimbs) { return; }   // nothing new: no line (INV-9)
    g_lastSumMs = ms;
    g_lastSumEvents = g_nEvents;
    g_lastSumLimbs = limbs;
    // [PROBE: CAMOWIPE] gate=camo_gesture  site=HEADSET  log-only=yes
    LOG("camogesture: [CAMOWIPE] 1 s (a hand holds a body, counts changed) - wipes %ld, started %ld, refused: not holding %ld / not "
        "infected %ld / used up %ld / not ready %ld / mode %ld / no duration %ld / unreadable %ld | relict spawns %ld, limbs recorded "
        "%ld (infected %ld), owner not proven %ld, faults %ld",
        g_nEvents, g_nStarted, g_nRefused[1], g_nRefused[2], g_nRefused[3], g_nRefused[4], g_nRefused[5], g_nRefused[6], g_nRefused[7],
        static_cast<long>(g_nCuts), limbs, static_cast<long>(g_nLimbsInfected), static_cast<long>(g_nCutRefused),
        static_cast<long>(g_nCutFault));
}

}  // namespace

void CamoGesturePadPoll(int hand, const float gripTrk[3], bool havePos, double tS)
{
    if (!CamoGesture || hand < 0 || hand > 1) { return; }
    camo::WipeState& s = g_wipe[hand];
    if (!havePos || !BodyGrabHandActive(hand)) {
        s = camo::WipeState{};
        return;
    }
    XrPosef head{};
    if (!GetHeadPose(head)) {
        s = camo::WipeState{};
        return;
    }
    const float hp[3] = { head.position.x, head.position.y, head.position.z };
    const float hq[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
    float hl[3] = {};
    if (!camo::HeadLocal(hp, hq, gripTrk, hl)) { return; }
    if (InterlockedCompareExchange(&g_firstPad, 1, 0) == 0) {
        // [PROBE: CAMOWIPE] gate=camo_gesture  site=HEADSET  log-only=yes
        LOG("camogesture: [CAMOWIPE] FIRST HELD SAMPLE (logged unconditionally) - %s hand in the head's frame (+x right, +y up, -z "
            "forward) %.3f %.3f %.3f m; in the face zone %d | thread %lu",
            hand == 0 ? "LEFT" : "RIGHT", hl[0], hl[1], hl[2],
            camo::InZone(camo::WipeParams{ CamoWipeFwdM, CamoWipeRadiusM, CamoWipeSweepM, CamoWipeMaxS }, hl) ? 1 : 0,
            GetCurrentThreadId());
    }
    const camo::WipeParams prm{ CamoWipeFwdM, CamoWipeRadiusM, CamoWipeSweepM, CamoWipeMaxS };
    float right[3] = {};
    camo::HeadRight(hq, right);   // final review I1: the sweep is the hand's own travel along this (at zone entry), never head turn
    if (camo::WipeStep(s, prm, hl, gripTrk, right, tS)) {
        InterlockedExchange(&g_wipeSpanUm[hand], static_cast<LONG>((s.maxX - s.minX) * 1e6f));
        InterlockedIncrement(&g_wipeSeq[hand]);
    }
}

void CamoGestureTick()
{
    if (!CamoGesture) { return; }
    if (g_state == 0) {
        const char* why = Resolve();
        g_state = why ? -1 : 1;
        if (why) {
            // [PROBE: CAMOWIPE] gate=camo_gesture  site=FLAT  log-only=no
            LOG("camogesture: [CAMOWIPE] REFUSED - %s. The gesture stays off this launch (camo_gesture=1).", why);
        }
    }
    for (int h = 0; h < 2; ++h) {
        const LONG s = g_wipeSeq[h];
        if (s == g_seen[h]) { continue; }
        g_seen[h] = s;   // several wipes since the last update = judged once (the hand cannot wipe twice in one update)
        if (g_state == 1) { Judge(h); }
    }
    if (g_state == 1) { Summary(); }
}

}  // namespace dlvr
