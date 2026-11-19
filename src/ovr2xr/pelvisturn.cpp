// pelvisturn - the lower body turns with the STICK when looking down. 2026-11-19.
//
// WHY (trace docs/traces/20261119_1530_pelvis_stick_turn.md; INV-749; Jay, RUN ST1: "The turning with stick is mostly good, but when
// you look directly down it still has the delay"; his rule: "remove the delay from stick turning ONLY, but not from the headturn";
// option (a) "only while the stick is turning": "A"):
// the game keeps a pelvis direction (PlayerVis +11256..+11264) apart from the body facing (player +4588). UpdatePelvisDir (+C8AA80,
// from the vis post-update slot 129 +C5FD10) asks vis slot 270 (+C8ADE0, "can use pelvis stand rotation"); that answers TRUE only while
// the look pitch (player +4464) <= -35 deg (and a few other tests, INV-749). TRUE -> the pelvis holds and catches up (moving: a rate;
// standing: a snap at 65 deg + the PelvisRotate_L / _R step, 0.567 s). FALSE -> the pelvis = the facing at once. Our body facing turns
// with the stick in the same update (follow::FacingWithNeck), so looking down the lower body trailed the stick turn.
// HERE: PlayerFppVis's slot 270 is swapped; the original runs first and its answer is kept, EXCEPT that TRUE becomes FALSE while the
// right stick turns (|x| > 0.20, the S-60 check's own test). Head-driven body turns (the neck hold's pull past 40 deg) keep the game's
// step. Only the PlayerFppVis vtable (the local first-person body): PlayerTppVis / PlayerVis (co-op partners, the TPP body) unchanged.
// Facts (OUR gamedll 97046bba, set-aside): ida/pelv1_out.txt, pelv2_out.txt, pelv3_out.txt (slot 270 = +C8ADE0 in the three vis
// vtables), pelv4_out.txt (callers through +0x870: +C8AA80, +C89010 a blend time, 124 other classes' slot 270); the slot's bytes in the
// set-aside file E0 AD C8 80 01 00 00 00 (control: lockpitch's +172E868 = +D3F9D0).
// COST: one 8-byte write at install; per call (the FPP vis update, once per update) the original + one stick read when it says TRUE.
#include "pelvisturn.h"

#include "controllers.h"   // GetControllerStick

#include <windows.h>

#include <cstdint>

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"   // the BETA tree: which build is loaded
#define DLVR_PELVISTURN_HAVE_BUILDRVA 1
#endif
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

constexpr uintptr_t kSlotRva = 0x1705D08;   // PlayerFppVis vtable +1705498, slot 270 (+2160) - Steam
constexpr uintptr_t kGateRva = 0xC8ADE0;    // the stand-rotation gate the slot holds - Steam
constexpr float     kStickTurn = 0.20f;     // the right stick |x| that counts as a stick turn (the S-60 check's own test)

using GateFn = bool(__fastcall*)(void* vis);
GateFn g_orig = nullptr;
bool g_done = false;
int  g_tries = 0;
constexpr int kMaxTries = 30;

volatile LONG g_overrides = 0;   // stick-turn episodes in which the game asked for the hold and was answered no
volatile LONG g_wasOverriding = 0;

