// lockpitch - lockpick position P1. 2026-10-25.
//
// WHY (spec docs/superpowers/specs/2026-10-25-lockpick-position-p1-design.md; RESEARCH-LOCKPICK-SCREEN-2026-10-25 §1, §2, §5):
// while lockpicking the LockpickController owns the look (its slot 28 +D3FE90 returns its look interface, INV-272's mechanism) and
// that interface's slot 0 +D3F9D0 (Look_GetWantedDev) demands yaw = facing the lock and PITCH = elevation from the head point +1840 to
// the keyhole ctrl+148..156. The minigame (+782250) places the lock 0.40 m along the look during SHOW (~0.5 s), then freezes it.
// Our view keeps the head's own pitch (camhook.cpp YawOnly + head pitch / roll), so with a level head the lock sat 39.2-39.6 deg
// below the view centre (menupass_run1, demanded -45.2 deg, seated). Here the slot is swapped: the original runs, its yaw is kept,
// and its pitch becomes the rendered view's pitch - the game's look follows the head during SHOW and the lock appears where the
// player is looking. At the hand-back the look pitch is ~ the head's (INV-272's carry-over avoided).
// lpf1_out.txt: slot 0 == +D3F9D0 in .rdata; xmm2 written at +D3FA30 before any read -> signature (this, out), rax = out; the
// vtable's only reference +BEC556 (the controller builder). EGL (tools/egl_allowlist.tsv family 9): vtable 0x172D438, slot 0
// 0xD3EE70 - both translated exactly, and the EGL file's slot 0 read back = 0xD3EE70.
// COST: one 8-byte write at install; per call (only while lockpicking, the look update) three guarded loads and one seqlock read.
#include "lockpitch.h"

#include "camhook.h"   // CamHookGetRenderedViewMatrix - the rendered view (columns 0 / 1 / 2 = right / up / back, grappleaim.cpp:99)

#include <windows.h>

#include <cmath>
#include <cstdint>

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"   // the BETA tree: the Epic build's rvas (family 9)
#define DLVR_LOCKPITCH_HAVE_BUILDRVA 1
#endif
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

constexpr uintptr_t kVtableRva = 0x172E868;   // Steam rvas; buildrva family 9 moves both on the Epic build
constexpr uintptr_t kWantedRva = 0xD3F9D0;
constexpr size_t kLockA = 64, kLockB = 72, kLockC = 80;   // the interface's lock pointers, the original's own test (lps4 :52-66)

using WantedFn = float*(__fastcall*)(void* self, float* out);
WantedFn g_orig = nullptr;
bool g_done = false;
int  g_tries = 0;
constexpr int kMaxTries = 30;

volatile LONG   g_calls = 0, g_replaced = 0, g_stale = 0, g_noLock = 0, g_zero = 0, g_sessions = 0;
volatile LONG64 g_lastMs = 0;

uintptr_t Gd(uintptr_t rva)
{
#if defined(DLVR_LOCKPITCH_HAVE_BUILDRVA)
    return ::dlvr::buildrva::Gd(rva);
#else
    return rva;   // the DEV tree: Steam rvas only
#endif
}

