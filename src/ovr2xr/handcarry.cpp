// handcarry.cpp - see handcarry.h. The engine side of the hand-carried model (INV-613, OUR engine e9fbfc2c):
//   IModelObject::AttachChildToMeshElement +143DE0 (child IControlObject*, element, keepWorld) - first takes the child off its
//     current parent; keepWorld 0 keeps the child's LOCAL, so it rides element x local with no per-frame write;
//   IControlObject::SetLocalXform +13C890 / GetLocalXform +13C860 / GetParentMeshElement +13E940.
// Every call is inside __try: a fault forgets the carry (never a second call on that pointer) and says so by name.
#include "handcarry.h"

#include "boneblend.h"      // BowItemElemWorld - the rig's world accessor (the self-check's drawn origin)
#include "handcarry_logic.h"  // ComposeLocal

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

typedef void (*AttachFn)(void* model, void* childCo, int elem, bool keepWorld);
typedef void (*SetLocalFn)(void* co, const float* m);
typedef const float* (*GetLocalFn)(void* co);
typedef int (*GetParentElemFn)(void* co);

AttachFn        g_attach = nullptr;
SetLocalFn      g_setLocal = nullptr;
GetLocalFn      g_getLocal = nullptr;
GetParentElemFn g_parentElem = nullptr;

// The world carry's exports (trace 20261120_2210) - NOT in HandCarryReady's refusal list; null = the world functions refuse, named once.
typedef void (*DetachFn)(void* co, bool keepWorld);           // ?DetachFromParent@IControlObject@@QEAAX_N@Z  +13D920
typedef void (*SetWorldXformFn)(void* co, const float* m);    // ?SetWorldXform@IControlObject@@QEAAXAEBVmtx34@@@Z  +13C8C0
typedef const float* (*GetWorldXformFn)(void* co);            // ?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ
typedef int (*GetIdFn)(void* gso);                            // ?GetID@IGSObject@@QEBAHXZ  +252080
typedef void* (*FindByIdFn)(void* gso, int id);               // ?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z  +251DF0
DetachFn        g_detach = nullptr;
SetWorldXformFn g_setWorldX = nullptr;
GetWorldXformFn g_getWorldX = nullptr;
GetIdFn         g_gsoId = nullptr;
FindByIdFn      g_findById = nullptr;
constexpr size_t kGsoOff = 40;   // obj + 40 = IGSObject (bodygrab.cpp kGsoOff; DroppedItem / ItemSpawner / ContainerItem / PlayerDI alike)

bool WorldReady()
{
    return g_detach && g_setWorldX && g_getWorldX && g_gsoId && g_findById && g_attach && g_setLocal && g_parentElem;
}

void* Co(void* obj) { return obj ? static_cast<char*>(obj) + 24 : nullptr; }   // INV-402: the IControlObject is at +24

