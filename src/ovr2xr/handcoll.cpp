// Live-enemy hand collision - spec docs/superpowers/specs/2026-10-30-live-enemy-hand-collision-design.md, plan
// docs/superpowers/plans/2026-10-30-live-enemy-hand-collision.md Task 3. The shapes and the stop are handcoll_logic.h (unit-tested).
//
// HOW (RESEARCH-ENEMY-GRAB-2026-10-30 §6; INV-659 / INV-660 / INV-661; OUR gamedll 97046bba, OUR engine e9fbfc2c):
//   - Which enemies: the ones the game lets the player grapple - the logic module *(AI + 3416) (float +76 != 0 = not valid) vfunc
//     +2864 (CanApplyNageWaza, INV-659). Found by the engine's level query ILevel::FindObjectsInRadius around the shoulders, <= 10 Hz
//     (body grab's RelictQuery idiom); a result is a HumanAI when its vftable slot 37 is HumanAI::IsDead (+5E41C0).
//   - The body: ragdoll_ai.phx Bones() (OUR Data0.pak, kBones below), each shape sized by OUR engine's rule (+3B0280): the bone's
//     extents from the engine's own +151B00 (engine rva, Epic via buildrva_table.h family 13, prologue byte-checked), mapped by the
//     bone's world scale (handcoll::ScaleToBone), then handcoll::BuildLocalShape. Built ONCE per tracked enemy; posed every update
//     from IModelObject::GetElementWorldMatrix (the AI is its own IModelObject).
//   - When: HandCollisionTick runs in HookTail BEFORE the tail solve. INV-661: at that point every enemy is animated for this update
//     but its own post-update (AIManager, after the player's) has not run - that post-update twists / shifts / leans three elements
//     (U-67). The 1 Hz VISADJ line logs those three values per tracked enemy so a run sizes the gap.
//   - The stop: SolveArmIk calls HandCollisionStop before the reach clamp with the shoulder S, the wrist target T and the wrist ->
//     fingertip offset; handcoll::StopTarget moves the fingertip to where S -> fingertip first enters a shape, and T moves by the
//     same shift (direction kept, only the reach shortens). One PulseController pulse on the rising edge (handcoll::ContactEdge).
//   - SHAPECHECK (the positive control, spec §5.5): a tracked enemy that dies is watched for 3 s; when its ragdoll appears, OUR shapes
//     posed on its bones that update are compared with the ragdoll's own hitboxes as body grab measures them
//     (BodyGrabRagdollHitboxes) - the engine built those from the same bones by the same rule.
//
// THREADS. Tick and Stop on the game thread (HookTail -> the tail solve); a Stop from any other thread (the pad-poll fallback solve)
// returns false and is counted. Every game read and call is SEH-guarded; a fault refuses by name.
#include "handcoll.h"

#include "bodygrab.h"        // BodyGrabRagdollHitboxes - SHAPECHECK
#include "controllers.h"     // PulseController
#include "handcoll_logic.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: the fixed gamedll rvas below are family 8 of tools/egl_allowlist.tsv (body grab's own), the engine extents
// rva is family 13; resolved ONCE in Resolve. Engine exports are found by name on both builds. The DEV tree has no buildrva.h:
// Steam rvas only.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_HC_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_HC_EN_RVA(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_HC_GD_RVA(rva) (rva)
#define DLVR_HC_EN_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: hand_collision] required-for=the live-enemy hand stop - an empty hand (fists = empty) stops on a live grapple-able enemy's
//      body with one buzz on contact (HandCollision) - spec 2026-10-30-live-enemy-hand-collision-design.
//      tree=INERT at 0: HandCollisionTick and HandCollisionStop return at their first test; nothing of the game's is read or
//      written (the tick clears our own tracked / watch / shoulder state - review F7).
//      HEADSET: needs the tail solve (rig_solve_at_tail, HookTail - the three master enables), controller poses (the hand target),
//      the engine exports named at READY and the Steam gamedll 97046bba / engine e9fbfc2c or the Epic build (buildrva families 8 +
//      13) - READY checks HumanAI slot 37 and the extents function's first 16 bytes; any other build REFUSES.
//      FLAT: READY + TRACK only (the level query runs around the drawn shoulders; no stop without controller poses).
//      SHAPECHECK also needs body_grab=1 (its hitbox walk). Was compiled default 0 - Tier C until Jay verifies it in the headset.
//      SHIPS ON since 2026-11-03 (Jay picked "Hand collision" for the tester package).
bool  HandCollision = true;
// [DEP: hand_collision_buzz_amp] required-for=the contact pulse's strength 0..1 (HandCollisionBuzzAmp). Start value = the two-hand
//      grip's 0.5 (INV-501) - Tier D. tree=INERT at hand_collision=0.
float HandCollisionBuzzAmp = 0.5f;
// [DEP: hand_collision_buzz_ms] required-for=the contact pulse's length in ms (HandCollisionBuzzMs). Start value = the bow grab's
//      40 ms (INV-543) - Tier D. tree=INERT at hand_collision=0.
float HandCollisionBuzzMs = 40.0f;
// [DEP: push_grab_reach_m] required-for=how far short of a body surface the empty-hand fingertip still counts as a CONTACT for the
//      open-hand push and grab (PushGrabReachM; 2026-10-31 (6), Jay: "Reach a few cm short" - PG8: 5 of 9 missed grabs had no touch
//      within 1 s). The hand stop, the buzz and the TOUCH line still need the real surface. 0 = off. Start value 0.05 m - Jay's "a few
//      cm", Tier D. tree=INERT at enemy_push_grab=0 (only the push / grab read the contact) or hand_collision=0.
float PushGrabReachM = 0.05f;

namespace {

namespace hc = ::dlvr::handcoll;

// ---- OUR gamedll 97046bba / engine e9fbfc2c - RESEARCH-ENEMY-GRAB §6, INV-659 / INV-661 ----
constexpr uintptr_t kHumanAiVtRva = 0x151D1C0;    // HumanAI's primary vftable (Steam; for the TRACK line's class column only)
constexpr uintptr_t kIsDeadRva = 0x5E41C0;        // HumanAI::IsDead, slot 37 - the HumanAI test (family 8)
constexpr size_t    kIsDeadSlot = 37;
constexpr uintptr_t kVisGetRva = 0x191C50;        // HumanAI slot 194: (AI + 816)->vfunc +8 = its vis (family 8)
constexpr size_t    kVisGetSlot = 194;
constexpr uintptr_t kExtentsRva = 0x151B00;       // engine: the per-bone extents the ragdoll build uses (family 13)
constexpr unsigned char kExtentsHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41 };
constexpr size_t    kCtlObjOff = 24;              // AI + 24 = IControlObject
constexpr size_t    kGsoOff = 40;                 // PlayerDI + 40 = IGSObject; a HumanAI's too (INV-659 addendum, gp19)
constexpr size_t    kVisPlayerOff = 0x50;         // PlayerFppVis + 80 = PlayerDI
constexpr size_t    kModelOff = 8;                // IModelObject + 8 = the engine model +151B00 takes
constexpr size_t    kLogicModOff = 3416;          // AI + 3416 = the logic module
constexpr size_t    kModStaleOff = 76;            // module float +76 != 0 = not the live module
constexpr size_t    kCanGrappleOff = 2864;        // module vfunc +2864 = CanApplyNageWaza
constexpr size_t    kBindRagdollOff = 168;        // IPhysicsBind vfunc +168 -> the ragdoll (null = none)
// U-67 (INV-661 (4)): the vis post-update adjustments, read for the VISADJ line only
constexpr size_t    kVisIdsOff = 112;             // vis + 112 -> the element-id table
constexpr size_t    kVisTwistIdOff = 236;         // ... + 236 = the RotateElement element (ZombieAIVis +10CF4C0)
constexpr size_t    kVisShiftIdOff = 44;          // ... + 44  = the SetElementWorldMatrix element (BodyVis +10A9000)
constexpr size_t    kVisTwistOff = 4308;          // vis float: the twist angle
constexpr size_t    kVisShiftOff = 836;           // vis float: the shift along the element's X
constexpr size_t    kVisLeanOff = 800;            // vis float: the lean (BodyVis slot 312)
constexpr uintptr_t kVtZombieAiVisRva = 0x1810608, kVtHumanAiVisRva = 0x1521CF0, kVtBodyVisRva = 0x1808A38;   // Steam, labels only