uintptr_t ReadP(uintptr_t a)
{
    __try { return *reinterpret_cast<const volatile uintptr_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

float PitchOf(void* vis)
{
    __try {
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(static_cast<char*>(vis) + 80);
        return player ? *reinterpret_cast<const float*>(player + 4464) : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

bool __fastcall StandRotHook(void* vis)
{
    const bool game = g_orig(vis);   // the game's answer first - outside any __try
    if (!game) {
        InterlockedExchange(&g_wasOverriding, 0);
        return false;
    }
    float sx = 0.0f, sy = 0.0f;
    const bool turning = GetControllerStick(kRightHand, sx, sy) && (sx > kStickTurn || sx < -kStickTurn);
    if (!turning) {
        InterlockedExchange(&g_wasOverriding, 0);
        return true;
    }
    if (InterlockedExchange(&g_wasOverriding, 1) == 0) {
        const LONG k = InterlockedIncrement(&g_overrides);
        if (k <= 20) {
            // [PROBE: PELVISTURN] gate=none (always on)  site=HEADSET  log-only=yes
            LOG("pelvisturn: [PELVISTURN] STICK TURN #%ld looking down - the game asked the lower body to hold (look pitch %+.1f deg, "
                "<= -35), answered NO: the pelvis follows the body at once (right stick x %+.2f)%s", k, PitchOf(vis), sx,
                k == 20 ? " | LAST line of this kind" : "");
        }
    }
    return false;
}

}  // namespace

void PelvisTurnInstall()
{
    if (g_done) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    const char* build = "Steam (the DEV tree carries Steam rvas only)";
#if defined(DLVR_PELVISTURN_HAVE_BUILDRVA)
    const ::dlvr::buildrva::Build b = ::dlvr::buildrva::ModuleBuild(::dlvr::buildrva::kGamedll);
    if (b == ::dlvr::buildrva::kNotLoaded) { return; }
    build = ::dlvr::buildrva::BuildName(b);
    if (b != ::dlvr::buildrva::kSteam) {
        g_done = true;
        // [PROBE: PELVISTURN] gate=none (always on)  site=FLAT  log-only=yes
        LOG("pelvisturn: [PELVISTURN] REFUSED - the gamedll is the %s build; only the Steam rvas are known (no Epic row yet). Looking "
            "down, the lower body keeps the game's delayed turn.", build);
        return;
    }
#endif
    ++g_tries;
    const uintptr_t slot = gd + kSlotRva;
    const uintptr_t want = gd + kGateRva;
    const uintptr_t have = ReadP(slot);
    if (have != want) {
        if (have == reinterpret_cast<uintptr_t>(&StandRotHook)) { g_done = true; return; }
        g_done = g_tries >= kMaxTries || have != 0;
        // [PROBE: PELVISTURN] gate=none (always on)  site=FLAT  log-only=yes
        LOG("pelvisturn: [PELVISTURN] REFUSED (try %d) - PlayerFppVis slot 270 gamedll+0x%zX holds %p, expected gamedll+0x%zX (%p) on the "
            "%s build. Nothing written; looking down the lower body keeps the game's delayed turn.%s", g_tries,
            static_cast<size_t>(slot - gd), reinterpret_cast<void*>(have), static_cast<size_t>(want - gd), reinterpret_cast<void*>(want),
            build, g_done ? "" : " Retried next second.");
        return;
    }
    g_orig = reinterpret_cast<GateFn>(want);
    DWORD old = 0;
    // [LOADBEARING: pelvisturn vtable slot] gamedll+0x1705D08 (PlayerFppVis slot 270) -> StandRotHook. INV-749 / ida/pelv3_out.txt.
    // Without it, looking down (pitch <= -35) the lower body trails every stick turn (RUN ST1).
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        g_done = true;
        // [PROBE: PELVISTURN] gate=none (always on)  site=FLAT  log-only=yes
        LOG("pelvisturn: [PELVISTURN] REFUSED - VirtualProtect on the slot failed (error %lu). Nothing written.", GetLastError());
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&StandRotHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    g_done = true;
    // [PROBE: PELVISTURN] gate=none (always on)  site=FLAT  log-only=yes
    LOG("pelvisturn: [PELVISTURN] ARMED (logged unconditionally) - %s build: PlayerFppVis slot 270 gamedll+0x%zX swapped (was "
        "gamedll+0x%zX, read back %s). Looking down, the lower body follows the body at once while the right stick turns (|x| > %.2f); "
        "head-driven turns keep the game's step.", build, static_cast<size_t>(slot - gd), static_cast<size_t>(want - gd),
        ReadP(slot) == reinterpret_cast<uintptr_t>(&StandRotHook) ? "OK" : "*** NOT OUR FUNCTION ***", kStickTurn);
}

}  // namespace dlvr
