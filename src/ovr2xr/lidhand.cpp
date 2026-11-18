// lidhand.cpp - see lidhand.h. Spec docs/superpowers/specs/2026-11-18-hand-open-lids-design.md (Jay 2026-11-18: "not affect the
// players view or positioning at all", "physically grab the top of the container and open it/close it", "the item to physically
// appear in the hand until the player stows it", close "Visual only", "Top lids first", option "A"). Trace
// docs/traces/20261118_1711_hand_open_lids.md. Facts (OUR gamedll 97046bba / engine e9fbfc2c): RESEARCH-NATIVE-LOOTING §12-§13.
//
// WHAT IT DOES:
//   - NearbyHook (looting.cpp, HUD trigger update) hands us the game's gathered triggers: ComplexContainers whose mesh + lid bone
//     match lid_hinge_table.inc, and ContainerItems, are published (seqlock); a game pick on one of them is replaced by the game's
//     own NULL pick (the caller's CallSetter) - no prompt, X has no target, so the game's interaction (walk-to, look turn, the
//     player clip - §12) never starts for these containers.
//   - LidHandTick (game thread): a grip edge with the hand in reach of a published lid's box (top half; state 2 or 1 - never 10,
//     locked) owns that lid; its angle follows the hand about the hinge (relative to the grab); SetElementLocalMatrix(lid bone,
//     R(axis, deg) . rest) each tick while it is off rest, the local read back first (an overwrite is counted, logged once).
//     First crossing of lid_open_frac x open in state 2 -> one +B9B930(player, container, info): the game's own open (points,
//     state 1, SpawnItems, its own lid clip). Option A: the hand lets go there and our write for that lid stops until the next grab.
//   - Items: a grip edge in reach of a ContainerItem whose container is open past the open point -> HandCarryTakeAs onto r_hand /
//     l_hand (home = its current parent element = the hook the game attached it to). Grip open in the shoulder zone with the item's
//     action == 1 -> give back, then +B9B930(player, item, info) = the game's own take; anywhere else -> give back. The container
//     leaving state 1 while carried -> forget (the game destroys its placed items, §13.5).
//   - LidHandPoll (scePad poll) withholds L1 / R1 while a hand owns a lid / item or is next to one with the grip squeezed.
// THREADS: LidHandOnNearby on the HUD update (presumed the game thread - looting.cpp's note); LidHandTick on the game thread;
// LidHandPoll on the pad thread. They share a seqlocked list and a few interlocked longs only.
#include "lidhand.h"

#include "boneblend.h"      // RigGripWorldRead (game world grip pose)
#include "camhook.h"        // BoneProbeCapturedModel (the player's FPP body model)
#include "controllers.h"    // GetControllerSqueeze, GetControllerGripPose
#include "handcarry.h"      // the item on the hand (INV-613 engine attach)
#include "headpose.h"       // GetHeadPose (the shoulder zone)
#include "holster.h"        // HolsterZoneLocal, HolsterCenterX / Y / Z, HolsterRadius - the right-shoulder zone
#include "holster_logic.h"  // holster::kExitScale
#include "lidhand_logic.h"
#include "looting.h"        // LootReachM (the same reach as loot by hand)
#include "looting_logic.h"  // looting::BoxDist
#include "rtti_vtable.h"    // RttiFindVtable
#include "twohand.h"        // RigTwoHandHaptic - the buzz strength (trace 20261118_1905: reuse, no new key)

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_LID_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_LID_GD_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: lid_by_hand] required-for=opening top-lid containers by hand + items held until stowed (LidByHand) - spec
//      2026-11-18-hand-open-lids-design. tree=INERT at 0: LidHandOnNearby / LidHandTick / LidHandPoll return at their first test,
//      nothing is read or written. Needs loot_by_hand=1 (the shared GetNearbyTriggers detour installs only then). HEADSET: the grip
//      poses + squeeze, the rig's game-world grip (RigGripWorldRead), the player body model (BoneProbeCapturedModel). Tier C -
//      SHIPS OFF from 1.04 (Jay 2026-11-18: "Ship it OFF"; trace 20261118_2045): it withholds the game's prompt on every lid
//      container incl. a locked one's lockpick, and roomscale pushes the player off the chest. Jay tests it ON on his disk.
bool  LidByHand = false;
// [DEP: lid_open_frac] required-for=where the game's own open fires, as a fraction of the lid's open angle (LidOpenFrac). Tier D
//      (my call 0.5, spec §4.4). tree=INERT at lid_by_hand=0.
float LidOpenFrac = 0.5f;