constexpr int    kMaxTracked = 6;
constexpr int    kMaxResults = 512;              // level-query results examined per query - all of them (review F2: HC1 saw 35; the
                                                 // engine returns them in spatial order, not by distance - ida/hc31_en)
constexpr int    kMaxCand = 32;                  // live grapple-able candidates ranked per query
constexpr float  kQueryRadiusM = 2.5f;
constexpr double kQueryEveryS = 0.10;
constexpr double kShoulderMaxAgeS = 0.5;         // review F8: shoulders older than this (no tail solve) are not queried around
// INV-9, review F1: one cap PER LINE KIND, so a chatty kind cannot silence the others (HC1: VISADJ spent a shared 200 and the last
// 48 s logged nothing). Each kind says once when its cap is reached.
constexpr long   kMaxEventLines = 200;           // TRACK / TOUCH / refusals / SHAPECHECK verdicts
constexpr long   kMaxWindowLines = 200;          // the 1 s lines
constexpr long   kMaxVisLines = 120;             // VISADJ - printed on a CHANGE of an enemy's values, or every kVisHeartbeatS
constexpr double kVisHeartbeatS = 10.0;
constexpr int    kMaxShapeChecks = 3;
constexpr double kWatchS = 3.0;

const char* const kExpWorld = "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z";                     // +142E90
const char* const kExpElemId = "?GetElementID@IModelObject@@QEBAHPEBD@Z";                                   // +142720
const char* const kExpElemName = "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z";                           // +1428D0
const char* const kExpPhysicsBind = "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ";                // +13FC50
const char* const kExpFindRadius =
    "?FindObjectsInRadius@ILevel@@QEAA_NPEAV?$vector@PEAVIControlObject@@@ttl@@AEBVvec3@@MPEBVCRTTI@@_NPEAH@Z";   // +299560
const char* const kExpGetLevel = "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ";                                 // +2521D0
// Review F3 (INV-663): the engine's own id lookup re-validates a tracked enemy without reading the enemy.
const char* const kExpGetId = "?GetID@IGSObject@@QEBAHXZ";                                                  // +252080
const char* const kExpFindById = "?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z";                                // +251DF0
// The GAP probe (2026-10-31 (5), Jay: "We need to resolve the gap"; INV-665): the player's and each tracked enemy's object positions,
// horizontal (Y up), so a run shows whether the bodies stop ~0.70 m apart (the walk bodies) or farther (the enemy's own distance).
// Optional: missing = no distance column, said at READY; hand collision is unaffected. The walk radii are NOT read (the config's
// owner is not established - INV-665).
const char* const kExpWorldPos = "?GetWorldPosition@IControlObject@@QEBA?AVvec3@@XZ";                       // +13C920

// ragdoll_ai.phx Bones() (OUR Data0.pak): UseBone = scale 1 (engine +3BC680 passes 1.0f), UseBoneScale = its scale. The three
// CollisionHelper lines (l_foot / r_foot on the calves, spine on spine1) are geoms without a body - skipped, named at READY.
struct BoneDef { const char* name; hc::ShapeKind kind; float scale; };
constexpr BoneDef kBones[] = {
    { "pelvis", hc::ShapeKind::Sphere, 0.8f },     { "spine1", hc::ShapeKind::Sphere, 0.8f },
    { "spine3", hc::ShapeKind::Sphere, 0.8f },     { "head", hc::ShapeKind::Sphere, 1.0f },
    { "l_upperarm", hc::ShapeKind::Capsule, 1.0f }, { "r_upperarm", hc::ShapeKind::Capsule, 1.0f },
    { "l_forearm", hc::ShapeKind::Capsule, 1.0f },  { "r_forearm", hc::ShapeKind::Capsule, 1.0f },
    { "l_hand", hc::ShapeKind::Sphere, 1.4f },      { "r_hand", hc::ShapeKind::Sphere, 1.4f },
    { "l_thigh", hc::ShapeKind::Capsule, 0.8f },    { "r_thigh", hc::ShapeKind::Capsule, 0.8f },
    { "l_calf", hc::ShapeKind::Capsule, 0.7f },     { "r_calf", hc::ShapeKind::Capsule, 0.7f },
};
constexpr int kNumBones = static_cast<int>(sizeof(kBones) / sizeof(kBones[0]));

using WorldFn = const float*(__fastcall*)(const void* model, int elem);
using ElemIdFn = int(__fastcall*)(const void* model, const char* name);
using ElemNameFn = const char*(__fastcall*)(const void* model, int elem);
using PhysicsBindFn = void*(__fastcall*)(void* ico);
using GetLevelFn = void*(__fastcall*)(const void* gso);
using GetIdFn = int(__fastcall*)(const void* gso);
using FindByIdFn = void*(__fastcall*)(const void* gso, int id);
struct EngineVec { void** data; unsigned count; unsigned capacity; };
using FindRadiusFn = bool(__fastcall*)(void* level, EngineVec* out, const float* c, float r, const void* rtti, bool b, int* n);
using ExtentsFn = float*(__fastcall*)(void* model, float* out24, int elem);
using VFn0 = void*(__fastcall*)(void*);
using VFnBool = bool(__fastcall*)(void*);
using WorldPosFn = float*(__fastcall*)(const void* ico, float* out);   // as roomscale.cpp calls it

WorldFn g_world = nullptr;
ElemIdFn g_elemId = nullptr;
ElemNameFn g_elemName = nullptr;
PhysicsBindFn g_physicsBind = nullptr;
GetLevelFn g_getLevel = nullptr;
GetIdFn g_getId = nullptr;
FindByIdFn g_findById = nullptr;
FindRadiusFn g_findRadius = nullptr;
ExtentsFn g_extents = nullptr;
WorldPosFn g_worldPos = nullptr;     // the gap probe; null = not logged
bool g_gapFirstSaid = false;
uintptr_t g_gd = 0, g_en = 0;
uintptr_t g_rIsDead = kIsDeadRva, g_rVisGet = kVisGetRva, g_rHumanAiVt = kHumanAiVtRva;
uintptr_t g_rVtZv = kVtZombieAiVisRva, g_rVtHv = kVtHumanAiVisRva, g_rVtBv = kVtBodyVisRva;
int g_resolved = 0;              // 0 not yet, 1 READY, -1 REFUSED (named once)
DWORD g_tickThread = 0;
volatile LONG g_nEvent = 0, g_nWindow = 0, g_nVis = 0;
volatile LONG g_nTruncSaid = 0, g_nUnrankedSaid = 0;   // the query's two "first time" lines
LONG64 g_offThread = 0;
LONG64 g_idDrops = 0;            // review F3: tracked enemies / watches dropped because the id lookup no longer returns them
bool g_idSelfTestSaid = false;
EngineVec g_vec = {};            // game thread only; grown by the engine's realloc, reused