bool CallGetLocal(void* obj, float out[12])
{
    __try {
        const float* m = g_getLocal(Co(obj));
        if (!m) { return false; }
        std::memcpy(out, m, 12 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetLocal(void* obj, const float m[12])
{
    __try { g_setLocal(Co(obj), m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallAttach(void* model, void* obj, int elem)
{
    __try { g_attach(model, Co(obj), elem, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallParentElem(void* obj, int* out)
{
    __try { *out = g_parentElem(Co(obj)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Faulted(HandCarry& c, const char* what)
{
    // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
    LOG("handcarry: [BOWNOCK] HANDCARRY FAULT in %s on object %p - the carry is dropped and the object is not touched again.", what,
        c.obj);
    c.obj = nullptr;
    c.active = false;
    ++c.forgets;
}

}  // namespace

bool HandCarryReady()
{
    static int s_state = 0;   // 0 untried, 1 ready, -1 refused
    if (s_state != 0) { return s_state > 0; }
    HMODULE eng = GetModuleHandleA("engine_x64_rwdi.dll");
    const char* missing = nullptr;
    if (!eng) {
        missing = "engine_x64_rwdi.dll (not mapped)";
    } else {
        g_attach = reinterpret_cast<AttachFn>(
            GetProcAddress(eng, "?AttachChildToMeshElement@IModelObject@@QEAAXPEAVIControlObject@@H_N@Z"));
        g_setLocal = reinterpret_cast<SetLocalFn>(GetProcAddress(eng, "?SetLocalXform@IControlObject@@QEAAXAEBVmtx34@@@Z"));
        g_getLocal = reinterpret_cast<GetLocalFn>(GetProcAddress(eng, "?GetLocalXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
        g_parentElem = reinterpret_cast<GetParentElemFn>(GetProcAddress(eng, "?GetParentMeshElement@IControlObject@@QEBAHXZ"));
        g_detach = reinterpret_cast<DetachFn>(GetProcAddress(eng, "?DetachFromParent@IControlObject@@QEAAX_N@Z"));
        g_setWorldX = reinterpret_cast<SetWorldXformFn>(GetProcAddress(eng, "?SetWorldXform@IControlObject@@QEAAXAEBVmtx34@@@Z"));
        g_getWorldX = reinterpret_cast<GetWorldXformFn>(GetProcAddress(eng, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
        g_gsoId = reinterpret_cast<GetIdFn>(GetProcAddress(eng, "?GetID@IGSObject@@QEBAHXZ"));
        g_findById = reinterpret_cast<FindByIdFn>(GetProcAddress(eng, "?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z"));
        if (!g_attach) { missing = "IModelObject::AttachChildToMeshElement"; }
        else if (!g_setLocal) { missing = "IControlObject::SetLocalXform"; }
        else if (!g_getLocal) { missing = "IControlObject::GetLocalXform"; }
        else if (!g_parentElem) { missing = "IControlObject::GetParentMeshElement"; }
    }
    if (missing) {
        s_state = -1;
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("handcarry: [BOWNOCK] HANDCARRY REFUSED - %s not found in the engine; the carried arrow is not put in the hand this "
            "session (a different game build?).", missing);
        return false;
    }
    s_state = 1;
    const uintptr_t base = reinterpret_cast<uintptr_t>(eng);
    // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
    LOG("handcarry: [BOWNOCK] HANDCARRY READY (logged unconditionally) - AttachChildToMeshElement +%zX, SetLocalXform +%zX, "
        "GetLocalXform +%zX, GetParentMeshElement +%zX (INV-613: +143DE0 / +13C890 / +13C860 / +13E940).",
        static_cast<size_t>(reinterpret_cast<uintptr_t>(g_attach) - base), static_cast<size_t>(reinterpret_cast<uintptr_t>(g_setLocal) - base),
        static_cast<size_t>(reinterpret_cast<uintptr_t>(g_getLocal) - base),
        static_cast<size_t>(reinterpret_cast<uintptr_t>(g_parentElem) - base));
    // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
    LOG("handcarry: [LIDHAND] WORLD CARRY %s (logged unconditionally) - DetachFromParent +%zX (want +13D920), SetWorldXform +%zX (want "
        "+13C8C0), GetWorldXform %s, GetID +%zX (want +252080), FindObjectByID +%zX (want +251DF0)%s",
        WorldReady() ? "READY" : "*** OFF ***",
        g_detach ? static_cast<size_t>(reinterpret_cast<uintptr_t>(g_detach) - base) : 0,
        g_setWorldX ? static_cast<size_t>(reinterpret_cast<uintptr_t>(g_setWorldX) - base) : 0, g_getWorldX ? "found" : "MISSING",
        g_gsoId ? static_cast<size_t>(reinterpret_cast<uintptr_t>(g_gsoId) - base) : 0,
        g_findById ? static_cast<size_t>(reinterpret_cast<uintptr_t>(g_findById) - base) : 0,
        WorldReady() ? "" : " - a world export is missing: placed / dropped items are not taken by hand (the element carry is unaffected)");
    return true;
}

namespace {

// The one take path behind HandCarryTake and HandCarryTakeAs. `drawn` null = HandCarryTake (rel as given, unchanged behaviour for the
// bow); `drawn` set = HandCarryTakeAs (rel = drawn x L^-1 from the local L read here, handcarry::RelForDrawn). COST: 4 export calls
// plus one 3x3 inverse when `drawn` is set - once per take (an event), never per frame.
bool TakeCore(HandCarry& c, void* obj, void* homeModel, int homeElem, void* onModel, int onElem, const float relIn[12],
              const float drawn[12], float d)
{
    if (!HandCarryReady() || !obj || !homeModel || !onModel || homeElem < 0 || onElem < 0) { return false; }
    int parent = -2;
    if (!CallParentElem(obj, &parent)) { c.obj = obj; Faulted(c, "the take's parent check"); return false; }
    if (parent != homeElem) {
        static long s_refused = 0;
        if (s_refused++ == 0) {
            // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
            LOG("handcarry: [BOWNOCK] TAKE REFUSED (first; counted) - object %p hangs on element %d, not %d: it is not the object "
                "the bow attached there (a different build's layout?). Not carried.", obj, parent, homeElem);
        }
        return false;
    }
    float found[12];
    if (!CallGetLocal(obj, found)) { c.obj = obj; Faulted(c, "GetLocalXform"); return false; }
    float rel[12];
    if (drawn) {
        if (!handcarry::RelForDrawn(drawn, found, rel)) {
            static long s_singular = 0;
            if (s_singular++ == 0) {
                // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
                LOG("handcarry: [BOWNOCK] TAKE-AS REFUSED (first; counted) - object %p on element %d: its own local is singular "
                    "(determinant below 1e-12), so it cannot be drawn at the hand's frame. Not carried, nothing changed | local "
                    "(%+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f).", obj, homeElem, found[0], found[1], found[2],
                    found[4], found[5], found[6], found[8], found[9], found[10]);
            }
            return false;
        }
    } else {
        std::memcpy(rel, relIn, sizeof(rel));
    }
    float local[12];
    handcarry::ComposeLocal(rel, found, local);
    if (!CallAttach(onModel, obj, onElem)) { c.obj = obj; Faulted(c, "the take's attach"); return false; }
    if (!CallSetLocal(obj, local)) { c.obj = obj; Faulted(c, "the take's SetLocalXform"); return false; }
    std::memcpy(c.homeLocal, found, sizeof(found));
    c.obj = obj;
    c.homeModel = homeModel;
    c.homeElem = homeElem;
    c.onModel = onModel;
    c.onElem = onElem;
    std::memcpy(c.appliedRel, rel, sizeof(c.appliedRel));
    c.active = true;
    ++c.takes;
    int now = -2;
    CallParentElem(obj, &now);
    // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
    LOG("handcarry: [BOWNOCK] TAKE (carried) #%ld - object %p from element %d onto element %d (read back %d), back %.3f m | its own "
        "local as found: (%+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f | %+.4f %+.4f %+.4f %+.4f).", c.takes, obj, homeElem,
        onElem, now, d, found[0], found[1], found[2], found[3], found[4], found[5], found[6], found[7], found[8], found[9], found[10],
        found[11]);
    return true;
}

}  // namespace

bool HandCarryTake(HandCarry& c, void* obj, void* homeModel, int homeElem, void* onModel, int onElem, const float rel[12], float d)
{
    if (!rel) { return false; }
    return TakeCore(c, obj, homeModel, homeElem, onModel, onElem, rel, nullptr, d);
}

bool HandCarryTakeAs(HandCarry& c, void* obj, void* homeModel, int homeElem, void* onModel, int onElem, const float drawn[12])
{
    if (!drawn) { return false; }
    return TakeCore(c, obj, homeModel, homeElem, onModel, onElem, nullptr, drawn, 0.0f);
}

bool HandCarryStillOn(const HandCarry& c)
{
    if (!c.active || !c.obj || !g_parentElem) { return false; }
    int parent = -2;
    return CallParentElem(c.obj, &parent) && parent == c.onElem;
}

bool HandCarryReattach(HandCarry& c, const float rel[12])
{
    if (!c.active || !c.obj) { return false; }
    float local[12];
    handcarry::ComposeLocal(rel, c.homeLocal, local);
    if (!CallAttach(c.onModel, c.obj, c.onElem) || !CallSetLocal(c.obj, local)) { Faulted(c, "the re-take"); return false; }
    if (c.retakes++ == 0) {
        // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
        LOG("handcarry: [BOWNOCK] TAKEN BACK BY THE GAME (first; counted in the totals) - object %p was moved off element %d "
            "mid-carry (an arrow-type or mesh change, INV-613 F5); attached back.", c.obj, c.onElem);
    }
    return true;
}

void HandCarrySetLocal(HandCarry& c, const float rel[12])
{
    if (!c.active || !c.obj) { return; }
    float local[12];
    handcarry::ComposeLocal(rel, c.homeLocal, local);
    if (!CallSetLocal(c.obj, local)) { Faulted(c, "SetLocalXform (a slider moved)"); return; }
    std::memcpy(c.appliedRel, rel, sizeof(c.appliedRel));
}

bool HandCarryRelChanged(const HandCarry& c, const float rel[12])
{
    return std::memcmp(c.appliedRel, rel, sizeof(c.appliedRel)) != 0;
}

bool HandCarryGiveBack(HandCarry& c, const char* why)
{
    if (!c.active || !c.obj) { return false; }
    if (!CallSetLocal(c.obj, c.homeLocal) || !CallAttach(c.homeModel, c.obj, c.homeElem)) { Faulted(c, "the give-back"); return false; }
    int parent = -2;
    CallParentElem(c.obj, &parent);
    c.active = false;
    ++c.giveBacks;
    // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
    LOG("handcarry: [BOWNOCK] GIVEBACK #%ld (%s) - object %p back on element %d (read back %d%s), its own local restored.",
        c.giveBacks, why, c.obj, c.homeElem, parent, parent == c.homeElem ? "" : " *** NOT HOME ***");
    return parent == c.homeElem;
}

void HandCarryForget(HandCarry& c, const char* why)
{
    if (!c.obj && !c.active) { return; }
    ++c.forgets;
    // [PROBE: BOWNOCK] gate=bow_manual_nock  site=HEADSET  log-only=no
    LOG("handcarry: [BOWNOCK] OBJECT CHANGED / GONE #%ld (%s) - object %p forgotten without a call on it (the bow owns and destroys "
        "its arrow, INV-613 F7).", c.forgets, why, c.obj);
    c.obj = nullptr;
    c.active = false;
}

bool HandCarryObjectOrigin(const HandCarry& c, float out[3])
{
    float m[12];
    if (!c.active || !c.obj || !BowItemElemWorld(reinterpret_cast<uintptr_t>(c.obj), 0, m)) { return false; }
    out[0] = m[3];
    out[1] = m[7];
    out[2] = m[11];
    return true;
}

// ---- the WORLD carry (trace 20261120_2210) ----
namespace {

bool CallGetWorld(void* obj, float out[12])
{
    __try {
        const float* m = g_getWorldX(Co(obj));
        if (!m) { return false; }
        std::memcpy(out, m, 12 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetWorld(void* obj, const float m[12])
{
    __try { g_setWorldX(Co(obj), m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallDetach(void* obj)
{
    __try { g_detach(Co(obj), true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallGsoId(void* obj, int* out)
{
    __try { *out = g_gsoId(static_cast<char*>(obj) + kGsoOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is `obj` still the live object the engine returns for `id`? Asked through the PLAYER's IGSObject (bodygrab.cpp StillInEngine, INV-663):
// no read of obj's own memory, which may already be freed.
bool CallStillLive(void* player, void* obj, int id)
{
    void* found = nullptr;
    __try { found = g_findById(static_cast<char*>(player) + kGsoOff, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return found == static_cast<char*>(obj) + kGsoOff;
}

void SayWorldRefused(const char* why, void* obj, int detail)
{
    static const char* s_said[8] = {};
    for (const char* p : s_said) { if (p == why) { return; } }
    for (const char*& p : s_said) { if (!p) { p = why; break; } }
    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
    LOG("handcarry: [LIDHAND] WORLD TAKE REFUSED (first for this reason) - object %p: %s (%d). Not carried, nothing changed.", obj, why,
        detail);
}

}  // namespace

bool HandCarryTakeWorld(HandCarry& c, void* obj, void* onModel, int onElem, const float drawn[12])
{
    if (!HandCarryReady() || !WorldReady() || !obj || !onModel || onElem < 0 || !drawn) {
        SayWorldRefused("a world export is missing or a null argument (see WORLD CARRY at READY)", obj, onElem);
        return false;
    }
    int parent = -2;
    if (!CallParentElem(obj, &parent)) { c.obj = obj; Faulted(c, "the world take's parent check"); return false; }
    if (parent >= 0) { SayWorldRefused("it hangs off a mesh element (a stuck / attached item - not read, spec 5.2)", obj, parent); return false; }
    float home[12];
    if (!CallGetWorld(obj, home)) { c.obj = obj; Faulted(c, "GetWorldXform (the world take)"); return false; }
    int id = -1;
    const bool idOk = CallGsoId(obj, &id);
    if (!CallAttach(onModel, obj, onElem)) { c.obj = obj; Faulted(c, "the world take's attach"); return false; }
    if (!CallSetLocal(obj, drawn)) { c.obj = obj; Faulted(c, "the world take's SetLocalXform"); return false; }
    std::memcpy(c.homeWorld, home, sizeof(home));
    std::memcpy(c.appliedRel, drawn, sizeof(c.appliedRel));
    c.obj = obj;
    c.homeModel = nullptr;
    c.homeElem = -1;
    c.onModel = onModel;
    c.onElem = onElem;
    c.world = true;
    c.gsoId = id;
    c.idOk = idOk;
    c.active = true;
    ++c.takes;
    return true;
}

bool HandCarryReleaseWorld(HandCarry& c, const float* world, const char* why)
{
    if (!c.active || !c.obj || !c.world || !WorldReady()) { return false; }
    if (!CallDetach(c.obj)) { Faulted(c, "DetachFromParent (the world release)"); return false; }
    if (world && !CallSetWorld(c.obj, world)) { Faulted(c, "SetWorldXform (the world release)"); return false; }
    c.active = false;
    c.world = false;
    ++c.giveBacks;
    (void)why;
    return true;
}

bool HandCarryObjectGone(const HandCarry& c, void* player)
{
    if (!c.active || !c.obj) { return true; }
    if (c.idOk && player && g_findById && !CallStillLive(player, c.obj, c.gsoId)) { return true; }
    return !HandCarryStillOn(c);
}

}  // namespace dlvr