namespace {

namespace lh = lidhand;
#include "lid_hinge_table.inc"   // kLidHinges - tools/lid_hinge_table.py (OUR data, controls in the tool)
constexpr int kRows = static_cast<int>(sizeof(kLidHinges) / sizeof(kLidHinges[0]));

// ---- OUR gamedll 97046bba (RESEARCH §12d / §12e / §13) ----
constexpr uintptr_t kExecuteRva = 0xB9B930;   // sub_180B9B930(player, trigger, info) - the game's execute (§12d)
constexpr size_t    kStateOff = 824;           // PersistentTriggerObjectDI state (ComplexContainer slot 195 +A57BF0 returns it)
constexpr size_t    kItemOwnerOff = 1016;      // ContainerItem -> its ComplexContainer (CreateItem +A6E720 writes it, §12e)
constexpr size_t    kActionSlotOff = 1432;     // vfunc: the trigger's action for a use info (ContainerItem +A69FD0: 1 or 23)
constexpr size_t    kCtlObjOff = 24;           // IControlObject (INV-402)
constexpr int       kStateOpened = 1, kStateClosed = 2, kStateLocked = 10;
constexpr float     kGrip = 0.5f;              // ovr2xr.cpp's vr_grip threshold
constexpr int       kMaxPub = 16;
constexpr int       kMaxLids = 8;
constexpr float     kDropDistM = 6.0f;         // a lid this far from the hand stops being written (my call)
// The lid grab zone (trace 20261118_2010, Jay "Yes"): the hand within this of the lid's FAR EDGE (kLidHinges[].edge, a segment that
// rides the lid). Was 0.35 m round the whole box + a top band (trace 20261118_1905) - RUN LH3 grabbed the basket 0.267 m outside its
// box, ~1.0 m from the hinge. My call, a constant (no new key).
constexpr float     kLidEdgeReachM = 0.20f;

// The use info +B9B930 takes, built as the game builds it (+D2D7A0 / +BC9510; §12e): 40 bytes.
struct UseInfo {
    void*         actor;
    int           slot;          // 0 = primary
    float         f0;            // 0.1
    float         f1;            // 2.6
    unsigned char flags;         // 0
    unsigned char pad[3];
    float         v[4];          // xmmword_181928640 = (1.5708, 1.5708, 1, 1)
};
static_assert(sizeof(UseInfo) == 40, "UseInfo layout (+D2D7A0 / +BC9510 build it on the stack: 8 + 4 + 4 + 4 + 4 + 16)");

struct StrBase { char* p; unsigned size; unsigned cap; };   // ttl::string_base<char> as the game passes it (ptr / size / cap)

using SetLocalFn = void(__fastcall*)(void* model, int elem, const float* m);
using GetLocalFn = const float*(__fastcall*)(void* model, int elem);
using GetIdFn = int(__fastcall*)(void* model, const char* name);
using GetWorldElemFn = const float*(__fastcall*)(void* model, int elem);
using GetParentFn = int(__fastcall*)(void* model, int elem);
using GetMeshNameFn = StrBase*(__fastcall*)(void* model, StrBase* out);
using GetXformFn = const float*(__fastcall*)(void* co);
using GetParentMeshFn = int(__fastcall*)(void* co);
using GetExtentsFn = const float*(__fastcall*)(void* co);
using ExecuteFn = void(__fastcall*)(void* player, void* trigger, UseInfo* info);
using ActionFn = int(__fastcall*)(void* trigger, UseInfo* info);

SetLocalFn      g_setLocal = nullptr;
GetLocalFn      g_getLocal = nullptr;
GetIdFn         g_getId = nullptr;
GetWorldElemFn  g_getWorldElem = nullptr;
GetParentFn     g_getParent = nullptr;
GetMeshNameFn   g_getMeshName = nullptr;
GetXformFn      g_getXform = nullptr;
GetParentMeshFn g_getParentMesh = nullptr;
GetExtentsFn    g_getExtents = nullptr;
ExecuteFn       g_execute = nullptr;
uintptr_t       g_vtContainer = 0, g_vtItem = 0;
volatile LONG   g_ready = 0;      // 0 not tried, 1 ready, -1 refused (named once)
void* volatile  g_player = nullptr;   // the PlayerDI NearbyHook was called with (the execute's actor)

// ---- published by LidHandOnNearby (HUD) -> LidHandTick (game) ----
volatile LONG g_pubSeq = 0;
int   g_pubLidN = 0, g_pubItemN = 0;
void* g_pubLid[kMaxPub] = {};
int   g_pubLidRow[kMaxPub] = {};
int   g_pubLidBone[kMaxPub] = {};
float g_pubLidBox[kMaxPub][6] = {};
void* g_pubItem[kMaxPub] = {};
float g_pubItemBox[kMaxPub][6] = {};

// identity cache (HUD thread only): object -> row / bone, or -1 = not a lid (checked once per object pointer)
struct IdCache { void* obj; int row; int bone; };
IdCache g_idc[64] = {};
int     g_idcNext = 0;

// ---- per hand / per lid (game thread), and what the pad thread reads ----
struct LidState {
    void* obj = nullptr; int row = -1; int bone = -1;
    float deg = 0.0f;           // our last written angle
    bool  writing = false;      // write every tick (off rest, owned, or released off rest)
    bool  gameClip = false;     // the game's own clip took it at the open (option A) - no write until the next grab
    bool  overwriteSaid = false;
};
LidState g_lids[kMaxLids];
struct HandState {
    int   lid = -1;             // index into g_lids while this hand owns a lid
    float lidAtGrab = 0.0f, grabW[3] = {};
    bool  prevDown = false;
    bool  prevZone = false;     // the hand was in a lid's / an item's grab zone last tick (the entry buzz's edge)
    HandCarry carry;            // the item while carried
    void* item = nullptr;
    void* itemOwner = nullptr;
};
HandState g_hand[2];            // 0 = right, 1 = left (RigGripWorldRead's sides)
volatile LONG g_busy[2] = {};   // tick -> pad: this hand owns a lid / item, or is next to one
volatile LONG g_padOwned[2] = {};   // pad -> tick: an earlier poller owns this press
void* volatile g_carried[2] = {};   // tick -> HUD: the items carried (their pick is nulled)
volatile LONG g_nGrab = 0, g_nOpen = 0, g_nTake = 0, g_nStow = 0, g_nGiveBack = 0, g_nForgot = 0, g_nOverwrite = 0, g_nNull = 0;

// ---- SEH-guarded game access (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadI32(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeBox(void* obj, float out[6])
{
    __try {
        const float* e = g_getExtents(static_cast<char*>(obj) + kCtlObjOff);
        if (!e) { return false; }
        for (int k = 0; k < 6; ++k) { out[k] = e[k]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeMeshName(void* obj, char* out, size_t n)
{
    // The returned string is NOT freed (the engine's heap is not ours to free into); called once per object (the identity cache).
    __try {
        StrBase s{};
        g_getMeshName(obj, &s);
        if (!s.p) { return false; }
        strncpy_s(out, n, s.p, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeElemId(void* obj, const char* name, int* out)
{
    __try { *out = g_getId(obj, name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeLocal(void* obj, int elem, float out[12])
{
    __try {
        const float* m = g_getLocal(obj, elem);
        if (!m) { return false; }
        memcpy(out, m, 12 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetLocal(void* obj, int elem, const float m[12])
{
    __try { g_setLocal(obj, elem, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The lid bone's PARENT frame in world: its parent element's world, or the object's world xform when it is a root-level element.
bool SafeParentWorld(void* obj, int elem, float out[12])
{
    __try {
        const int p = g_getParent(obj, elem);
        const float* m = p >= 0 ? g_getWorldElem(obj, p) : g_getXform(static_cast<char*>(obj) + kCtlObjOff);
        if (!m) { return false; }
        memcpy(out, m, 12 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeParentMeshElem(void* obj, int* out)
{
    __try { *out = g_getParentMesh(static_cast<char*>(obj) + kCtlObjOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeExecute(void* player, void* trig)
{
    UseInfo info{};
    info.actor = player;
    info.slot = 0;
    info.f0 = 0.1f;
    info.f1 = 2.6f;
    info.flags = 0;
    info.v[0] = 1.5707964f; info.v[1] = 1.5707964f; info.v[2] = 1.0f; info.v[3] = 1.0f;
    __try { g_execute(player, trig, &info); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeAction(void* trig, void* player, int* out)
{
    UseInfo info{};
    info.actor = player;
    info.f0 = 0.1f;
    info.f1 = 2.6f;
    info.v[0] = 1.5707964f; info.v[1] = 1.5707964f; info.v[2] = 1.0f; info.v[3] = 1.0f;
    __try {
        uintptr_t vt = 0, fn = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(trig), &vt) || !ReadPtr(vt + kActionSlotOff, &fn) || !fn) { return false; }
        *out = reinterpret_cast<ActionFn>(fn)(trig, &info);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IsVt(void* obj, uintptr_t vt)
{
    uintptr_t v = 0;
    return obj && vt && ReadPtr(reinterpret_cast<uintptr_t>(obj), &v) && v == vt;
}
int State(void* obj)
{
    int s = -1;
    return ReadI32(reinterpret_cast<uintptr_t>(obj) + kStateOff, &s) ? s : -1;
}

void MulRot(const float m[12], const float v[3], float out[3])
{
    for (int r = 0; r < 3; ++r) { out[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2]; }
}
void MulPoint(const float m[12], const float v[3], float out[3])
{
    MulRot(m, v, out);
    for (int r = 0; r < 3; ++r) { out[r] += m[r * 4 + 3]; }
}

// +B9B930's first 16 bytes (set-aside gamedll 97046bba, 2026-11-18; control +B9BBB0 read looting's kNearbyBytes): test rdx,rdx /
// jz rel32 / push rbp / push rsi / push rdi / sub rsp,40h. The jz's rel32 (bytes 5-8) is not compared (another build may move its
// target); everything else must match, or the execute is never called (called, not patched).
bool ExecHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[16] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x02, 0x02, 0x00, 0x00, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    __try {
        for (int i = 0; i < 16; ++i) {
            if (i >= 5 && i <= 8) { continue; }
            if (p[i] != kHead[i]) { return false; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Resolve once: the engine exports by name, the execute, the two vtables by RTTI name. Refuses by name.
bool Ready()
{
    if (g_ready != 0) { return g_ready == 1; }
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!en || !gd || !HandCarryReady()) { return false; }   // not mapped yet / hand-carry not resolved yet: retried
    g_setLocal = reinterpret_cast<SetLocalFn>(GetProcAddress(en, "?SetElementLocalMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    g_getLocal = reinterpret_cast<GetLocalFn>(GetProcAddress(en, "?GetElementLocalMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_getId = reinterpret_cast<GetIdFn>(GetProcAddress(en, "?GetElementID@IModelObject@@QEBAHPEBD@Z"));
    g_getWorldElem = reinterpret_cast<GetWorldElemFn>(GetProcAddress(en, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_getParent = reinterpret_cast<GetParentFn>(GetProcAddress(en, "?GetElementParent@IModelObject@@QEBAHH@Z"));
    g_getMeshName = reinterpret_cast<GetMeshNameFn>(GetProcAddress(en, "?GetMeshName@IModelObject@@QEBA?AV?$string_base@D@ttl@@XZ"));
    g_getXform = reinterpret_cast<GetXformFn>(GetProcAddress(en, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
    g_getParentMesh = reinterpret_cast<GetParentMeshFn>(GetProcAddress(en, "?GetParentMeshElement@IControlObject@@QEBAHXZ"));
    g_getExtents = reinterpret_cast<GetExtentsFn>(GetProcAddress(en, "?GetExtentsInWorld@IControlObject@@QEBAAEBVextents@@XZ"));
    const uintptr_t gdb = reinterpret_cast<uintptr_t>(gd);
    int s1 = -1, s2 = -1;
    g_vtContainer = RttiFindVtable(gdb, ".?AVComplexContainer@@", 0, &s1);
    g_vtItem = RttiFindVtable(gdb, ".?AVContainerItem@@", 0, &s2);
    g_execute = reinterpret_cast<ExecuteFn>(gdb + DLVR_LID_GD_RVA(kExecuteRva));
    const bool headOk = ExecHeadOk(reinterpret_cast<const unsigned char*>(g_execute));
    const char* missing = !g_setLocal ? "SetElementLocalMatrix" : !g_getLocal ? "GetElementLocalMatrix" : !g_getId ? "GetElementID"
                        : !g_getWorldElem ? "GetElementWorldMatrix" : !g_getParent ? "GetElementParent"
                        : !g_getMeshName ? "GetMeshName" : !g_getXform ? "GetWorldXform" : !g_getParentMesh ? "GetParentMeshElement"
                        : !g_getExtents ? "GetExtentsInWorld" : !g_vtContainer ? "ComplexContainer vtable (RTTI)"
                        : !g_vtItem ? "ContainerItem vtable (RTTI)" : !headOk ? "the execute's code" : nullptr;
    if (missing) {
        InterlockedExchange(&g_ready, -1);
        // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
        LOG("lidhand: [LIDHAND] REFUSED - %s not found (RTTI stages container %d item %d) - lids open the game's way", missing, s1, s2);
        return false;
    }
    InterlockedExchange(&g_ready, 1);
    // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
    LOG("lidhand: [LIDHAND] READY - ComplexContainer vt +%llX (want +165A950 Steam), ContainerItem vt +%llX (want +165BA58), execute "
        "+%llX, %d lid rows (lid_open_frac %.2f)", static_cast<unsigned long long>(g_vtContainer - gdb),
        static_cast<unsigned long long>(g_vtItem - gdb), static_cast<unsigned long long>(DLVR_LID_GD_RVA(kExecuteRva)), kRows,
        LidOpenFrac);
    return true;
}

// HUD thread: is `obj` a lid container? -> row + bone, cached per object pointer.
bool LidIdentity(void* obj, int* row, int* bone)
{
    for (const IdCache& c : g_idc) {
        if (c.obj == obj) { *row = c.row; *bone = c.bone; return c.row >= 0; }
    }
    char mesh[96] = {};
    int r = -1, b = -1;
    const bool haveName = SafeMeshName(obj, mesh, sizeof(mesh));
    // The BASE name (trace docs/traces/20261118_1849_lid_mesh_name.md; RUN LH1 matched nothing): GetMeshName returns the FILE name,
    // "furniture_chest_anm.msh" (OUR dynamic_objects_containers.scr), so the folder and the extension go before the compare.
    char base[96] = {};
    if (haveName) {
        const char* s = mesh;
        for (const char* q = mesh; *q; ++q) { if (*q == '/' || *q == '\\') { s = q + 1; } }
        strncpy_s(base, sizeof(base), s, _TRUNCATE);
        char* dot = strrchr(base, '.');
        if (dot) { *dot = 0; }
    }
    int rowByName = -1;
    for (int i = 0; haveName && i < kRows && r < 0; ++i) {
        if (_stricmp(base, kLidHinges[i].mesh) == 0) {
            rowByName = i;
            int id = -1;
            if (SafeElemId(obj, kLidHinges[i].bone, &id) && id >= 0) { r = i; b = id; }
        }
    }
    static LONG s_firstCc = 0;
    if (InterlockedIncrement(&s_firstCc) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST ComplexContainer gathered (logged unconditionally): object %p mesh '%s' (base '%s') -> %s", obj,
            haveName ? mesh : "(name not read)", base, r >= 0 ? kLidHinges[r].object : rowByName >= 0 ? "a lid row, but its lid bone is missing" : "not a lid row");
    }
    if (r < 0 && rowByName >= 0) {
        static LONG s_noBone[kRows] = {};
        if (InterlockedIncrement(&s_noBone[rowByName]) == 1) {
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] ROW MATCHED BUT NO LID BONE - object %p mesh '%s' = row %s, bone '%s' not found in it - not a lid",
                obj, mesh, kLidHinges[rowByName].object, kLidHinges[rowByName].bone);
        }
    } else if (r < 0) {
        static char s_said[16][64] = {};
        static int  s_nSaid = 0;
        bool known = false;
        for (int k = 0; k < s_nSaid && !known; ++k) { known = _stricmp(s_said[k], base) == 0; }
        if (!known && s_nSaid < 16) {
            strncpy_s(s_said[s_nSaid++], sizeof(s_said[0]), base, _TRUNCATE);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] NOT A LID ROW - a ComplexContainer with mesh '%s' (base '%s') matches no row (%d of 16 names logged)",
                haveName ? mesh : "(name not read)", base, s_nSaid);
        }
    }
    g_idc[g_idcNext] = { obj, r, b };
    g_idcNext = (g_idcNext + 1) % static_cast<int>(sizeof(g_idc) / sizeof(g_idc[0]));
    if (r >= 0) {
        static LONG s_saidRow[kRows] = {};
        if (InterlockedIncrement(&s_saidRow[r]) == 1) {
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] FIRST %s seen: object %p mesh %s, lid bone %s = element %d (hinge axis %.3f %.3f %.3f, open %.2f deg)",
                kLidHinges[r].object, obj, mesh, kLidHinges[r].bone, b, kLidHinges[r].axis[0], kLidHinges[r].axis[1],
                kLidHinges[r].axis[2], kLidHinges[r].openDeg);
        }
    }
    *row = r;
    *bone = b;
    return r >= 0;
}

bool HandWorldPos(int side, float out[3])
{
    float rot[9];
    long long qpc = 0;
    return RigGripWorldRead(side, out, rot, &qpc);
}

// The right hand at the right shoulder: bownock.cpp's HandInShoulder (the holster's zone, its 1.2x exit). The left hand: the same
// zone mirrored to the left shoulder (my call - the left holster's own flag belongs to its poll).
bool InShoulder(int side)
{
    static bool s_in[2] = {};
    XrPosef head{}, grip{};
    if (!GetHeadPose(head) || !GetControllerGripPose(side == 0 ? kRightHand : kLeftHand, grip)) { s_in[side] = false; return false; }
    const float hp[3] = { head.position.x, head.position.y, head.position.z };
    const float hq[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
    const float gp[3] = { grip.position.x, grip.position.y, grip.position.z };
    float lv[3];
    HolsterZoneLocal(kHolsterShoulder, kHolsterCallerBow, hp, hq, gp, lv);
    const float cx = side == 0 ? HolsterCenterX : -HolsterCenterX;
    const float dx = lv[0] - cx, dy = lv[1] - HolsterCenterY, dz = lv[2] - HolsterCenterZ;
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float exitR = HolsterRadius * holster::kExitScale;
    if (s_in[side]) { if (d2 > exitR * exitR) { s_in[side] = false; } }
    else if (d2 < HolsterRadius * HolsterRadius) { s_in[side] = true; }
    return s_in[side];
}

int FindLidSlot(void* obj)
{
    for (int i = 0; i < kMaxLids; ++i) { if (g_lids[i].obj == obj) { return i; } }
    return -1;
}
int NewLidSlot(void* obj, int row, int bone)
{
    int i = FindLidSlot(obj);
    if (i >= 0) { return i; }
    for (int k = 0; k < kMaxLids; ++k) {
        if (!g_lids[k].obj || !g_lids[k].writing) {
            if (g_hand[0].lid == k || g_hand[1].lid == k) { continue; }
            g_lids[k] = LidState{};
            g_lids[k].obj = obj; g_lids[k].row = row; g_lids[k].bone = bone;
            return k;
        }
    }
    return -1;
}

// The lid's current angle from its own local (our write or the game's clip), about the row's axis.
bool LidNowDeg(const LidState& L, float* deg)
{
    float cur[12];
    if (!SafeLocal(L.obj, L.bone, cur)) { return false; }
    *deg = lh::LidAngleFromLocal(kLidHinges[L.row].rest, cur, kLidHinges[L.row].axis);
    return true;
}

// The hinge in world: pivot = parent world x the rest translation, axis = parent world rotation x the row's axis.
bool HingeWorld(const LidState& L, float pivotW[3], float axisW[3])
{
    float pw[12];
    if (!SafeParentWorld(L.obj, L.bone, pw)) { return false; }
    MulPoint(pw, kLidHinges[L.row].pivot, pivotW);
    MulRot(pw, kLidHinges[L.row].axis, axisW);
    return true;
}

void WriteLid(LidState& L, float deg)
{
    const LidHinge& h = kLidHinges[L.row];
    float now[12], want[12];
    // read back first: anything that changed the local since our last write is counted (§13.3 ruling)
    if (SafeLocal(L.obj, L.bone, now) && L.writing) {
        float last[12];
        lh::LidLocal(h.rest, h.axis, L.deg, last);
        float worst = 0.0f;
        for (int k = 0; k < 12; ++k) { worst = std::fmax(worst, std::fabs(now[k] - last[k])); }
        if (worst > 1.0e-4f) {
            InterlockedIncrement(&g_nOverwrite);
            if (!L.overwriteSaid) {
                L.overwriteSaid = true;
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] OVERWRITE - %s %p: the lid's local differs from our last write by %.4f (something else writes "
                    "it between ticks; logged once per lid, counted)", h.object, L.obj, worst);
            }
        }
    }
    lh::LidLocal(h.rest, h.axis, deg, want);
    if (SafeSetLocal(L.obj, L.bone, want)) { L.deg = deg; L.writing = deg > 0.05f; }
}

void Publish(void* const* lids, const int* rows, const int* bones, float (*lboxes)[6], int nl, void* const* items,
             float (*iboxes)[6], int ni)
{
    InterlockedIncrement(&g_pubSeq);   // odd: writing
    for (int i = 0; i < nl; ++i) {
        g_pubLid[i] = lids[i]; g_pubLidRow[i] = rows[i]; g_pubLidBone[i] = bones[i];
        memcpy(g_pubLidBox[i], lboxes[i], sizeof(g_pubLidBox[i]));
    }
    for (int i = 0; i < ni; ++i) { g_pubItem[i] = items[i]; memcpy(g_pubItemBox[i], iboxes[i], sizeof(g_pubItemBox[i])); }
    g_pubLidN = nl;
    g_pubItemN = ni;
    InterlockedIncrement(&g_pubSeq);   // even: settled
}

struct Snapshot {
    int nl = 0, ni = 0;
    void* lid[kMaxPub]; int row[kMaxPub]; int bone[kMaxPub]; float lbox[kMaxPub][6];
    void* item[kMaxPub]; float ibox[kMaxPub][6];
};
bool ReadSnapshot(Snapshot& s)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG s0 = g_pubSeq;
        if (s0 & 1) { continue; }
        s.nl = g_pubLidN; s.ni = g_pubItemN;
        for (int i = 0; i < s.nl; ++i) {
            s.lid[i] = g_pubLid[i]; s.row[i] = g_pubLidRow[i]; s.bone[i] = g_pubLidBone[i];
            memcpy(s.lbox[i], g_pubLidBox[i], sizeof(s.lbox[i]));
        }
        for (int i = 0; i < s.ni; ++i) { s.item[i] = g_pubItem[i]; memcpy(s.ibox[i], g_pubItemBox[i], sizeof(s.ibox[i])); }
        if (g_pubSeq == s0) { return true; }
    }
    return false;
}

const char* kSideName[2] = { "right", "left" };

// The grip opened (or the item was lost) while carrying: stow or give back (spec §4.5).
void ReleaseItem(int side, bool lost)
{
    HandState& H = g_hand[side];
    if (!H.item) { return; }
    void* item = H.item;
    if (lost) {
        HandCarryForget(H.carry, "lidhand: the container left the opened state");
        InterlockedIncrement(&g_nForgot);
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FORGOT %s hand's item %p - its container left state 1 (the game removes its placed items, §13.5)",
            kSideName[side], item);
    } else {
        int action = -1;
        const bool haveAction = SafeAction(item, g_player, &action);
        const lh::ItemRelease what = lh::OnItemRelease(InShoulder(side), haveAction ? action : -1);
        const bool back = HandCarryGiveBack(H.carry, "lidhand: release");
        if (what == lh::ItemRelease::Stow && back) {
            const bool ok = SafeExecute(g_player, item);
            InterlockedIncrement(&g_nStow);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] STOW %s hand's item %p over the shoulder - the game's own take (+B9B930, action %d) %s",
                kSideName[side], item, action, ok ? "called" : "*** FAULTED ***");
        } else {
            InterlockedIncrement(&g_nGiveBack);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] GIVE BACK %s hand's item %p to its place (%s; action %d%s)%s", kSideName[side], item,
                InShoulder(side) ? "in the shoulder zone" : "away from the shoulder", action,
                action == 23 ? " = the inventory refuses it" : "", back ? "" : " *** give-back failed ***");
        }
    }
    H.item = nullptr;
    H.itemOwner = nullptr;
    g_carried[side] = nullptr;
}

}  // namespace

bool LidHandOnNearby(void* player, void* const* gameData, unsigned gameCount, void* const* gazeData, unsigned gazeCount)
{
    if (!LidByHand || !Ready() || !player) { return false; }
    g_player = player;
    void* lids[kMaxPub]; int rows[kMaxPub]; int bones[kMaxPub]; float lbox[kMaxPub][6];
    void* items[kMaxPub]; float ibox[kMaxPub][6];
    int nl = 0, ni = 0;
    const unsigned total = gameCount + gazeCount;
    for (unsigned i = 0; i < total; ++i) {
        uintptr_t o = 0;
        if (i < gameCount) {
            if (!ReadPtr(reinterpret_cast<uintptr_t>(gameData) + 8 * i, &o) || !o) { continue; }
        } else {
            o = reinterpret_cast<uintptr_t>(gazeData[i - gameCount]);
            if (!o) { continue; }
        }
        void* obj = reinterpret_cast<void*>(o);
        bool dup = false;
        for (int k = 0; k < nl && !dup; ++k) { dup = lids[k] == obj; }
        for (int k = 0; k < ni && !dup; ++k) { dup = items[k] == obj; }
        if (dup) { continue; }
        if (IsVt(obj, g_vtContainer) && nl < kMaxPub) {
            int r = -1, b = -1;
            if (LidIdentity(obj, &r, &b) && SafeBox(obj, lbox[nl])) { lids[nl] = obj; rows[nl] = r; bones[nl] = b; ++nl; }
        } else if (IsVt(obj, g_vtItem) && ni < kMaxPub) {
            if (SafeBox(obj, ibox[ni])) { items[ni] = obj; ++ni; }
        }
    }
    Publish(lids, rows, bones, lbox, nl, items, ibox, ni);
    static LONG s_first = 0;
    if (InterlockedIncrement(&s_first) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST gather (logged unconditionally): %u game + %u gaze candidates -> %d lid container(s), %d "
            "container item(s) | thread %lu", gameCount, gazeCount, nl, ni, GetCurrentThreadId());
    }
    // the game's pick: a lid container or a carried item -> the caller writes the game's own NULL pick
    uintptr_t h = 0, pick = 0;
    const uintptr_t pl = reinterpret_cast<uintptr_t>(player);
    if (!ReadPtr(pl + 5272, &h) || !h || !ReadPtr(pl + 5280, &pick) || !pick) { return false; }
    bool nullIt = pick == reinterpret_cast<uintptr_t>(g_carried[0]) || pick == reinterpret_cast<uintptr_t>(g_carried[1]);
    for (int k = 0; k < nl && !nullIt; ++k) { nullIt = reinterpret_cast<uintptr_t>(lids[k]) == pick; }
    if (nullIt && InterlockedIncrement(&g_nNull) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST NULL PICK - the game picked %p (a lid container / a carried item): its prompt and X are "
            "withheld, the game's interaction never starts for it", reinterpret_cast<void*>(pick));
    }
    return nullIt;
}

void LidHandTick()
{
    if (!LidByHand || g_ready != 1 || !g_player) {
        g_busy[0] = g_busy[1] = 0;
        return;
    }
    Snapshot s;
    const bool haveSnap = ReadSnapshot(s);
    void* body = const_cast<void*>(BoneProbeCapturedModel());
    // each published lid's far edge in world, ONCE per tick (both hands read it): parent world . the bone's CURRENT local . edge
    float edgeA[kMaxPub][3], edgeB[kMaxPub][3];
    bool haveEdge[kMaxPub] = {};
    for (int i = 0; haveSnap && i < s.nl; ++i) {
        float cur[12], pw[12];
        if (SafeLocal(s.lid[i], s.bone[i], cur) && SafeParentWorld(s.lid[i], s.bone[i], pw)) {
            lh::EdgeWorld(pw, cur, kLidHinges[s.row[i]].edge, edgeA[i], edgeB[i]);
            haveEdge[i] = true;
        }
    }
    for (int side = 0; side < 2; ++side) {
        HandState& H = g_hand[side];
        float sq = 0.0f;
        const bool down = GetControllerSqueeze(side == 0 ? kRightHand : kLeftHand, sq) && sq >= kGrip;
        const bool edge = down && !H.prevDown;
        H.prevDown = down;
        float hw[3];
        const bool haveHand = HandWorldPos(side, hw);
        bool isNear = false;   // (not `near`: windows.h defines near / far as empty macros)
        // ---- a carried item ----
        if (H.item) {
            if (!IsVt(H.itemOwner, g_vtContainer) || State(H.itemOwner) != kStateOpened || !HandCarryStillOn(H.carry)) {
                ReleaseItem(side, true);
            } else if (!down) {
                ReleaseItem(side, false);
            }
            isNear = H.item != nullptr;
        }
        // ---- an owned lid ----
        if (H.lid >= 0) {
            LidState& L = g_lids[H.lid];
            float pivotW[3], axisW[3];
            if (!down || !IsVt(L.obj, g_vtContainer) || !haveHand || !HingeWorld(L, pivotW, axisW)) {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] RELEASE %s hand's lid %s at %.1f deg (%s)", kSideName[side], kLidHinges[L.row].object, L.deg,
                    !down ? "grip opened" : !haveHand ? "no hand pose" : "the container is gone");
                H.lid = -1;
            } else {
                const float openDeg = kLidHinges[L.row].openDeg;
                const float prev = L.deg;
                const float deg = lh::ClampLid(H.lidAtGrab + lh::HandAngleDeg(pivotW, axisW, H.grabW, hw), openDeg);
                WriteLid(L, deg);
                isNear = true;
                if (lh::CrossesOpenPoint(prev, deg, openDeg, LidOpenFrac) && State(L.obj) == kStateClosed) {
                    const bool ok = SafeExecute(g_player, L.obj);
                    const int st = State(L.obj);
                    InterlockedIncrement(&g_nOpen);
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] OPEN CALL %s %p at %.1f deg (open point %.1f): the game's own execute (+B9B930, action 6) %s"
                        " -> state %d (want 1) - option A: the hand lets go, the game's own lid clip finishes the swing",
                        kLidHinges[L.row].object, L.obj, deg, LidOpenFrac * openDeg, ok ? "called" : "*** FAULTED ***", st);
                    L.gameClip = true;
                    L.writing = false;
                    H.lid = -1;
                }
            }
        }
        // ---- a new grab: an item first (it lies in an open container), else a lid ----
        if (H.lid < 0 && !H.item && haveHand && haveSnap) {
            float dl[kMaxPub], di[kMaxPub];
            for (int i = 0; i < s.ni; ++i) {
                di[i] = -1.0f;
                uintptr_t owner = 0;
                if (!ReadPtr(reinterpret_cast<uintptr_t>(s.item[i]) + kItemOwnerOff, &owner) ||
                    !IsVt(reinterpret_cast<void*>(owner), g_vtContainer) || State(reinterpret_cast<void*>(owner)) != kStateOpened) {
                    continue;
                }
                // reachable only while its container's lid is open past the open point (a closed lid hides them, spec §4.5)
                int oi = -1;
                for (int k = 0; k < s.nl && oi < 0; ++k) { if (reinterpret_cast<uintptr_t>(s.lid[k]) == owner) { oi = k; } }
                if (oi >= 0) {
                    LidState tmp{}; tmp.obj = s.lid[oi]; tmp.row = s.row[oi]; tmp.bone = s.bone[oi];
                    float now = 0.0f;
                    if (!LidNowDeg(tmp, &now) || now < LidOpenFrac * kLidHinges[tmp.row].openDeg) { continue; }
                }
                di[i] = looting::BoxDist(s.ibox[i], hw);
            }
            for (int i = 0; i < s.nl; ++i) {
                dl[i] = -1.0f;
                const int st = State(s.lid[i]);
                if (st == kStateLocked || (st != kStateClosed && st != kStateOpened)) { continue; }
                // THE LID ZONE (trace 20261118_2010): within kLidEdgeReachM of the lid's far edge, wherever the lid is now
                if (!haveEdge[i]) { continue; }
                dl[i] = lh::SegDist(edgeA[i], edgeB[i], hw);
            }
            const int bi = lh::NearestWithin(di, s.ni, LootReachM);
            const int bl = bi < 0 ? lh::NearestWithin(dl, s.nl, kLidEdgeReachM) : -1;
            isNear = isNear || bi >= 0 || bl >= 0;
            // THE ENTRY BUZZ (Jay: "Buzz is fine"): a light pulse when this hand comes into a lid's / an item's zone
            const bool zone = bi >= 0 || bl >= 0;
            if (zone && !H.prevZone && RigTwoHandHaptic > 0.0f) {
                PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic * 0.35f, 15);
            }
            H.prevZone = zone;
            if (edge && !g_padOwned[side] && bi >= 0 && body) {
                void* item = s.item[bi];
                uintptr_t owner = 0;
                int home = -1, onElem = -1;
                ReadPtr(reinterpret_cast<uintptr_t>(item) + kItemOwnerOff, &owner);
                const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
                if (SafeParentMeshElem(item, &home) && home >= 0 && SafeElemId(body, side == 0 ? "r_hand" : "l_hand", &onElem) &&
                    onElem >= 0 && HandCarryTakeAs(H.carry, item, reinterpret_cast<void*>(owner), home, body, onElem, ident)) {
                    H.item = item;
                    H.itemOwner = reinterpret_cast<void*>(owner);
                    g_carried[side] = item;
                    InterlockedIncrement(&g_nTake);
                    if (RigTwoHandHaptic > 0.0f) { PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40); }
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] TAKE %s hand: item %p from container %p (its hook element %d) - drawn on %s (element %d) "
                        "until stowed", kSideName[side], item, reinterpret_cast<void*>(owner), home, side == 0 ? "r_hand" : "l_hand",
                        onElem);
                } else {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] REFUSED take of item %p (%s hand): home element %d, hand element %d - not carried",
                        item, kSideName[side], home, onElem);
                }
            } else if (edge && !g_padOwned[side] && bl >= 0) {
                const int k = NewLidSlot(s.lid[bl], s.row[bl], s.bone[bl]);
                float pivotW[3], axisW[3], now = 0.0f;
                if (k >= 0 && HingeWorld(g_lids[k], pivotW, axisW) && LidNowDeg(g_lids[k], &now)) {
                    LidState& L = g_lids[k];
                    H.lid = k;
                    H.lidAtGrab = lh::ClampLid(now, kLidHinges[L.row].openDeg);
                    memcpy(H.grabW, hw, sizeof(H.grabW));
                    L.deg = H.lidAtGrab;
                    L.gameClip = false;
                    L.writing = true;
                    InterlockedIncrement(&g_nGrab);
                    if (RigTwoHandHaptic > 0.0f) { PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40); }
                    const float em[3] = { (edgeA[bl][0] + edgeB[bl][0]) / 2, (edgeA[bl][1] + edgeB[bl][1]) / 2,
                                          (edgeA[bl][2] + edgeB[bl][2]) / 2 };
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] GRAB %s hand: %s %p (state %d) at %.1f deg, hand %.3f m from the lid's edge (zone %.2f) | "
                        "hand %.3f m from the hinge line, the edge %.3f m (equal = on the edge; larger = more hand travel per degree)",
                        kSideName[side], kLidHinges[L.row].object, L.obj, State(L.obj), H.lidAtGrab, dl[bl], kLidEdgeReachM,
                        lh::LineDist(pivotW, axisW, hw), lh::LineDist(pivotW, axisW, em));
                } else {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] REFUSED grab of %p (%s hand): %s", s.lid[bl], kSideName[side],
                        k < 0 ? "no free lid slot" : "the hinge / lid local did not read");
                }
            }
        }
        InterlockedExchange(&g_busy[side], isNear ? 1 : 0);
    }
    // ---- lids released off rest keep their angle (visual only), until the game's clip owns them or they are far ----
    float any[3];
    const bool haveAny = HandWorldPos(0, any) || HandWorldPos(1, any);
    for (int i = 0; i < kMaxLids; ++i) {
        LidState& L = g_lids[i];
        if (!L.obj || g_hand[0].lid == i || g_hand[1].lid == i) { continue; }
        if (!IsVt(L.obj, g_vtContainer)) { L = LidState{}; continue; }
        if (!L.writing || L.gameClip) { continue; }
        float box[6];
        if (haveAny && SafeBox(L.obj, box) && looting::BoxDist(box, any) > kDropDistM) { L.writing = false; continue; }
        WriteLid(L, L.deg);
    }
}

void LidHandPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1)
{
    withholdL1 = withholdR1 = false;
    if (!LidByHand || !vrGrip) { return; }
    float sqL = 0.0f, sqR = 0.0f;
    const bool downL = GetControllerSqueeze(kLeftHand, sqL) && sqL >= kGrip;
    const bool downR = GetControllerSqueeze(kRightHand, sqR) && sqR >= kGrip;
    InterlockedExchange(&g_padOwned[1], downL && leftOwned ? 1 : 0);
    InterlockedExchange(&g_padOwned[0], downR && rightOwned ? 1 : 0);
    withholdL1 = downL && !leftOwned && g_busy[1] != 0;
    withholdR1 = downR && !rightOwned && g_busy[0] != 0;
}

}  // namespace dlvr
