// cinbars - no cutscene side bars in VR. 2026-10-25.
//
// WHY (INV-620 (b); spec docs/superpowers/specs/2026-10-25-cutscene-camera-design.md + ADDENDUM 1 R1): besides INV-609's BeltUp /
// BeltDown (Data3), a cinematic shows two 30 x 720 black images, I_HorzBar0 / I_HorzBar1, built in code from XUI text inside the
// gamedll (+1231380, stored at this+544 / this+552). UIGameMovieBackground slot 114 (OURS +1233020, EGL +12326C0) runs each update:
// target = byte this+572 ? 1 : 0 -> a spring at this+560 -> SetVisible(both bars, this+560 > 0.005) and slides them in from the edges.
// Here, in a VR session, the slot runs with this+572 = 0 for the call and the game's byte is restored after - the spring stays at 0,
// the bars never show, and nothing else sees a changed flag. Flat: passed through (Jay on the letterbox: "Ideally just VR").
// VR SESSION = HeadYawDecoupleActive(): it latches only in ovr_GetTrackingState, which the game calls only in a VR session, and
// head_yaw_decouple is S_BOOL_LOCKED on - so it is ON in every VR session and OFF in flat.
// The vtables (UIGameMovieBackground and MenuGameMovie - both hold this function) are found by RTTI name (COL offset 0) and accepted
// only if slot 114's first 24 bytes are the function both known builds share (they include `cmp byte ptr [rcx+23Ch], 0` - the +572 read) - no rva table.
// COST: install - one RTTI scan (once) + one 8-byte write. Per call (the movie background's update) - one load of a latched int,
// one byte save / store / restore around the original.
#include "cinbars.h"

#include "headpose.h"     // HeadYawDecoupleActive - the VR-session test (see above)
#include "rtti_vtable.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

constexpr uint32_t kColOffset = 0;
constexpr size_t   kSlot = 114;
constexpr size_t   kShow = 572;   // byte: show the side bars (Mac +556; +16 like the bar fields)
// OURS +1233020 == EGL +12326C0 (scratchpad sigs.py, unique in OURS at 24 bytes): mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi;
// sub rsp,50h; cmp byte ptr [rcx+23Ch],0; mov rbx,rcx ...
constexpr unsigned char kSig[24] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                     0x83, 0xec, 0x50, 0x80, 0xb9, 0x3c, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8b };

using UpdFn = void(__fastcall*)(void* self);
UpdFn         g_orig = nullptr;
bool          g_done = false;
int           g_tries = 0;
constexpr int kMaxTries = 30;
volatile LONG g_saidHidden = 0, g_saidFlat = 0;

void __fastcall UpdHook(void* self)
{
    unsigned char* show = static_cast<unsigned char*>(self) + kShow;
    if (!HeadYawDecoupleActive()) {
        if (*show && InterlockedExchange(&g_saidFlat, 1) == 0) {
            // [PROBE: CINBARS] gate=none (always on)  site=FLAT  log-only=yes
            LOG("cinbars: [CINBARS] FIRST side bars in a flat session - left as the game draws them");
        }
        g_orig(self);
        return;
    }
    const unsigned char was = *show;
    if (was && InterlockedExchange(&g_saidHidden, 1) == 0) {
        // [PROBE: CINBARS] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("cinbars: [CINBARS] FIRST side bars asked for in VR - kept hidden (logged once)");
    }
    *show = 0;
    g_orig(self);
    *show = was;
}

// cc12 / cc13: TWO vtables hold the same slot-114 function - UIGameMovieBackground (+18A11D8) and its scene class MenuGameMovie
// (+15D8E58, INV-609's "scene class MenuGameMovie"). The live object can be either, so both are swapped; each is found and checked alone.
constexpr const char* kClasses[2] = { ".?AVUIGameMovieBackground@@", ".?AVMenuGameMovie@@" };
bool g_classDone[2] = { false, false };

// One class: 1 = swapped (or already ours), 0 = not found yet (retry), -1 = refused for good. Logs its own outcome.
int SwapOne(uintptr_t gd, int i)
{
    int stage = 0;
    const uintptr_t vt = RttiFindVtable(gd, kClasses[i], kColOffset, &stage);
    if (!vt) {
        static const char* const kStage[] = { "the class name is not in the module", "no object locator names it with offset 0",
                                              "no vtable points at that locator" };
        // [PROBE: CINBARS] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinbars: [CINBARS] REFUSED (try %d) %s - %s. Nothing written for this class.%s", g_tries, kClasses[i],
            stage < 0 ? "the RTTI scan faulted" : kStage[stage > 2 ? 2 : stage], g_tries >= kMaxTries ? "" : " Retried next second.");
        return g_tries >= kMaxTries ? -1 : 0;
    }
    const uintptr_t slot = vt + 8 * kSlot;
    uintptr_t have = 0;
    __try { have = *reinterpret_cast<const volatile uintptr_t*>(slot); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (have == reinterpret_cast<uintptr_t>(&UpdHook)) { return 1; }
    bool sigOk = false;
    __try { sigOk = have && memcmp(reinterpret_cast<const void*>(have), kSig, sizeof(kSig)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!sigOk || (g_orig && reinterpret_cast<uintptr_t>(g_orig) != have)) {
        // [PROBE: CINBARS] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinbars: [CINBARS] REFUSED %s - slot 114 gamedll+0x%zX holds gamedll+0x%zX, %s. Nothing written for this class.", kClasses[i],
            static_cast<size_t>(slot - gd), static_cast<size_t>(have - gd),
            !sigOk ? "whose first 24 bytes are not the side-bar update both known builds share"
                   : "a different function from the other class's slot");
        return -1;
    }
    g_orig = reinterpret_cast<UpdFn>(have);
    DWORD old = 0;
    // [LOADBEARING: cinbars vtable slot] UIGameMovieBackground + MenuGameMovie (RTTI COL offset 0) slot 114 -> UpdHook. OURS +1233020
    // (vtables +18A11D8 / +15D8E58), EGL +12326C0. Without it a VR cutscene shows I_HorzBar0 / I_HorzBar1 at the edges (INV-620 (b)).
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        // [PROBE: CINBARS] gate=none (always on)  site=FLAT  log-only=yes
        LOG("cinbars: [CINBARS] REFUSED %s - VirtualProtect on the slot failed (error %lu). Nothing written.", kClasses[i], GetLastError());
        return -1;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&UpdHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    uintptr_t back = 0;
    __try { back = *reinterpret_cast<const volatile uintptr_t*>(slot); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // [PROBE: CINBARS] gate=none (always on)  site=FLAT  log-only=yes
    LOG("cinbars: [CINBARS] ARMED (logged unconditionally) %s - slot 114 gamedll+0x%zX swapped (vtable +0x%zX, was gamedll+0x%zX, read "
        "back %s). In a VR session a cutscene's side bars stay hidden; flat keeps them.", kClasses[i], static_cast<size_t>(slot - gd),
        static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd), back == reinterpret_cast<uintptr_t>(&UpdHook) ? "OK" : "*** NOT OURS ***");
    return 1;
}

}  // namespace

void CinBarsInstall()
{
    if (g_done) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    ++g_tries;
    for (int i = 0; i < 2; ++i) {
        if (!g_classDone[i]) { g_classDone[i] = SwapOne(gd, i) != 0; }
    }
    g_done = g_classDone[0] && g_classDone[1];
}

}  // namespace dlvr
