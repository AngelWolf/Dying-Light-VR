// dlgview - the dialogue's camera point gets the same [EYEPOINT] view offset as on foot. 2026-10-25.
//
// WHY (spec docs/superpowers/specs/2026-10-25-dialogue-view-offset-design.md; INV-619 + addendum 1): GetCameraPos +C01860 takes its
// base from PlayerDI slot 230 +BA72F0, which asks the ACTIVE look interface (+BA0F70) for slot 6: the sentinel -> vis+4112 (the eye
// point [EYEPOINT] moves by cam_eye_forward / cam_eye_arc / cam_eye_up_mm); a position -> that position, as the view. DialogController's
// slot 6 (OURS +D05600, EGL +D04AA0) returns its own point (controller +156), so in a dialogue the view lost the whole offset while the
// hand anchor kept the forward part - a tester's "mantis" upper arms filling the view. Here the slot is swapped: the original runs, then
// the world offset the camera base got this update (boneblend.cpp EyePointViewOffsetWorld) is added - for the LOCAL player only
// (*(owner + 3144) == the controller, INV-327's table, ID 21).
// dc1 (ida/dc1_out.txt): slot 6's only consumer is +BA72F0. The vtable is found by RTTI name with the COL offset 24 (the ILookController
// sub-object) and accepted only if slot 6's first 24 bytes are the function both builds share - no rva table, so Steam and Epic arm
// and an unknown build either matches the bytes or refuses by name.
// COST: install - one RTTI scan of gamedll's data sections (once) + one 8-byte write. Per call (only while a dialogue owns the look,
// +BA72F0's callers) - the original, one guarded load, one shared-lock read, three adds.
#include "dlgview.h"

#include "boneblend.h"   // EyePointViewOffsetWorld - the world offset [EYEPOINT] gave the camera base

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

constexpr uint32_t kColOffset = 24;      // DialogController's ILookController sub-object (dlg3: sub-vtable offset 24)
constexpr size_t   kSlot = 6;            // +48: the look position (+BA72F0 :13 / :35)
constexpr size_t   kCtrlSlot = 3144;     // player + 2976 + 8 * 21 (INV-327's controller table, DialogController = ID 21)
// The slot's own bytes - OURS +D05600 == EGL +D04AA0 (read 2026-10-25 (2), scratchpad lookslot6.py): mov eax,[rcx+84h]; mov [rdx],eax;
// mov eax,[rcx+88h]; mov [rdx+4],eax; mov eax,[rcx+8Ch]; mov ... = return this+132..140 into out.
constexpr unsigned char kSig[24] = { 0x8b, 0x81, 0x84, 0x00, 0x00, 0x00, 0x89, 0x02, 0x8b, 0x81, 0x88, 0x00,
                                     0x00, 0x00, 0x89, 0x42, 0x04, 0x8b, 0x81, 0x8c, 0x00, 0x00, 0x00, 0x89 };

using PosFn = float*(__fastcall*)(void* self, float* out);
PosFn g_orig = nullptr;
bool  g_done = false;
int   g_tries = 0;
constexpr int kMaxTries = 30;

volatile LONG   g_calls = 0, g_added = 0, g_stale = 0, g_notLocal = 0, g_sessions = 0;
volatile LONG64 g_lastMs = 0;
volatile uintptr_t g_lookVt = 0;   // the found look-interface vtable (absolute) - DlgViewLookVtable; written once, 1 Hz thread

uintptr_t ReadP(uintptr_t a)
{
    __try { return *reinterpret_cast<const volatile uintptr_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Every non-executable readable section of the module, for the RTTI scan.
template <typename F>
void ForDataSections(uintptr_t base, F f)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        const DWORD c = s->Characteristics;
        if ((c & IMAGE_SCN_MEM_EXECUTE) || !(c & IMAGE_SCN_MEM_READ)) { continue; }
        f(base + s->VirtualAddress, static_cast<size_t>(s->Misc.VirtualSize));
    }
}

