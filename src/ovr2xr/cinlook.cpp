// cinlook - a cutscene's wanted look no longer feeds design D's head yaw back into the body. 2026-10-25.
//
// WHY (INV-620; spec docs/superpowers/specs/2026-10-25-cutscene-camera-design.md (a) + ADDENDUM 1 R3): under design D, camyaw::Hook
// (boneblend.cpp, D2) adds the head yaw into +4460 after the engine's chase. While a cutscene owns the look, CinematicController's look
// slot 0 (OURS +CF7A40, EGL +CF6EE0) hands +4460 back as the wanted look whenever the movie has no actor look of its own; the look
// integrator +BA1350 puts +4468 on it (no rate limit) and D2 adds the yaw again - one addend per update. Measured (cutscene_yawstep_run1):
// +4468 / +4588 drifting 28-66 deg/s with the head still and 1,000-7,500 deg/s with it turned. Here the slot is swapped: the original runs,
// then - only when it returned +4460 itself (bit for bit) for the object D2 last wrote, and not from inside D2's own chase (+BA1110 calls
// the selector inside camyaw::Hook, where the addend is already out) - D2's addend is taken out of the returned yaw. The movie's own look
// deviation is passed through. Same shape as head_yaw_look_enable_fix (camyaw::lookenable, INV-329).
// The vtable is found by RTTI name with the COL offset 32 (the look interface) and accepted only if slot 0's first 24 bytes are the
// function both known builds share - no rva table.
// COST: install - one RTTI scan (once) + one 8-byte write. Per call (only while a cutscene owns the look; +BA1350 / +BA1FD0 / +BA2500 /
// +D40650 per player update) - the original, one virtual call (the controller's owner, as +CF7A40 itself makes), three loads, a compare.
#include "cinlook.h"

#include "boneblend.h"    // CamYawAddendNow - D2's addend, its object, in-chase
#include "cinyaw_logic.h"
#include "rtti_vtable.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

constexpr uint32_t kColOffset = 32;   // CinematicController's look interface sub-object (cc3: vt +172BB28, COL offset 32)
constexpr size_t   kSlot = 0;         // the wanted look (yaw, pitch) - +BA1350 :100-104 (INV-258)
constexpr size_t   kLook = 4460;      // PlayerDI +4460, the look D2 writes
// The slot's own bytes - OURS +CF7A40 == EGL +CF6EE0 (read 2026-10-25 (3), scratchpad sigs.py, unique in OURS): push rbx; sub rsp,20h;
// mov rbx,rdx; mov rdx,[rcx+28h]; test rdx,rdx; jz; mov eax,[rdx+1C4h]; dec eax ...
constexpr unsigned char kSig[24] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0x51,
                                     0x28, 0x48, 0x85, 0xd2, 0x74, 0x3b, 0x8b, 0x82, 0xc4, 0x01, 0x00, 0x00 };

using LookFn = float*(__fastcall*)(void* self, float* out);
using OwnerFn = uintptr_t(__fastcall*)(uintptr_t ctrl);
LookFn          g_orig = nullptr;
bool            g_done = false;
volatile LONG   g_armed = 0;
uintptr_t       g_vtRva = 0;
int             g_tries = 0;
constexpr int   kMaxTries = 30;

// One cutscene = calls with no gap over 1 s. Game thread only (the player update).
long      g_calls = 0, g_branch = 0, g_removed = 0, g_chaseSkips = 0, g_otherObj = 0, g_faults = 0, g_cutscenes = 0;
float     g_minRem = 0.0f, g_maxRem = 0.0f;
ULONGLONG g_lastMs = 0;

