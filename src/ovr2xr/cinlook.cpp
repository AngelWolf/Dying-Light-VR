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

// ===== [DEATHLOOK] - the same runaway during a DEATH (2026-11-19 (41), Jay: "CAMERA SPINNING ON DEATH" - "It spins left/right around
// me"; trace docs/traces/20261119_2115_deathlook.md). Static, OUR gamedll 97046bba (ida/dth1_out.txt, dth2_out.txt; Mac names dth3_mac):
// a death runs DeadController::Start -> ReactionController::StartDeathReaction (Mac). ReactionController's GetILookController (OURS
// +D73610) returns its look interface (this+48) ONLY while its reaction type +304 is 10..19 = EReactionType::IsDeath (Mac +591A90; 10
// DeathDefault .. 19 DeathFromLethalObject); DeadController has none (+84830 = 0). That interface's slot 0 (OURS +D735E0, vtable
// +1730618, COL offset 48) returns PlayerDI +4460 with pitch 0, unconditionally; its rate slots return -1 (no limit) - CinematicController's
// shape before cinlook. The look integrator +BA1350 (the only caller of the selected controller's slot 0, ida/dth5_out.txt:204) puts +4468
// on it and D2 adds the head yaw again: one addend per update = the view spinning left / right. Same removal as LookHook.
constexpr uint32_t kDlColOffset = 48;   // ReactionController's look interface sub-object (dth1: vt +1730618, COL offset 48)
// OURS +D735E0 == EGL +D72A80 (scratchpad sigfind.py, 1 hit in each): push rbx; sub rsp,20h; mov rax,[rcx-30h]; add rcx,-30h;
// mov rbx,rdx; call [rax+20h]; mov dword [rbx+4],0; mov eax,[rax+116Ch] (+4460); mov [rbx],eax
constexpr unsigned char kDlSig[34] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0xd0, 0x48, 0x83, 0xc1, 0xd0, 0x48, 0x8b,
                                       0xda, 0xff, 0x50, 0x20, 0xc7, 0x43, 0x04, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x80, 0x6c, 0x11, 0x00,
                                       0x00, 0x89 };

LookFn             g_dlOrig = nullptr;
bool               g_dlDone = false;
int                g_dlTries = 0;
// Game thread (the player update) writes; camhook's 1 Hz thread prints and resets once no call has come for 1.5 s (a death is over by
// then - a reset racing a new death's first call can at worst merge or drop one line's counts; log only).
volatile long      g_dlCalls = 0, g_dlRemoved = 0, g_dlChaseSkips = 0, g_dlOtherObj = 0, g_dlNotLook = 0, g_dlFaults = 0, g_dlDeaths = 0;
float              g_dlMinRem = 0.0f, g_dlMaxRem = 0.0f;
volatile ULONGLONG g_dlLastMs = 0;

float* __fastcall DeathLookHook(void* self, float* out)
{
    float* r = g_dlOrig(self, out);   // the reaction's own wanted look - outside any __try
    const ULONGLONG now = GetTickCount64();
    if (g_dlLastMs == 0 || now - g_dlLastMs > 1000) {
        ++g_dlDeaths;
        // [PROBE: DEATHLOOK] gate=none (always on)  site=HEADSET  log-only=no
        LOG("cinlook: [DEATHLOOK] death #%ld first wanted-look call (ReactionController owns the look)", g_dlDeaths);
    }
    g_dlLastMs = now;
    ++g_dlCalls;
    if (!r) { return r; }
    uintptr_t owner = 0;
    float look = 0.0f;
    bool ok = false;
    __try {
        const uintptr_t ctrl = reinterpret_cast<uintptr_t>(self) - kDlColOffset;   // as +D735E0 itself: [rcx-30h], vfunc +20h
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ctrl);
        const OwnerFn f = *reinterpret_cast<const OwnerFn*>(vt + 32);
        owner = f(ctrl);
        if (owner) {
            look = *reinterpret_cast<const float*>(owner + kLook);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) {
        ++g_dlFaults;
        return r;
    }
    float added = 0.0f;
    uintptr_t addedObj = 0;
    bool inChase = false;
    CamYawAddendNow(&added, &addedObj, &inChase);
    if (memcmp(&r[0], &look, sizeof(float)) != 0) {   // not +4460 itself - left exactly as the game returned it
        ++g_dlNotLook;
        return r;
    }
    const float before = r[0];
    r[0] = cinyaw::RemoveAddend(r[0], look, added, addedObj == owner, inChase);
    if (r[0] != before) {
        const float d = before - r[0];
        if (g_dlRemoved == 0 || d < g_dlMinRem) { g_dlMinRem = d; }
        if (g_dlRemoved == 0 || d > g_dlMaxRem) { g_dlMaxRem = d; }
        ++g_dlRemoved;
    } else if (inChase) {
        ++g_dlChaseSkips;
    } else if (addedObj != owner) {
        ++g_dlOtherObj;
    }
    return r;
}