struct Tracked {
    void* ai;
    void* gso;                   // AI + 40 = its IGSObject (INV-659 addendum, gp19) - review F3
    int id;                      // IGSObject::GetID at TRACK time; re-checked every update by FindObjectByID (INV-663)
    uintptr_t vt;
    int n;                       // shapes built
    int def[kNumBones];          // kBones index of each shape
    int elem[kNumBones];
    hc::LocalShape loc[kNumBones];
    hc::WorldShape w[kNumBones];
    bool posed[kNumBones];
    bool seen;                   // returned by the last level query
    double since;
    long touches;
    float dist;                  // root element to the shoulders at the last query, m (-1 = not readable)
    float bodyXZ;                // the gap probe: player to enemy object positions, horizontal, at the last query (-1 = not read)
    float minXZ;                 // ... the smallest while tracked (-1 = never read)
    bool haveVis;                // VISADJ: the last values printed for this enemy, and when
    float lastVis[3];
    int lastVisIds[2];
    double lastVisS;
};
Tracked g_t[kMaxTracked] = {};
int g_nt = 0;

struct Watch { bool on; double untilS; Tracked t; };
Watch g_watch[2] = {};
int g_checks = 0;

struct HandState {
    hc::ContactEdge edge;
    bool touching = false;
    double winS = 0.0;
    long frames = 0, stopped = 0, touchesWin = 0, pulses = 0;
    double depthSum = 0.0;
    float depthMax = 0.0f;
    float S[3] = {};
    bool haveS = false;
    double sAtS = 0.0;           // when S was published (NowS) - review F8
    LONG64 calls = 0;
    // The open-hand push and grab (plan 2026-10-31 Task 3): the real fingertip's velocity and the last contact, game thread only.
    bool haveTip = false;
    float prevTip[3] = {};
    double prevTipS = 0.0;
    float vel[3] = {};
    bool haveContact = false;
    HandContact contact = {};
    // the miss line's reading (push_grab_reach_m): written by the solve thread, read by the pad thread (aligned, one store each)
    volatile float nearestGap = -1.0f;
    volatile double nearestS = 0.0;
};
HandState g_h[2];
void* g_player = nullptr;        // the PlayerDI the last tick read (push / grab)
double g_tickS = -1.0e9;         // when the last tick ran - a contact older than it is from an earlier update
double g_lastQuery = -1.0e9;
double g_lastVisAdj = -1.0e9;
LONG64 g_queries = 0;