uintptr_t ReadP(uintptr_t a)
{
    __try { return *reinterpret_cast<const volatile uintptr_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The controller's owner (PlayerDI) and its +4460, exactly as +CF7A40 :23-28 reaches them. False on a fault or no owner.
bool OwnerLook(void* self, uintptr_t* owner, float* look)
{
    __try {
        const uintptr_t ctrl = reinterpret_cast<uintptr_t>(self) - kColOffset;
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
        const OwnerFn f = *reinterpret_cast<const OwnerFn*>(vt + 32);
        const uintptr_t o = f(ctrl);
        if (!o) { return false; }
        *owner = o;
        *look = *reinterpret_cast<const float*>(o + kLook);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EndCutsceneLine()
{
    // [PROBE: CINLOOK] gate=none (always on)  site=HEADSET  log-only=yes
    LOG("cinlook: [CINLOOK] cutscene #%ld ended - wanted-look calls %ld, of them returned +4460 %ld, head yaw taken out %ld (min %+.2f "
        "max %+.2f deg), left alone: inside the engine's chase %ld, another object %ld, faults %ld", g_cutscenes, g_calls, g_branch,
        g_removed, g_minRem, g_maxRem, g_chaseSkips, g_otherObj, g_faults);
    g_calls = g_branch = g_removed = g_chaseSkips = g_otherObj = g_faults = 0;
    g_minRem = g_maxRem = 0.0f;
}

float* __fastcall LookHook(void* self, float* out)
{
    float* r = g_orig(self, out);   // the cutscene's own wanted look - outside any __try
    const ULONGLONG now = GetTickCount64();
    if (g_lastMs == 0 || now - g_lastMs > 1000) {
        if (g_calls != 0) { EndCutsceneLine(); }   // the hand-back did not report it (CinLookReport)
        ++g_cutscenes;
        // [PROBE: CINLOOK] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("cinlook: [CINLOOK] cutscene #%ld first wanted-look call", g_cutscenes);
    }
    g_lastMs = now;
    ++g_calls;
    if (!r) { return r; }
    uintptr_t owner = 0;
    float look = 0.0f;
    if (!OwnerLook(self, &owner, &look)) {
        ++g_faults;
        return r;
    }
    float added = 0.0f;
    uintptr_t addedObj = 0;
    bool inChase = false;
    CamYawAddendNow(&added, &addedObj, &inChase);
    if (memcmp(&r[0], &look, sizeof(float)) != 0) { return r; }   // the movie's own look deviation
    ++g_branch;
    const float before = r[0];
    r[0] = cinyaw::RemoveAddend(r[0], look, added, addedObj == owner, inChase);
    if (r[0] != before) {
        const float d = before - r[0];
        if (g_removed == 0 || d < g_minRem) { g_minRem = d; }
        if (g_removed == 0 || d > g_maxRem) { g_maxRem = d; }
        ++g_removed;
    } else if (inChase) {
        ++g_chaseSkips;
    } else if (addedObj != owner) {
        ++g_otherObj;
    }
    return r;
}

}  // namespace

void CinLookReport()
{
    if (g_calls != 0) { EndCutsceneLine(); }
}

bool CinLookArmed(uintptr_t* vtRva)
{
    const bool a = g_armed != 0;
    if (vtRva) { *vtRva = a ? g_vtRva : 0; }
    return a;
}

void CinLookInstall()
{
    if (g_done) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    ++g_tries;
    int stage = 0;
    const uintptr_t vt = RttiFindVtable(gd, ".?AVCinematicController@@", kColOffset, &stage);
    if (!vt) {
        g_done = g_tries >= kMaxTries;
        static const char* const kStage[] = { "the class name .?AVCinematicController@@ is not in the module",
                                              "no object locator names it with offset 32 (the look interface)",
                                              "no vtable points at that locator" };
        // [PROBE: CINLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [CINLOOK] REFUSED (try %d) - %s. Nothing written; a cutscene's look keeps feeding the head yaw back.%s", g_tries,
            stage < 0 ? "the RTTI scan faulted" : kStage[stage > 2 ? 2 : stage], g_done ? "" : " Retried next second.");
        return;
    }
    const uintptr_t slot = vt + 8 * kSlot;
    const uintptr_t have = ReadP(slot);
    if (have == reinterpret_cast<uintptr_t>(&LookHook)) { g_done = true; return; }
    bool sigOk = false;
    __try { sigOk = have && memcmp(reinterpret_cast<const void*>(have), kSig, sizeof(kSig)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!sigOk) {
        g_done = true;
        // [PROBE: CINLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [CINLOOK] REFUSED - CinematicController look slot 0 gamedll+0x%zX holds gamedll+0x%zX, whose first 24 bytes are "
            "not the wanted-look function both known builds share. Nothing written.", static_cast<size_t>(slot - gd),
            static_cast<size_t>(have - gd));
        return;
    }
    g_orig = reinterpret_cast<LookFn>(have);
    DWORD old = 0;
    // [LOADBEARING: cinlook vtable slot] CinematicController look interface (RTTI COL offset 32) slot 0 -> LookHook. OURS vtable
    // +172BB28 slot 0 +CF7A40, EGL +CF6EE0. Without it a cutscene integrates design D's head yaw into the body every update (INV-620).
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        g_done = true;
        // [PROBE: CINLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [CINLOOK] REFUSED - VirtualProtect on the slot failed (error %lu). Nothing written.", GetLastError());
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&LookHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    g_done = true;
    const bool ok = ReadP(slot) == reinterpret_cast<uintptr_t>(&LookHook);
    g_vtRva = vt - gd;
    InterlockedExchange(&g_armed, ok ? 1 : 0);
    // [PROBE: CINLOOK] gate=none (always on)  site=FLAT  log-only=yes
    LOG("cinlook: [CINLOOK] ARMED (logged unconditionally) - CinematicController look slot 0 gamedll+0x%zX swapped (vtable +0x%zX, was "
        "gamedll+0x%zX, read back %s). A cutscene's wanted look no longer hands design D's head yaw back to the body.",
        static_cast<size_t>(slot - gd), static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd), ok ? "OK" : "*** NOT OUR FUNCTION ***");
}

}  // namespace dlvr