void DeathLookInstall()
{
    if (g_dlDone) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    ++g_dlTries;
    int stage = 0;
    const uintptr_t vt = RttiFindVtable(gd, ".?AVReactionController@@", kDlColOffset, &stage);
    if (!vt) {
        g_dlDone = g_dlTries >= kMaxTries;
        static const char* const kStage[] = { "the class name .?AVReactionController@@ is not in the module",
                                              "no object locator names it with offset 48 (the look interface)",
                                              "no vtable points at that locator" };
        // [PROBE: DEATHLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [DEATHLOOK] REFUSED (try %d) - %s. Nothing written; a death's look keeps feeding the head yaw back (the view "
            "spins).%s", g_dlTries, stage < 0 ? "the RTTI scan faulted" : kStage[stage > 2 ? 2 : stage], g_dlDone ? "" : " Retried next second.");
        return;
    }
    const uintptr_t slot = vt + 8 * kSlot;
    const uintptr_t have = ReadP(slot);
    if (have == reinterpret_cast<uintptr_t>(&DeathLookHook)) { g_dlDone = true; return; }
    bool sigOk = false;
    __try { sigOk = have && memcmp(reinterpret_cast<const void*>(have), kDlSig, sizeof(kDlSig)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!sigOk) {
        g_dlDone = true;
        // [PROBE: DEATHLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [DEATHLOOK] REFUSED - ReactionController look slot 0 gamedll+0x%zX holds gamedll+0x%zX, whose first 34 bytes are "
            "not the wanted-look function both known builds share (OURS +D735E0, EGL +D72A80). Nothing written.",
            static_cast<size_t>(slot - gd), static_cast<size_t>(have - gd));
        return;
    }
    g_dlOrig = reinterpret_cast<LookFn>(have);
    DWORD old = 0;
    // [LOADBEARING: deathlook vtable slot] ReactionController look interface (RTTI COL offset 48) slot 0 -> DeathLookHook. OURS vtable
    // +1730618 slot 0 +D735E0, EGL +D72A80. Without it a death integrates design D's head yaw into the look every update - the view spins.
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        g_dlDone = true;
        // [PROBE: DEATHLOOK] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinlook: [DEATHLOOK] REFUSED - VirtualProtect on the slot failed (error %lu). Nothing written.", GetLastError());
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&DeathLookHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    g_dlDone = true;
    const bool ok = ReadP(slot) == reinterpret_cast<uintptr_t>(&DeathLookHook);
    // [PROBE: DEATHLOOK] gate=none (always on)  site=FLAT  log-only=yes
    LOG("cinlook: [DEATHLOOK] ARMED (logged unconditionally) - ReactionController look slot 0 gamedll+0x%zX swapped (vtable +0x%zX, was "
        "gamedll+0x%zX, read back %s). A death's wanted look no longer hands design D's head yaw back to the look.",
        static_cast<size_t>(slot - gd), static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd), ok ? "OK" : "*** NOT OUR FUNCTION ***");
}

// camhook's 1 Hz thread: a death's totals once its calls have stopped for 1.5 s. COST: two loads and a compare a second.
void DeathLookTick()
{
    if (g_dlCalls == 0) { return; }
    const ULONGLONG last = g_dlLastMs;
    if (GetTickCount64() - last <= 1500) { return; }
    // [PROBE: DEATHLOOK] gate=none (always on)  site=HEADSET  log-only=yes
    LOG("cinlook: [DEATHLOOK] death #%ld ended - wanted-look calls %ld, head yaw taken out %ld (min %+.2f max %+.2f deg), left alone: "
        "not +4460 %ld, inside the engine's chase %ld, another object %ld, faults %ld", g_dlDeaths, g_dlCalls, g_dlRemoved, g_dlMinRem,
        g_dlMaxRem, g_dlNotLook, g_dlChaseSkips, g_dlOtherObj, g_dlFaults);
    g_dlCalls = g_dlRemoved = g_dlChaseSkips = g_dlOtherObj = g_dlNotLook = g_dlFaults = 0;
    g_dlMinRem = g_dlMaxRem = 0.0f;
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
    // [DEATHLOOK] (trace 20261119_2115_deathlook): ReactionController's death look - its own done flag, so it runs before the cutscene
    // install's early return; then a finished death's totals. Both no-ops once armed / with no death.
    DeathLookInstall();
    DeathLookTick();
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