double NowS()   // the same clock as looting.cpp / bodygrab.cpp NowS
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
// True = this line may be written. The first line past a kind's cap is replaced by one CAP REACHED line naming it.
bool CapLine(volatile LONG* n, long cap, const char* kind)
{
    const LONG k = InterlockedIncrement(n);
    if (k == cap + 1) {
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] CAP REACHED - %ld %s lines written; no more of that kind this launch (the other kinds go on)", cap, kind);
    }
    return k <= cap;
}
bool EventLine() { return CapLine(&g_nEvent, kMaxEventLines, "event (TRACK / TOUCH / SHAPECHECK)"); }
bool WindowLine() { return CapLine(&g_nWindow, kMaxWindowLines, "1 s"); }
bool VisLine() { return CapLine(&g_nVis, kMaxVisLines, "VISADJ"); }
const char* ArmName(int a) { return a == 0 ? "RIGHT" : "LEFT"; }
bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// ---- SEH-guarded game reads and calls (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadF(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadI(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadBytes(uintptr_t a, unsigned char* out, size_t n)
{
    __try { std::memcpy(out, reinterpret_cast<const void*>(a), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallWorld(void* ai, int e, float out[12])
{
    __try {
        const float* m = g_world(ai, e);
        if (!m) { return false; }
        for (int i = 0; i < 12; ++i) { out[i] = m[i]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallElemId(void* ai, const char* name, int* out)
{
    __try { *out = g_elemId(ai, name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const char* CallElemName(void* ai, int e)
{
    __try { return g_elemName(ai, e); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CopyName(const char* src, char* dst, int cap)
{
    __try {
        int i = 0;
        for (; src && i < cap - 1 && src[i]; ++i) { dst[i] = src[i]; }
        dst[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = 0; return false; }
}
bool CallExtents(void* ai, int e, float out[6])
{
    __try {
        void* model = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(ai) + kModelOff);
        if (!model) { return false; }
        float b[6] = {};
        g_extents(model, b, e);
        for (int i = 0; i < 6; ++i) { out[i] = b[i]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// AI -> PhysicsBind(AI + 24) -> vfunc +168 = the ragdoll (null = none). False = a fault.
bool HasRagdoll(void* ai, bool* yes)
{
    __try {
        void* bind = g_physicsBind(static_cast<char*>(ai) + kCtlObjOff);
        if (!bind) { *yes = false; return true; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        *yes = reinterpret_cast<VFn0>(*reinterpret_cast<const uintptr_t*>(vt + kBindRagdollOff))(bind) != nullptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallIsDead(void* ai, uintptr_t fn, bool* dead)
{
    __try { *dead = reinterpret_cast<VFnBool>(fn)(ai); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The logic module's CanApplyNageWaza. *ok = the call ran; *can = its answer; *why = the refusal when it did not.
void CallCanGrapple(void* ai, bool* ok, bool* can, const char** why)
{
    *ok = false;
    *can = false;
    *why = "";
    __try {
        const uintptr_t mod = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(ai) + kLogicModOff);
        if (!mod) { *why = "no logic module"; return; }
        if (*reinterpret_cast<const float*>(mod + kModStaleOff) != 0.0f) { *why = "logic module not live (+76 != 0)"; return; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(mod);
        *can = reinterpret_cast<VFnBool>(*reinterpret_cast<const uintptr_t*>(vt + kCanGrappleOff))(reinterpret_cast<void*>(mod));
        *ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { *why = "logic module call faulted"; }
}
bool CallVis(void* ai, uintptr_t fn, void** vis)
{
    __try { *vis = reinterpret_cast<VFn0>(fn)(ai); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LevelOf(void* player, void** level)
{
    __try { *level = g_getLevel(static_cast<char*>(player) + kGsoOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallGetId(void* gso, int* out)
{
    __try { *out = g_getId(gso); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallFindById(void* gso, int id, void** out)
{
    __try { *out = g_findById(gso, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Review F3 (INV-663): is `gso` still the live object the engine knows by `id`? Asked through the PLAYER's IGSObject (the lookup
// reads the caller's level only), so the enemy's own memory is never read - a destroyed enemy is skipped by the engine's id tree
// the moment it is flagged, before its memory is freed. A fault or any other answer = no (fails closed: the enemy is dropped).
bool StillInEngine(void* playerGso, void* gso, int id)
{
    void* found = nullptr;
    return playerGso && gso && CallFindById(playerGso, id, &found) && found == gso;
}
bool QueryRadius(void* level, const float c[3], float r)
{
    __try { return g_findRadius(level, &g_vec, c, r, nullptr, false, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallWorldPos(void* obj, float out[3])
{
    if (!g_worldPos) { return false; }
    __try { g_worldPos(static_cast<char*>(obj) + kCtlObjOff, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- READY / REFUSED, once ----
bool Resolve()
{
    if (g_resolved != 0) { return g_resolved == 1; }
    HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return false; }   // not loaded yet: try again next update (never cached)
    g_rIsDead = DLVR_HC_GD_RVA(kIsDeadRva);
    g_rVisGet = DLVR_HC_GD_RVA(kVisGetRva);
    const uintptr_t rExt = DLVR_HC_EN_RVA(kExtentsRva);
    g_world = reinterpret_cast<WorldFn>(GetProcAddress(en, kExpWorld));
    g_elemId = reinterpret_cast<ElemIdFn>(GetProcAddress(en, kExpElemId));
    g_elemName = reinterpret_cast<ElemNameFn>(GetProcAddress(en, kExpElemName));
    g_physicsBind = reinterpret_cast<PhysicsBindFn>(GetProcAddress(en, kExpPhysicsBind));
    g_findRadius = reinterpret_cast<FindRadiusFn>(GetProcAddress(en, kExpFindRadius));
    g_getLevel = reinterpret_cast<GetLevelFn>(GetProcAddress(en, kExpGetLevel));
    g_getId = reinterpret_cast<GetIdFn>(GetProcAddress(en, kExpGetId));
    g_findById = reinterpret_cast<FindByIdFn>(GetProcAddress(en, kExpFindById));
    const char* missing = !g_world ? kExpWorld : !g_elemId ? kExpElemId : !g_elemName ? kExpElemName
                        : !g_physicsBind ? kExpPhysicsBind : !g_findRadius ? kExpFindRadius : !g_getLevel ? kExpGetLevel
                        : !g_getId ? kExpGetId : !g_findById ? kExpFindById : nullptr;
    if (missing) {
        g_resolved = -1;
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] REFUSED - engine export %s not found: hand collision is off for this launch", missing);
        return false;
    }
    const uintptr_t gdBase = reinterpret_cast<uintptr_t>(gd), enBase = reinterpret_cast<uintptr_t>(en);
    unsigned char head[16] = {};
    if (!ReadBytes(enBase + rExt, head, sizeof(head)) || std::memcmp(head, kExtentsHead, sizeof(head)) != 0) {
        g_resolved = -1;
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] REFUSED - the extents function engine+%llX does not start with the Steam e9fbfc2c bytes (first "
            "bytes %02X %02X %02X %02X): not a build this was read from - hand collision is off for this launch",
            static_cast<unsigned long long>(rExt), head[0], head[1], head[2], head[3]);
        return false;
    }
    g_extents = reinterpret_cast<ExtentsFn>(enBase + rExt);
    g_gd = gdBase;
    g_en = enBase;
    g_rHumanAiVt = DLVR_HC_GD_RVA(kHumanAiVtRva);
    g_rVtZv = kVtZombieAiVisRva;
    g_rVtHv = kVtHumanAiVisRva;
    g_rVtBv = kVtBodyVisRva;
    g_worldPos = reinterpret_cast<WorldPosFn>(GetProcAddress(en, kExpWorldPos));
    g_resolved = 1;
    // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
    LOG("handcoll: [HANDCOLL] READY (logged unconditionally with hand_collision=1) - engine exports resolved, extents engine+%llX "
        "byte-checked, HumanAI IsDead gamedll+%llX | %d shapes from ragdoll_ai.phx (CollisionHelper l_foot / r_foot / spine "
        "SKIPPED: geoms without a body) | query %.1f m every %.0f ms, up to %d enemies | buzz %.2f for %.0f ms | gap probe %s | "
        "thread %lu",
        static_cast<unsigned long long>(rExt), static_cast<unsigned long long>(g_rIsDead), kNumBones,
        static_cast<double>(kQueryRadiusM), kQueryEveryS * 1000.0, kMaxTracked, static_cast<double>(HandCollisionBuzzAmp),
        static_cast<double>(HandCollisionBuzzMs),
        g_worldPos ? "ON (player-enemy body distance, horizontal, on TRACK / TOUCH / DROPPED)"
                   : "OFF - engine export ?GetWorldPosition@IControlObject@@ not found, no distance column",
        GetCurrentThreadId());
    return true;
}

// HumanAI test + alive + no ragdoll. why = the first failed test (nullptr = passed).
const char* LiveCheck(void* ai, uintptr_t* vtOut)
{
    uintptr_t vt = 0, s37 = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(ai), &vt) || !vt) { return "vftable unreadable"; }
    if (vtOut) { *vtOut = vt; }
    if (!ReadPtr(vt + 8 * kIsDeadSlot, &s37)) { return "vftable unreadable"; }
    if (s37 != g_gd + g_rIsDead) { return "not a HumanAI (slot 37 is not IsDead)"; }
    bool dead = false;
    if (!CallIsDead(ai, s37, &dead)) { return "IsDead faulted"; }
    if (dead) { return "dead"; }
    bool rag = false;
    if (!HasRagdoll(ai, &rag)) { return "ragdoll read faulted"; }
    if (rag) { return "ragdoll"; }
    return nullptr;
}

// Build the shape set once per enemy. False = no shape could be built (named).
bool Build(void* ai, Tracked* t, char* skipped, size_t cap)
{
    std::memset(t, 0, sizeof(*t));
    t->ai = ai;
    skipped[0] = 0;
    size_t used = 0;
    for (int d = 0; d < kNumBones; ++d) {
        const char* why = nullptr;
        int e = -1;
        float ext[6] = {}, m[12] = {}, mn[3], mx[3];
        hc::LocalShape s{};
        if (!CallElemId(ai, kBones[d].name, &e) || e < 0) {
            why = "no such element";
        } else if (!CallExtents(ai, e, ext)) {
            why = "extents faulted";
        } else if (!CallWorld(ai, e, m)) {
            why = "world faulted";
        } else if (!hc::ScaleToBone(m, ext, ext + 3, mn, mx)) {
            why = "bone scale not readable";
        } else if (!hc::BuildLocalShape(kBones[d].kind, mn, mx, kBones[d].scale, &s)) {
            why = "extents not finite";
        }
        if (why) {
            const int k = _snprintf_s(skipped + used, cap - used, _TRUNCATE, "%s%s (%s)", used ? ", " : "", kBones[d].name, why);
            if (k > 0) { used += static_cast<size_t>(k); }
            continue;
        }
        t->def[t->n] = d;
        t->elem[t->n] = e;
        t->loc[t->n] = s;
        ++t->n;
    }
    return t->n > 0;
}

void Pose(Tracked* t)
{
    for (int k = 0; k < t->n; ++k) {
        float m[12] = {};
        t->posed[k] = CallWorld(t->ai, t->elem[k], m) && Finite3(&m[0]) && Finite3(&m[4]) && Finite3(&m[8]);
        if (t->posed[k]) { hc::PoseShape(t->loc[k], m, &t->w[k]); }
    }
}

float Dist3(const float a[3], const float b[3])
{
    const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
void Centre(const hc::WorldShape& w, float out[3])
{
    for (int i = 0; i < 3; ++i) { out[i] = (w.a[i] + w.b[i]) * 0.5f; }
}
// Review F2's ranking: the enemy's element 0 (its root) to the point c, m. -1 = not readable. COST: one world read.
float RootDist(void* ai, const float c[3])
{
    float m[12] = {};
    if (!CallWorld(ai, 0, m)) { return -1.0f; }
    const float p[3] = { m[3], m[7], m[11] };
    if (!Finite3(p)) { return -1.0f; }
    return Dist3(p, c);
}
// The gap probe: the player's and the enemy's object positions, horizontal distance (Y up) into t.bodyXZ / t.minXZ (-1 = not read).
// The first sample is logged unconditionally, read or not. COST: two engine calls per tracked enemy per query (every 100 ms).
void GapSample(void* player, Tracked& t)
{
    float p[3] = {}, e[3] = {};
    const bool ok = CallWorldPos(player, p) && CallWorldPos(t.ai, e) && Finite3(p) && Finite3(e);
    t.bodyXZ = ok ? std::sqrt((p[0] - e[0]) * (p[0] - e[0]) + (p[2] - e[2]) * (p[2] - e[2])) : -1.0f;
    if (ok && (t.minXZ < 0.0f || t.bodyXZ < t.minXZ)) { t.minXZ = t.bodyXZ; }
    if (!g_gapFirstSaid) {
        g_gapFirstSaid = true;
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] GAP first sample (logged unconditionally) - %s | player (%.3f, %.3f, %.3f), enemy %p (%.3f, %.3f, %.3f)"
            " | horizontal %.3f m (the walk bodies alone would stop centres ~0.70 m apart, INV-665)",
            !g_worldPos ? "NOT READ: no GetWorldPosition export" : ok ? "read" : "NOT READ: a position call faulted or was not finite",
            static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]), t.ai, static_cast<double>(e[0]),
            static_cast<double>(e[1]), static_cast<double>(e[2]), static_cast<double>(t.bodyXZ));
    }
}
const char* VisLabel(uintptr_t vt)
{
    if (!vt || vt < g_gd) { return "?"; }
    const uintptr_t r = vt - g_gd;
    return r == g_rVtZv ? "ZombieAIVis" : r == g_rVtHv ? "HumanAIVis" : r == g_rVtBv ? "BodyVis" : "other (Steam rvas only)";
}

void DropAt(int i, const char* why, double now)
{
    Tracked& t = g_t[i];
    if (EventLine()) {
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] TRACK - enemy %p DROPPED (%s) after %.1f s, %ld touches | GAP closest %.2f m (body centres, "
            "horizontal; -1 = not read)", t.ai, why, now - t.since, t.touches, static_cast<double>(t.minXZ));
    }
    // a death: watch it for its ragdoll - SHAPECHECK
    if (g_checks < kMaxShapeChecks && (std::strcmp(why, "dead") == 0 || std::strcmp(why, "ragdoll") == 0)) {
        for (Watch& w : g_watch) {
            if (!w.on) {
                w.on = true;
                w.untilS = now + kWatchS;
                w.t = t;
                break;
            }
        }
    }
    g_t[i] = g_t[g_nt - 1];
    --g_nt;
}

// SHAPECHECK: OUR shapes on the dead enemy's bones vs its ragdoll's own hitboxes, once its ragdoll exists.
void ShapeCheckTick(double now, void* playerGso)
{
    for (Watch& w : g_watch) {
        if (!w.on) { continue; }
        if (!StillInEngine(playerGso, w.t.gso, w.t.id)) {   // review F3: before any read of the dead enemy
            w.on = false;
            ++g_idDrops;
            if (EventLine()) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
                LOG("handcoll: [HANDCOLL] SHAPECHECK - enemy %p (id %d): no longer returned by the engine's id lookup (despawned, or "
                    "the player unreadable) - NO VERDICT, not read", w.t.ai, w.t.id);
            }
            continue;
        }
        if (now > w.untilS) {
            w.on = false;
            if (EventLine()) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
                LOG("handcoll: [HANDCOLL] SHAPECHECK - enemy %p: NO RAGDOLL within %.0f s of its death - NO VERDICT", w.t.ai, kWatchS);
            }
            continue;
        }
        bool rag = false;
        if (!HasRagdoll(w.t.ai, &rag) || !rag) { continue; }
        w.on = false;
        ++g_checks;
        BodyGrabHitbox hb[32] = {};
        const int nh = BodyGrabRagdollHitboxes(w.t.ai, hb, 32);
        if (nh <= 0) {
            // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("handcoll: [HANDCOLL] SHAPECHECK #%d - enemy %p: NOT READY - %s - NO VERDICT", g_checks, w.t.ai,
                nh == -1 ? "body_grab=0" : nh == -2 ? "body grab not READY" : nh == -3 ? "no ragdoll hitboxes readable" : "no hitboxes");
            continue;
        }
        Pose(&w.t);
        int matched = 0;
        float worstC = 0.0f, worstR = 0.0f;
        for (int k = 0; k < w.t.n; ++k) {
            if (!w.t.posed[k]) { continue; }
            const hc::WorldShape& s = w.t.w[k];
            float c[3];
            Centre(s, c);
            const int want = s.kind == hc::ShapeKind::Sphere ? 1 : 2;
            // Review F5: paired BY INDEX - ragdoll body k is the k-th ragdoll_ai.phx Bones() entry (HC1 SHAPECHECK #2 / #3, all 14).
            // Nearest-centre pairing mis-paired on a whole-body offset (#1: spine1 -> a hand, head + spine3 -> one body).
            const int body = w.t.def[k];
            int best = -1;
            for (int j = 0; j < nh && best < 0; ++j) {
                if (hb[j].body == body && hb[j].shape == want) { best = j; }
            }
            if (best < 0) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
                LOG("handcoll: [HANDCOLL] SHAPECHECK #%d %-10s %s: no ragdoll body %d %s to compare (%d hitboxes) - NO VERDICT for it",
                    g_checks, kBones[body].name, want == 1 ? "sphere" : "capsule", body, want == 1 ? "sphere" : "capsule", nh);
                continue;
            }
            const float bd = Dist3(c, hb[best].c);
            ++matched;
            const float dr = (s.r - hb[best].prm[0]) * 1000.0f;
            const float ourLen = Dist3(s.a, s.b);
            const float dl = want == 2 ? (ourLen - hb[best].prm[1]) * 1000.0f : 0.0f;
            if (bd * 1000.0f > worstC) { worstC = bd * 1000.0f; }
            if (std::fabs(dr) > worstR) { worstR = std::fabs(dr); }
            // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("handcoll: [HANDCOLL] SHAPECHECK #%d %-10s %s: centre %.1f mm from ragdoll body %d | radius ours %.4f vs %.4f (%+.1f mm)"
                " | length ours %.4f vs %.4f (%+.1f mm)", g_checks, kBones[w.t.def[k]].name, want == 1 ? "sphere" : "capsule",
                static_cast<double>(bd * 1000.0f), hb[best].body, static_cast<double>(s.r), static_cast<double>(hb[best].prm[0]),
                static_cast<double>(dr), static_cast<double>(want == 2 ? ourLen : 0.0f),
                static_cast<double>(want == 2 ? hb[best].prm[1] : 0.0f), static_cast<double>(dl));
        }
        // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
        LOG("handcoll: [HANDCOLL] SHAPECHECK #%d SUMMARY - enemy %p: %d of %d shapes paired by body index against %d ragdoll hitboxes | "
            "worst centre %.1f mm, worst radius %.1f mm", g_checks, w.t.ai, matched, w.t.n, nh, static_cast<double>(worstC),
            static_cast<double>(worstR));
    }
}

// The level query around the shoulders: add new grapple-able live enemies, drop those that left the radius.
void Query(void* player, double now)
{
    float c[3] = {};
    int nc = 0;
    for (const HandState& h : g_h) {
        if (h.haveS && now - h.sAtS <= kShoulderMaxAgeS) { for (int i = 0; i < 3; ++i) { c[i] += h.S[i]; } ++nc; }
    }
    // no solve yet, or none for kShoulderMaxAgeS (review F8: the rig disarmed - a query there would track a stale spot): nothing to
    // centre on. The tracked set is DROPPED too - without the query nothing would ever drop an enemy that leaves or despawns, and
    // LiveCheck would go on reading it (review F3's window would no longer be <= 100 ms).
    if (nc == 0) {
        for (int i = g_nt - 1; i >= 0; --i) { DropAt(i, "no shoulders for 0.5 s - the rig is not solving", now); }
        return;
    }
    for (int i = 0; i < 3; ++i) { c[i] /= static_cast<float>(nc); }
    void* level = nullptr;
    if (!LevelOf(player, &level) || !level || !QueryRadius(level, c, kQueryRadiusM)) { return; }
    const LONG64 q = ++g_queries;
    for (int i = 0; i < g_nt; ++i) { g_t[i].seen = false; }
    const int hits = static_cast<int>(g_vec.count);
    const int n = hits < kMaxResults ? hits : kMaxResults;
    int humans = 0, refusedGrapple = 0, ranked = 0, notRanked = 0;
    struct Cand { void* ai; uintptr_t vt; float d; };
    Cand cand[kMaxCand];
    int nc2 = 0;
    // pass 1 (review F2): EVERY result - mark the tracked ones seen with their distance, rank the new live grapple-able ones
    for (int i = 0; i < n; ++i) {
        uintptr_t e = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(g_vec.data) + 8 * static_cast<uintptr_t>(i), &e) || !e) { continue; }
        void* obj = reinterpret_cast<void*>(e - kCtlObjOff);   // the entry is the object's IControlObject (+24)
        bool known = false;
        for (int k = 0; k < g_nt; ++k) {
            if (g_t[k].ai == obj) {
                g_t[k].seen = true;
                g_t[k].dist = RootDist(obj, c);
                GapSample(player, g_t[k]);
                known = true;
            }
        }
        if (known || obj == player) { continue; }
        uintptr_t vt = 0;
        if (LiveCheck(obj, &vt) != nullptr) { continue; }
        ++humans;
        bool ok = false, can = false;
        const char* why = "";
        CallCanGrapple(obj, &ok, &can, &why);
        if (!ok || !can) { ++refusedGrapple; continue; }
        const float d = RootDist(obj, c);
        if (nc2 < kMaxCand) {
            cand[nc2++] = { obj, vt, d < 0.0f ? 1.0e30f : d };
            ++ranked;
        } else {
            ++notRanked;
        }
    }
    // nearest first (insertion sort, <= kMaxCand)
    for (int i = 1; i < nc2; ++i) {
        const Cand x = cand[i];
        int j = i - 1;
        while (j >= 0 && cand[j].d > x.d) { cand[j + 1] = cand[j]; --j; }
        cand[j + 1] = x;
    }
    for (int i = g_nt - 1; i >= 0; --i) {
        if (!g_t[i].seen) { DropAt(i, "left the query radius", now); }
    }
    // pass 2: admit nearest-first; a full set gives up its farthest for a strictly nearer one
    for (int ci = 0; ci < nc2; ++ci) {
        void* obj = cand[ci].ai;
        float dists[kMaxTracked];
        for (int k = 0; k < g_nt; ++k) { dists[k] = g_t[k].dist; }
        int rep = -1;
        if (!hc::Admit(dists, g_nt, kMaxTracked, cand[ci].d, &rep)) { break; }   // sorted: no later candidate is nearer
        if (rep >= 0) { DropAt(rep, "replaced by a nearer enemy", now); }
        char skipped[256];
        Tracked& t = g_t[g_nt];
        if (!Build(obj, &t, skipped, sizeof(skipped))) {
            if (EventLine()) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
                LOG("handcoll: [HANDCOLL] TRACK - enemy %p REFUSED: no shape could be built - %s", obj, skipped);
            }
            continue;
        }
        // Review F3: the id the engine knows it by, and the lookup must return it NOW - else it could never be re-validated, so it
        // is not tracked (fails closed, named). This is also the per-enemy positive control of the re-validation.
        void* gso = static_cast<char*>(obj) + kGsoOff;
        int id = 0;
        void* playerGso = static_cast<char*>(player) + kGsoOff;
        if (!CallGetId(gso, &id) || !StillInEngine(playerGso, gso, id)) {
            if (EventLine()) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
                LOG("handcoll: [HANDCOLL] TRACK - enemy %p REFUSED: the engine's id lookup does not return it (id %d) - it could "
                    "not be re-validated (review F3)", obj, id);
            }
            continue;
        }
        t.gso = gso;
        t.id = id;
        const uintptr_t vt = cand[ci].vt;
        t.vt = vt;
        t.seen = true;
        t.since = now;
        t.dist = cand[ci].d >= 1.0e29f ? -1.0f : cand[ci].d;
        t.minXZ = -1.0f;
        GapSample(player, t);
        ++g_nt;
        Pose(&t);
        if (EventLine()) {
            void* vis = nullptr;
            uintptr_t getter = 0, visVt = 0;
            const bool okVis = ReadPtr(vt + 8 * kVisGetSlot, &getter) && getter == g_gd + g_rVisGet && CallVis(obj, getter, &vis) &&
                               vis && ReadPtr(reinterpret_cast<uintptr_t>(vis), &visVt);
            // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
            LOG("handcoll: [HANDCOLL] TRACK + enemy %p id %d (vftable gamedll+%llX%s, vis %s) - %d shapes from ragdoll_ai.phx%s%s | "
                "root %.2f m from the shoulders (rank %d of %d candidates) | GAP %.2f m (body centres, horizontal) | %d tracked",
                obj, t.id, static_cast<unsigned long long>(vt - g_gd), vt - g_gd == g_rHumanAiVt ? " = HumanAI" : "",
                okVis ? VisLabel(visVt) : "unreadable", t.n, skipped[0] ? ", skipped: " : "", skipped,
                static_cast<double>(t.dist), ci + 1, nc2, static_cast<double>(t.bodyXZ), g_nt);
        }
    }
    if ((q == 1 || (n < hits && InterlockedIncrement(&g_nTruncSaid) == 1) || (notRanked > 0 && InterlockedIncrement(&g_nUnrankedSaid) == 1))
        && EventLine()) {
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] %s - %d objects within %.1f m of the shoulders, %d read (cap %d), %d live HumanAIs not yet tracked, "
            "%d of them not grapple-able, %d ranked by distance (%d past the rank cap %d) | %d tracked | thread %lu",
            q == 1 ? "first query (logged unconditionally)" : n < hits ? "QUERY TRUNCATED (first time)" : "RANK CAP REACHED (first time)",
            hits, static_cast<double>(kQueryRadiusM), n, kMaxResults, humans, refusedGrapple, ranked, notRanked, kMaxCand, g_nt,
            GetCurrentThreadId());
    }
}

// U-67: the three post-update adjustment values of each tracked enemy, read 1 Hz at HookTail (before that post-update runs) and
// PRINTED when they differ from the last line printed for that enemy, or every kVisHeartbeatS (review F1).
void VisAdjLine(double now)
{
    if (g_nt == 0 || now - g_lastVisAdj < 1.0) { return; }
    g_lastVisAdj = now;
    for (int i = 0; i < g_nt; ++i) {
        Tracked& t = g_t[i];
        uintptr_t getter = 0, ids = 0, visVt = 0;
        void* vis = nullptr;
        const bool okVis = ReadPtr(t.vt + 8 * kVisGetSlot, &getter) && getter == g_gd + g_rVisGet && CallVis(t.ai, getter, &vis) &&
                           vis && ReadPtr(reinterpret_cast<uintptr_t>(vis), &visVt);
        if (!okVis) {
            if (!t.haveVis && VisLine()) {
                // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
                LOG("handcoll: [HANDCOLL] VISADJ enemy %p: vis NOT READABLE (getter slot %zu not this build's) - NO VERDICT", t.ai,
                    kVisGetSlot);
            }
            t.haveVis = true;   // said once per enemy
            continue;
        }
        const uintptr_t v = reinterpret_cast<uintptr_t>(vis);
        float twist = -999.0f, shift = -999.0f, lean = -999.0f;
        int idTwist = -9, idShift = -9;
        const bool okT = ReadF(v + kVisTwistOff, &twist), okS = ReadF(v + kVisShiftOff, &shift), okL = ReadF(v + kVisLeanOff, &lean);
        const bool okIds = ReadPtr(v + kVisIdsOff, &ids) && ids && ReadI(ids + kVisTwistIdOff, &idTwist) && ReadI(ids + kVisShiftIdOff, &idShift);
        const float vals[3] = { twist, shift, lean };
        bool changed = !t.haveVis || idTwist != t.lastVisIds[0] || idShift != t.lastVisIds[1];
        for (int k = 0; k < 3 && !changed; ++k) { changed = std::fabs(vals[k] - t.lastVis[k]) > 1.0e-4f; }
        if (!changed && now - t.lastVisS < kVisHeartbeatS) { continue; }
        t.haveVis = true;
        for (int k = 0; k < 3; ++k) { t.lastVis[k] = vals[k]; }
        t.lastVisIds[0] = idTwist;
        t.lastVisIds[1] = idShift;
        t.lastVisS = now;
        if (!VisLine()) { continue; }
        char nT[48] = "-", nS[48] = "-";
        if (okIds && idTwist >= 0) { CopyName(CallElemName(t.ai, idTwist), nT, sizeof(nT)); }
        if (okIds && idShift >= 0) { CopyName(CallElemName(t.ai, idShift), nS, sizeof(nS)); }
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] VISADJ enemy %p (%s): twist %s%.4f on element %d '%s' | shift %s%.4f m on element %d '%s' | lean %s%.4f "
            "(INV-661 (4): applied AFTER this read; -999 = unreadable, the twist is a ZombieAIVis field; printed on a change or every "
            "10 s)", t.ai, VisLabel(visVt),
            okT ? "" : "UNREADABLE ", static_cast<double>(twist), idTwist, nT, okS ? "" : "UNREADABLE ", static_cast<double>(shift),
            idShift, nS, okL ? "" : "UNREADABLE ", static_cast<double>(lean));
    }
}

}  // namespace

// COST per player update: off = one bool test and four writes of our own state (review F7). On: the level query <= 10 Hz (one
// engine call; per result (all of them, <= 512) a vftable read, + IsDead and the logic-module call per new HumanAI, + one root world read per live grapple-able one and per tracked
// one; 14 name lookups + extents for each newly tracked enemy); every update per tracked enemy
// (<= 6) the engine's id lookup (review F3: FindObjectByID - one level-tree walk, O(log n), no allocation; also per SHAPECHECK
// watch, <= 2), a vftable read, IsDead, the ragdoll read and 14 GetElementWorldMatrix calls; the VISADJ line 1 Hz. No RTTI walk, no name
// lookup per update.
void HandCollisionTick(void* ent)
{
    if (!HandCollision) {
        // Review F7: switched off at runtime - forget everything that names an enemy or a place, so switching back on starts
        // clean (no SHAPECHECK watch on a pointer that may be freed by then, no query centred on old shoulders).
        if (g_nt) { g_nt = 0; }
        for (Watch& w : g_watch) { w.on = false; }
        for (HandState& h : g_h) { h.haveS = false; h.haveTip = false; h.haveContact = false; }
        g_player = nullptr;
        return;
    }
    if (!Resolve()) { return; }
    g_tickThread = GetCurrentThreadId();
    const double now = NowS();
    g_tickS = now;
    uintptr_t player = 0;
    if (!ent || !ReadPtr(reinterpret_cast<uintptr_t>(ent) + kVisPlayerOff, &player)) { player = 0; }
    g_player = reinterpret_cast<void*>(player);
    void* playerGso = player ? reinterpret_cast<void*>(player + kGsoOff) : nullptr;
    if (playerGso && !g_idSelfTestSaid) {
        g_idSelfTestSaid = true;
        int pid = 0;
        void* found = nullptr;
        const bool okId = CallGetId(playerGso, &pid), okFind = okId && CallFindById(playerGso, pid, &found);
        // [PROBE: HANDCOLL] gate=hand_collision  site=FLAT  log-only=yes
        LOG("handcoll: [HANDCOLL] ID CHECK self-test (logged unconditionally): player id %d -> the lookup returns %s (review F3: every "
            "tracked enemy is re-validated this way before it is read)", pid,
            !okId ? "NOTHING - GetID faulted" : !okFind ? "NOTHING - FindObjectByID faulted" : found == playerGso ? "the player (OK)"
            : found ? "ANOTHER object - enemies whose id another object shares will be refused" : "null");
    }
    for (int i = g_nt - 1; i >= 0; --i) {
        // Review F3 (INV-663): the engine's id lookup first - a despawned enemy is dropped without one read of its memory.
        if (!StillInEngine(playerGso, g_t[i].gso, g_t[i].id)) {
            ++g_idDrops;
            DropAt(i, playerGso ? "no longer returned by the engine's id lookup - despawned, not read"
                                : "the player is unreadable - cannot re-validate", now);
            continue;
        }
        const char* why = LiveCheck(g_t[i].ai, nullptr);
        if (why) { DropAt(i, why, now); continue; }
        Pose(&g_t[i]);
    }
    ShapeCheckTick(now, playerGso);
    if (player && now - g_lastQuery >= kQueryEveryS) {
        g_lastQuery = now;
        Query(reinterpret_cast<void*>(player), now);
    }
    VisAdjLine(now);
}

// COST per solve: off = one bool test; on, holding something = one edge update; empty = one segment test per posed shape (<= 84).
bool HandCollisionStop(int armIdx, bool handEmpty, const float S[3], const float tipOff[3], float T[3])
{
    if (!HandCollision || g_resolved != 1 || armIdx < 0 || armIdx > 1) { return false; }
    if (GetCurrentThreadId() != g_tickThread) {
        ++g_offThread;
        return false;
    }
    HandState& h = g_h[armIdx];
    const double now = NowS();
    if (Finite3(S)) {
        for (int i = 0; i < 3; ++i) { h.S[i] = S[i]; }
        h.haveS = true;
        h.sAtS = now;
    }
    if (++h.calls == 1) {
        // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
        LOG("handcoll: [HANDCOLL] %s hand first stop call (logged unconditionally) - hand %s, %d enemies tracked, thread %lu",
            ArmName(armIdx), handEmpty ? "EMPTY" : "HOLDING SOMETHING (passes through)", g_nt, GetCurrentThreadId());
    }
    bool stop = false;
    float depth = 0.0f, reachGap = 0.0f;
    int hitT = -1, hitK = -1;
    float hitPoint[3] = {};
    // The real fingertip's velocity (push / grab, plan 2026-10-31 Task 3): from successive solves on this thread, before the stop moves
    // T. A gap over 0.1 s (no solve) restarts it. COST: a few float ops per solve.
    if (Finite3(T) && Finite3(tipOff)) {
        const float tipNow[3] = { T[0] + tipOff[0], T[1] + tipOff[1], T[2] + tipOff[2] };
        const double dt = now - h.prevTipS;
        if (h.haveTip && dt > 1.0e-4 && dt <= 0.1) {
            for (int i = 0; i < 3; ++i) { h.vel[i] = static_cast<float>((tipNow[i] - h.prevTip[i]) / dt); }
        } else if (!h.haveTip || dt > 0.1) {
            for (float& v : h.vel) { v = 0.0f; }
        }
        for (int i = 0; i < 3; ++i) { h.prevTip[i] = tipNow[i]; }
        h.prevTipS = now;
        h.haveTip = true;
    }
    if (handEmpty && g_nt > 0 && Finite3(T) && Finite3(tipOff)) {
        static hc::WorldShape s_all[kMaxTracked * kNumBones];
        static int s_from[kMaxTracked * kNumBones][2];
        int n = 0;
        for (int i = 0; i < g_nt; ++i) {
            for (int k = 0; k < g_t[i].n; ++k) {
                if (!g_t[i].posed[k]) { continue; }
                s_all[n] = g_t[i].w[k];
                s_from[n][0] = i;
                s_from[n][1] = k;
                ++n;
            }
        }
        const float tip[3] = { T[0] + tipOff[0], T[1] + tipOff[1], T[2] + tipOff[2] };
        float out[3];
        int hit = -1;
        stop = hc::StopTarget(S, tip, s_all, n, out, &hit);
        if (stop) {
            const float sh[3] = { out[0] - tip[0], out[1] - tip[1], out[2] - tip[2] };
            depth = std::sqrt(sh[0] * sh[0] + sh[1] * sh[1] + sh[2] * sh[2]);
            for (int i = 0; i < 3; ++i) { T[i] += sh[i]; hitPoint[i] = out[i]; }
            hitT = s_from[hit][0];
            hitK = s_from[hit][1];
            h.nearestGap = 0.0f;
        } else {
            // The reach (push_grab_reach_m): a fingertip just short of a surface is a contact for the push / grab only - T is not
            // moved, no buzz, no TOUCH line. COST: one point-to-segment distance per posed shape, only on a solve that did not stop.
            float surf[3], nrm[3], nearest = -1.0f;
            int nearI = -1;
            if (hc::NearestSurface(S, tip, s_all, n, PushGrabReachM, &reachGap, &nearI, surf, nrm, &nearest)) {
                for (int i = 0; i < 3; ++i) { hitPoint[i] = surf[i]; }
                hitT = s_from[nearI][0];
                hitK = s_from[nearI][1];
            }
            h.nearestGap = nearest;
        }
        h.nearestS = now;
    } else {
        h.nearestGap = -1.0f;   // not measured: the hand holds something, nothing is tracked, or no pose
        h.nearestS = now;
    }
    h.haveContact = hitT >= 0;
    if (h.haveContact) {
        const Tracked& t = g_t[hitT];
        const hc::WorldShape& w = t.w[hitK];
        HandContact& c = h.contact;
        c.ai = t.ai;
        c.player = g_player;
        c.aiId = t.id;
        c.bodyIndex = t.def[hitK];
        // The outward normal: from the closest point of the shape's axis (a sphere: a == b) to the surface point.
        float ab[3] = { w.b[0] - w.a[0], w.b[1] - w.a[1], w.b[2] - w.a[2] };
        const float ab2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float u = 0.0f;
        if (ab2 > 1.0e-12f) {
            u = ((hitPoint[0] - w.a[0]) * ab[0] + (hitPoint[1] - w.a[1]) * ab[1] + (hitPoint[2] - w.a[2]) * ab[2]) / ab2;
            u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        }
        float n[3] = {};
        for (int i = 0; i < 3; ++i) { n[i] = hitPoint[i] - (w.a[i] + ab[i] * u); }
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int i = 0; i < 3; ++i) {
            c.point[i] = hitPoint[i];
            c.outwardNormal[i] = nl > 1.0e-6f ? n[i] / nl : 0.0f;
            c.realVel[i] = h.vel[i];
        }
        c.tS = now;
        c.gapM = stop ? 0.0f : reachGap;
    }
    const bool rising = h.edge.Update(stop, now);
    ++h.frames;
    if (stop) {
        ++h.stopped;
        h.depthSum += depth;
        if (depth > h.depthMax) { h.depthMax = depth; }
    }
    if (rising) {
        ++h.touchesWin;
        if (hitT >= 0) { ++g_t[hitT].touches; }
        const int ms = HandCollisionBuzzMs < 1.0f ? 1 : static_cast<int>(HandCollisionBuzzMs);
        const bool pulsed = HandCollisionBuzzAmp > 0.0f &&
                            PulseController(armIdx == 0 ? kRightHand : kLeftHand, HandCollisionBuzzAmp, ms);
        if (pulsed) { ++h.pulses; }
        if (EventLine()) {
            // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("handcoll: [HANDCOLL] TOUCH %s hand - enemy %p, shape %s | depth %.1f mm (the real fingertip past the surface) | pulse %s"
                " | GAP %.2f m (body centres, horizontal, last query)",
                ArmName(armIdx), hitT >= 0 ? g_t[hitT].ai : nullptr, hitT >= 0 ? kBones[g_t[hitT].def[hitK]].name : "?",
                static_cast<double>(depth * 1000.0f), pulsed ? "yes" : "NO (amp 0 or no vibration action)",
                static_cast<double>(hitT >= 0 ? g_t[hitT].bodyXZ : -1.0f));
        }
    }
    // Review F6: an untouched window is restarted silently every second, so a printed window never spans the time since the
    // process started (HC1's first line: "solves 2521, stopped 1") or since the last contact.
    if (!h.touching && !stop && now - h.winS >= 1.0) {
        h.winS = now;
        h.frames = h.stopped = h.touchesWin = h.pulses = 0;
        h.depthSum = 0.0;
        h.depthMax = 0.0f;
    }
    if (stop) { h.touching = true; }
    if (h.touching && now - h.winS >= 1.0) {
        if (WindowLine()) {
            // [PROBE: HANDCOLL] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("handcoll: [HANDCOLL] 1 s %s hand | solves %ld, stopped %ld | touches %ld, pulses %ld | depth mean %.1f max %.1f mm | "
                "%d tracked | off-thread solves (no stop) %lld", ArmName(armIdx), h.frames, h.stopped, h.touchesWin, h.pulses,
                h.stopped ? h.depthSum * 1000.0 / static_cast<double>(h.stopped) : 0.0, static_cast<double>(h.depthMax * 1000.0f),
                g_nt, g_offThread);
        }
        h.winS = now;
        h.frames = h.stopped = h.touchesWin = h.pulses = 0;
        h.depthSum = 0.0;
        h.depthMax = 0.0f;
        h.touching = stop;
    }
    return stop;
}

// COST: a copy. The contact counts only when made by a solve after this update's tick (an older one is from an earlier update).
bool HandCollisionContact(int armIdx, HandContact* out)
{
    if (!HandCollision || g_resolved != 1 || armIdx < 0 || armIdx > 1 || !out) { return false; }
    const HandState& h = g_h[armIdx];
    if (!h.haveContact || h.contact.tS < g_tickS) { return false; }
    *out = h.contact;
    return true;
}

// COST: two loads.
bool HandCollisionNearest(int armIdx, float* gapM, double* atS)
{
    if (!HandCollision || armIdx < 0 || armIdx > 1 || !gapM || !atS) { return false; }
    const double t = g_h[armIdx].nearestS;
    const float g = g_h[armIdx].nearestGap;
    if (!(t > 0.0) || g < 0.0f) { return false; }
    *gapM = g;
    *atS = t;
    return true;
}

// COST: the engine's id lookup (one level-tree walk) + the per-update live check (a vftable read, IsDead, the ragdoll read).
bool HandCollisionRevalidate(const HandContact& c)
{
    if (!HandCollision || g_resolved != 1 || !c.ai || !c.player) { return false; }
    void* playerGso = static_cast<char*>(c.player) + kGsoOff;
    if (!StillInEngine(playerGso, static_cast<char*>(c.ai) + kGsoOff, c.aiId)) { return false; }
    return LiveCheck(c.ai, nullptr) == nullptr;
}

// COST: the id lookup + a vftable read + IsDead + the ragdoll read (the grab's wait for the fall, per update while waiting / held).
int HandCollisionAiState(void* ai, int id, void* player)
{
    if (!HandCollision || g_resolved != 1 || !ai || !player) { return -1; }
    if (!StillInEngine(static_cast<char*>(player) + kGsoOff, static_cast<char*>(ai) + kGsoOff, id)) { return -1; }
    uintptr_t vt = 0, s37 = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(ai), &vt) || !vt || !ReadPtr(vt + 8 * kIsDeadSlot, &s37) || s37 != g_gd + g_rIsDead) {
        return -1;
    }
    bool dead = false, rag = false;
    if (!CallIsDead(ai, s37, &dead)) { return -1; }
    if (dead) { return 2; }
    if (!HasRagdoll(ai, &rag)) { return -1; }
    return rag ? 1 : 0;
}

// COST: a copy.
bool HandCollisionArmVelocity(int armIdx, float out[3])
{
    if (!HandCollision || armIdx < 0 || armIdx > 1 || !out || !g_h[armIdx].haveTip) { return false; }
    for (int i = 0; i < 3; ++i) { out[i] = g_h[armIdx].vel[i]; }
    return true;
}

// COST: a copy.
bool HandCollisionArmTip(int armIdx, float out[3])
{
    if (!HandCollision || armIdx < 0 || armIdx > 1 || !out || !g_h[armIdx].haveTip) { return false; }
    for (int i = 0; i < 3; ++i) { out[i] = g_h[armIdx].prevTip[i]; }
    return true;
}

// The zombie close approach (spec 2026-10-31-zombie-close-approach-and-grab-intent §2). COST: one pointer-sized load.
void* HandCollisionPlayer()
{
    return *static_cast<void* volatile*>(&g_player);
}

bool HandCollisionObjectId(void* obj, int* out)
{
    if (!obj || !out || !g_getId) { return false; }
    return CallGetId(static_cast<char*>(obj) + kGsoOff, out);
}

}  // namespace dlvr