uintptr_t ReadP(uintptr_t a)
{
    __try { return *reinterpret_cast<const volatile uintptr_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The rendered view's pitch in the game's convention (degrees, up positive, as +D3F9D0's atan(dy / horizontal)).
bool ViewPitchDeg(float* out)
{
    float m[12] = {};
    unsigned age = 0;
    if (!CamHookGetRenderedViewMatrix(m, &age)) { return false; }
    float s = -m[6];   // forward.y = -(back.y); column 2 = (m[2], m[6], m[10])
    s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    float d = asinf(s) * 57.2957795f;
    d = d > 89.0f ? 89.0f : (d < -89.0f ? -89.0f : d);
    *out = d;
    return true;
}

float* __fastcall WantedHook(void* self, float* out)
{
    float* r = g_orig(self, out);   // the game's yaw and pitch first - outside any __try
    InterlockedIncrement(&g_calls);
    const ULONGLONG now = GetTickCount64();
    const LONG64 last = InterlockedExchange64(&g_lastMs, static_cast<LONG64>(now));
    const bool newSession = last == 0 || now - static_cast<ULONGLONG>(last) > 1000;
    if (newSession && last != 0) {
        // [PROBE: LOCKPITCH] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("lockpitch: [LOCKPITCH] lockpick #%ld ended - calls %ld, pitch replaced %ld, kept the game's: stale view %ld, no lock %ld, "
            "TPP / zero %ld", g_sessions, g_calls - 1, g_replaced, g_stale, g_noLock, g_zero);
        InterlockedExchange(&g_calls, 1);
        InterlockedExchange(&g_replaced, 0);
        InterlockedExchange(&g_stale, 0);
        InterlockedExchange(&g_noLock, 0);
        InterlockedExchange(&g_zero, 0);
    }
    if (newSession) { InterlockedIncrement(&g_sessions); }
    const uintptr_t s = reinterpret_cast<uintptr_t>(self);
    if (!out || !ReadP(s + kLockA) || !ReadP(s + kLockB) || !ReadP(s + kLockC)) {
        InterlockedIncrement(&g_noLock);
        return r;
    }
    if (out[1] == 0.0f) {   // the original forces pitch 0 in third person
        InterlockedIncrement(&g_zero);
        return r;
    }
    float head = 0.0f;
    if (!ViewPitchDeg(&head)) {
        InterlockedIncrement(&g_stale);
        return r;
    }
    const float game = out[1];
    out[1] = head;
    if (InterlockedIncrement(&g_replaced) == 1) {
        // [PROBE: LOCKPITCH] gate=none (always on)  site=HEADSET  log-only=yes
        LOG("lockpitch: [LOCKPITCH] lockpick #%ld first call - the game asked pitch %+.1f deg (head point -> keyhole), the head's is "
            "%+.1f deg: the look follows the head, so the lock is placed where the player looks (yaw %+.1f deg kept)", g_sessions,
            game, head, out[0]);
    }
    return r;
}

}  // namespace

void LockPitchInstall()
{
    if (g_done) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    const char* build = "Steam (the DEV tree carries Steam rvas only)";
#if defined(DLVR_LOCKPITCH_HAVE_BUILDRVA)
    const ::dlvr::buildrva::Build b = ::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll);
    if (b == ::dlvr::buildrva::kNotLoaded) { return; }
    build = ::dlvr::buildrva::BuildName(b);
    if (b != ::dlvr::buildrva::kSteam && b != ::dlvr::buildrva::kEgl) {
        g_done = true;
        // [PROBE: LOCKPITCH] gate=none (always on)  site=FLAT  log-only=yes
        LOG("lockpitch: [LOCKPITCH] REFUSED - the gamedll is the %s build; only the Steam and Epic rvas are known. The lock stays where "
            "the game puts it.", build);
        return;
    }
#endif
    ++g_tries;
    const uintptr_t slot = gd + Gd(kVtableRva);
    const uintptr_t want = gd + Gd(kWantedRva);
    const uintptr_t have = ReadP(slot);
    if (have != want) {
        if (have == reinterpret_cast<uintptr_t>(&WantedHook)) { g_done = true; return; }
        g_done = g_tries >= kMaxTries || have != 0;
        // [PROBE: LOCKPITCH] gate=none (always on)  site=FLAT  log-only=yes
        LOG("lockpitch: [LOCKPITCH] REFUSED (try %d) - the lockpick look slot gamedll+0x%zX holds %p, expected gamedll+0x%zX (%p) on the "
            "%s build. Nothing written; the lock stays where the game puts it.%s", g_tries, static_cast<size_t>(slot - gd),
            reinterpret_cast<void*>(have), static_cast<size_t>(want - gd), reinterpret_cast<void*>(want), build,
            g_done ? "" : " Retried next second.");
        return;
    }
    g_orig = reinterpret_cast<WantedFn>(want);
    DWORD old = 0;
    // [LOADBEARING: lockpitch vtable slot] gamedll+0x172E868 slot 0 (EGL 0x172D438) -> WantedHook. lpf1_out.txt (slot == +D3F9D0,
    // .rdata, one reference). Without it the lock is placed at the keyhole's pitch, ~40 deg below a level VR view.
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        g_done = true;
        // [PROBE: LOCKPITCH] gate=none (always on)  site=FLAT  log-only=yes
        LOG("lockpitch: [LOCKPITCH] REFUSED - VirtualProtect on the slot failed (error %lu). Nothing written.", GetLastError());
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&WantedHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    g_done = true;
    // [PROBE: LOCKPITCH] gate=none (always on)  site=FLAT  log-only=yes
    LOG("lockpitch: [LOCKPITCH] ARMED (logged unconditionally) - %s build: lockpick look slot gamedll+0x%zX swapped (was gamedll+0x%zX, "
        "read back %s). While lockpicking the game's look pitch follows the head's; yaw unchanged.", build,
        static_cast<size_t>(slot - gd), static_cast<size_t>(want - gd),
        ReadP(slot) == reinterpret_cast<uintptr_t>(&WantedHook) ? "OK" : "*** NOT OUR FUNCTION ***");
}

}  // namespace dlvr