// MSVC x64 RTTI: name ".?AVDialogController@@" -> TypeDescriptor (name - 16) -> the COL that names it with offset kColOffset
// (signature 1, pSelf == its own rva) -> the vtable whose [-1] is that COL. 0 if any step finds nothing (the caller names which).
uintptr_t FindVtable(uintptr_t base, int* stage)
{
    static const char kName[] = ".?AVDialogController@@";
    uintptr_t td = 0;
    *stage = 0;
    __try {
        ForDataSections(base, [&](uintptr_t p, size_t n) {
            for (size_t i = 0; !td && i + sizeof(kName) <= n; ++i) {
                if (memcmp(reinterpret_cast<const void*>(p + i), kName, sizeof(kName)) == 0) { td = p + i - 16; }
            }
        });
        if (!td) { return 0; }
        *stage = 1;
        const uint32_t tdRva = static_cast<uint32_t>(td - base);
        uintptr_t col = 0;
        ForDataSections(base, [&](uintptr_t p, size_t n) {
            for (size_t i = 0; !col && i + 24 <= n; i += 4) {
                const uint32_t* c = reinterpret_cast<const uint32_t*>(p + i);
                if (c[0] == 1 && c[1] == kColOffset && c[3] == tdRva && c[5] == static_cast<uint32_t>(p + i - base)) { col = p + i; }
            }
        });
        if (!col) { return 0; }
        *stage = 2;
        uintptr_t vt = 0;
        ForDataSections(base, [&](uintptr_t p, size_t n) {
            for (size_t i = 0; !vt && i + 8 <= n; i += 8) {
                if (*reinterpret_cast<const uintptr_t*>(p + i) == col) { vt = p + i + 8; }
            }
        });
        if (vt) { *stage = 3; }
        return vt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *stage = -1;
        return 0;
    }
}

float* __fastcall PosHook(void* self, float* out)
{
    float* r = g_orig(self, out);   // the dialogue's own point - outside any __try
    InterlockedIncrement(&g_calls);
    const ULONGLONG now = GetTickCount64();
    const LONG64 last = InterlockedExchange64(&g_lastMs, static_cast<LONG64>(now));
    const bool newSession = last == 0 || now - static_cast<ULONGLONG>(last) > 1000;
    if (newSession && last != 0) {
        // [PROBE: DLGVIEW] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("dlgview: [DLGVIEW] dialogue #%ld ended - calls %ld, offset added %ld, kept the dialogue's point: no fresh offset %ld, "
            "not the local player %ld", g_sessions, g_calls - 1, g_added, g_stale, g_notLocal);
        InterlockedExchange(&g_calls, 1);
        InterlockedExchange(&g_added, 0);
        InterlockedExchange(&g_stale, 0);
        InterlockedExchange(&g_notLocal, 0);
    }
    if (newSession) { InterlockedIncrement(&g_sessions); }
    if (!r) { return r; }
    const float p[3] = { r[0], r[1], r[2] };
    float d[3] = {};
    uintptr_t owner = 0;
    const char* why = nullptr;
    if (!EyePointViewOffsetWorld(d, &owner)) {
        InterlockedIncrement(&g_stale);
        why = "no fresh eye-point offset (cam_eye_* all off, not armed, or > 250 ms old)";   // %s text ships readable in BETA
    } else if (!owner || ReadP(owner + kCtrlSlot) != reinterpret_cast<uintptr_t>(self) - kColOffset) {
        InterlockedIncrement(&g_notLocal);
        why = "the controller is not the local player's";
    } else {
        r[0] += d[0];
        r[1] += d[1];
        r[2] += d[2];
        InterlockedIncrement(&g_added);
    }
    if (newSession) {
        if (why) {
            // [PROBE: DLGVIEW] gate=none (always on)  site=HEADSET  log-only=yes
            LOG("dlgview: [DLGVIEW] dialogue #%ld first call - point %+.3f %+.3f %+.3f, NOTHING added: %s", g_sessions, p[0], p[1],
                p[2], why);
        } else {
            // [PROBE: DLGVIEW] gate=none (always on)  site=HEADSET  log-only=yes
            LOG("dlgview: [DLGVIEW] dialogue #%ld first call - point %+.3f %+.3f %+.3f, added the view offset %+.1f %+.1f %+.1f mm "
                "|%.1f| (world) - the view sits where it does on foot", g_sessions, p[0], p[1], p[2], 1000.0f * d[0],
                1000.0f * d[1], 1000.0f * d[2], 1000.0f * sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
    }
    return r;
}

}  // namespace

uintptr_t DlgViewLookVtable() { return g_lookVt; }

void DlgViewInstall()
{
    if (g_done) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    ++g_tries;
    int stage = 0;
    const uintptr_t vt = FindVtable(gd, &stage);
    if (!vt) {
        g_done = g_tries >= kMaxTries;
        static const char* const kStage[] = { "the class name .?AVDialogController@@ is not in the module",
                                              "no object locator names it with offset 24 (the look interface)",
                                              "no vtable points at that locator" };
        // [PROBE: DLGVIEW] gate=none (always on)  site=FLAT  log-only=yes
        LOG("dlgview: [DLGVIEW] REFUSED (try %d) - %s. Nothing written; a dialogue's view keeps the game's point.%s", g_tries,
            stage < 0 ? "the RTTI scan faulted" : kStage[stage > 2 ? 2 : stage], g_done ? "" : " Retried next second.");
        return;
    }
    g_lookVt = vt;   // found by RTTI - the scene stand-down's dialogue test needs only this, not the slot swap below
    const uintptr_t slot = vt + 8 * kSlot;
    const uintptr_t have = ReadP(slot);
    if (have == reinterpret_cast<uintptr_t>(&PosHook)) { g_done = true; return; }
    bool sigOk = false;
    __try { sigOk = have && memcmp(reinterpret_cast<const void*>(have), kSig, sizeof(kSig)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!sigOk) {
        g_done = true;
        // [PROBE: DLGVIEW] gate=none (always on)  site=FLAT  log-only=yes
        LOG("dlgview: [DLGVIEW] REFUSED - DialogController look slot 6 gamedll+0x%zX holds gamedll+0x%zX, whose first 24 bytes are not "
            "the look-position function both known builds share. Nothing written; a dialogue's view keeps the game's point.",
            static_cast<size_t>(slot - gd), static_cast<size_t>(have - gd));
        return;
    }
    g_orig = reinterpret_cast<PosFn>(have);
    DWORD old = 0;
    // [LOADBEARING: dlgview vtable slot] DialogController look interface (RTTI COL offset 24) slot 6 -> PosHook. OURS vtable +172C478
    // slot 6 +D05600, EGL +172B080 / +D04AA0. Without it a dialogue's view loses the cam_eye_* offset while the hands keep it (INV-619).
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        g_done = true;
        // [PROBE: DLGVIEW] gate=none (always on)  site=FLAT  log-only=yes
        LOG("dlgview: [DLGVIEW] REFUSED - VirtualProtect on the slot failed (error %lu). Nothing written.", GetLastError());
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&PosHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    g_done = true;
    // [PROBE: DLGVIEW] gate=none (always on)  site=FLAT  log-only=yes
    LOG("dlgview: [DLGVIEW] ARMED (logged unconditionally) - DialogController look slot 6 gamedll+0x%zX swapped (vtable +0x%zX, was "
        "gamedll+0x%zX, read back %s). In a dialogue the view gets the cam_eye_* offset it gets on foot.", static_cast<size_t>(slot - gd),
        static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd),
        ReadP(slot) == reinterpret_cast<uintptr_t>(&PosHook) ? "OK" : "*** NOT OUR FUNCTION ***");
}

}  // namespace dlvr
