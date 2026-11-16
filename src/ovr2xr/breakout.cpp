// Physical breakout - spec docs/superpowers/specs/2026-10-24-physical-breakout-design.md, plan
// docs/superpowers/plans/2026-10-24-physical-breakout.md Task 3. The decisions are breakout_logic.h (unit-tested).
//
// HOW (INV-618 + ADDENDUM 1; docs/RESEARCH-BREAKOUT-2026-10-24.md "Build constants"; OUR gamedll 97046bba):
//   - Being grabbed is the player's GrabController = *(player + 3000) (PlayerDI vfunc +816 = +B9EA00 forwards to +D237F0 on it).
//     Grabbed = byte +16 and the enemy pair +144 / +152 non-null (the game's own handlers refuse without them). Type = player vfunc
//     +872 (0-3 zombie grabs, 4 leg grab, 5 Volatile), anim set = +880, phase = +204, phase start = +208 (now = GC vfunc +48),
//     win-blocking byte +177 (anim event 541 sets, 540 clears - OnAnimEvent +D26750).
//   - Zombie: one shove -> the game's own phase setter +ECE6F0(player, 8) = what the KICK path +D21180 does, without its skill gate
//     +D24B80 (the setter reads no skill - ADDENDUM 1). Legal when the KICK path would be: phase 5 / 10, or a begin phase whose
//     elapsed time >= the late-begin float at +1C12300[16*type + 1 + 2*animSet]. Earlier = held (<= 1 s). +177 is logged, not
//     tested: breakout_run1 read it set through the whole fight phase, and the KICK path never reads it.
//   - Volatile: each shove -> kTapsPerShove of the game's own tap +D27AE0(GC), what OnInput does for TAP_MINIGAME (37). The bar is
//     the game's.
//   - THREAD (Build constants (a)): CLevel::Frame dispatches OnFrame (:250 -> PlayerDI +B9F600 -> UpdateControllers +BD9390 ->
//     GrabController Update) and later OnPostUpdate (:430 -> PlayerFppVis slot 129 = HookTail) serially on one thread, so
//     BreakoutTick runs on the grab's own thread, after that frame's grab update. BreakoutPoll (the scePad poll) only detects.
//   - HookTail's argument is PlayerFppVis; vis + 0x50 = PlayerDI (boneblend.cpp kPlayerDiOff, +C88BA0).
//   - Nothing is withheld or injected: A (tap) and Y (kick) reach the game as before.
//
// THREADS. BreakoutPoll (pad thread) publishes a shove under an SRW lock with a sequence number; BreakoutTick (game thread) takes
// it. g_grabbed is written by the tick and read by swing.cpp / meleethrow.cpp. Every game read and call is SEH-guarded; a fault
// turns the feature off for the launch, named.
#include "breakout.h"

#include "breakout_logic.h"
#include "calibscreen.h"    // CalibScreenActive - blocks input like the mod menu
#include "controllers.h"
#include "headpose.h"
#include "overlay.h"        // OverlayVisible - the mod menu

#include <windows.h>

#include <cstdint>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: the four fixed gamedll rvas below are on tools/egl_allowlist.tsv (family 8, translated exactly by
// tools/egl_rva_table.py); resolved ONCE with g_gd (CheckPlayer, game thread - their only reader). The DEV tree has no
// buildrva.h: Steam rvas only.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_BREAKOUT_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_BREAKOUT_GD_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: breakout_physical] required-for=the physical breakout - a two-handed shove escapes a zombie grab (the game's own phase 8)
//      and counts as kTapsPerShove struggle taps on a Volatile pin (BreakoutPhysical) - spec 2026-10-24-physical-breakout-design.
//      tree=INERT at 0: BreakoutPoll and BreakoutTick return at their first test, BreakoutPlayerGrabbed stays false.
//      HEADSET: needs both grip poses and the head pose (OpenXR, BreakoutPoll), the player-update tail detour (boneblend.cpp
//      HookTail) and the Steam gamedll 97046bba or the Epic build (its rvas from buildrva_table.h family 8) - READY checks
//      PlayerDI slot +816 against the build's forwarder; any other build REFUSES.
//      FLAT: READY only (no grip poses, no shove). Compiled default 1 since 2026-10-24 (3) (Jay: "They should be switched on"
//      -> "Yes." - for the tester package; zombie grab verified breakout_run2, Volatile not tested).
bool  BreakoutPhysical = true;
// [DEP: breakout_shove_speed_mps] required-for=how fast each hand must move for a shove (BreakoutShoveSpeedMps, m/s) - Claude's
//      starting value 1.6 (spec 6b.5), Tier D once Jay tunes it. tree=INERT at breakout_physical=0.
float BreakoutShoveSpeedMps = 1.6f;

namespace {

// ---- OUR gamedll 97046bba - INV-618, docs/RESEARCH-BREAKOUT-2026-10-24.md ----
constexpr size_t    kVisPlayerOff = 0x50;         // PlayerFppVis + 80 = PlayerDI (kPlayerDiOff, +C88BA0)
constexpr size_t    kSetPhaseSlotOff = 816;       // PlayerDI vfunc +816 = +B9EA00 (the build check)
constexpr uintptr_t kSetPhaseSlotRva = 0xB9EA00;
constexpr size_t    kGcOff = 3000;                // PlayerDI + 3000 = GrabController
constexpr size_t    kGcActiveOff = 16;            // byte
constexpr size_t    kGcEnemyAOff = 144;           // qword, both non-null while grabbed
constexpr size_t    kGcEnemyBOff = 152;
constexpr size_t    kGcPhaseOff = 204;            // int
constexpr size_t    kGcPhaseTOff = 208;           // float, phase start (GC vfunc +48 clock)
constexpr size_t    kGcBlockOff = 177;            // byte, anim-driven "no win yet"
constexpr size_t    kGcBarOff = 224;              // float, the struggle bar (> 1.0 wins)
constexpr size_t    kGcNowSlotOff = 48;           // GC vfunc +48 -> float now
constexpr size_t    kTypeSlotOff = 872;           // PlayerDI vfunc +872 -> grab type
constexpr size_t    kAnimSetSlotOff = 880;        // PlayerDI vfunc +880 -> anim set
constexpr uintptr_t kLateBeginRva = 0x1C12300;    // float table, [16*type + 1 + 2*animSet]
constexpr uintptr_t kSetPhaseRva = 0xECE6F0;      // void(player, phase) - the game's phase setter
constexpr uintptr_t kTapRva = 0xD27AE0;           // void(GrabController) - the game's TAP_MINIGAME handler
constexpr unsigned  kFastBreakPhase = 8;          // fast_finish_win_break
constexpr int       kTapsPerShove = 2;            // Volatile: game taps per shove - 0.200 each (vb1_run) -> the win on shove 3
constexpr long      kMaxEventLines = 200;         // INV-9

using VFnInt = int(__fastcall*)(uintptr_t);
using VFnFloat = float(__fastcall*)(uintptr_t);
using SetPhaseFn = void(__fastcall*)(uintptr_t, unsigned);
using TapFn = void(__fastcall*)(uintptr_t);

uintptr_t g_gd = 0;
// the rvas as the loaded build has them (Steam values until CheckPlayer resolves them with g_gd)
uintptr_t g_rSetPhaseSlot = kSetPhaseSlotRva, g_rLateBegin = kLateBeginRva, g_rSetPhase = kSetPhaseRva, g_rTap = kTapRva;
int g_resolved = 0;             // 0 not yet, 1 ready, -1 refused / faulted (off for the launch)
uintptr_t g_checkedPlayer = 0;  // the player pointer whose vtable passed the build check
volatile LONG g_nEvent = 0;
volatile bool g_grabbed = false;

// pad thread -> game thread
SRWLOCK g_lock = SRWLOCK_INIT;
LONG g_seq = 0;                  // under g_lock
double g_shoveT = 0.0;
breakout::ShoveInfo g_shoveInfo{};

breakout::ShoveDetector g_det;   // pad thread only
bool g_padOn = false;

// game thread only
LONG g_takenSeq = 0;
bool g_pending = false;
bool g_pendingSaidHeld = false;
double g_pendingT = 0.0;
breakout::ShoveInfo g_pendingInfo{};
bool g_wasGrabbed = false;
uintptr_t g_lastEnemy = 0;      // GrabController +144 of the grab in progress - a different enemy = a new grab (breakout::Edges)
double g_grabStartT = 0.0;
volatile ULONGLONG g_grabStartMs = 0;   // GetTickCount64 at GRAB START - BreakoutMeleeGateClosed (0 = never stamped)
volatile int g_gatePhase = -1;           // the grab's phase (GrabController +204) as this tick read it, -1 not grabbed - BreakoutMeleeGateClosed
int g_lastPhase = -1, g_grabType = -1, g_shoves = 0, g_calls = 0;

double NowS()   // the same clock as looting.cpp / bodygrab.cpp NowS
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
bool EventLine() { return InterlockedIncrement(&g_nEvent) <= kMaxEventLines; }

// ---- SEH-guarded game reads and calls (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadByte(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadInt(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadFloat(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool VCallInt(uintptr_t obj, size_t slotOff, int* out)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        *out = reinterpret_cast<VFnInt>(*reinterpret_cast<const uintptr_t*>(vt + slotOff))(obj);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool VCallFloat(uintptr_t obj, size_t slotOff, float* out)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        *out = reinterpret_cast<VFnFloat>(*reinterpret_cast<const uintptr_t*>(vt + slotOff))(obj);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetPhase(uintptr_t player, unsigned phase)
{
    __try { reinterpret_cast<SetPhaseFn>(g_gd + g_rSetPhase)(player, phase); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallTap(uintptr_t gc)
{
    __try { reinterpret_cast<TapFn>(g_gd + g_rTap)(gc); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void TurnOff(const char* why)
{
    g_resolved = -1;
    g_grabbed = false;
    // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=no
    LOG("breakout: [BREAKOUT] OFF - %s; the physical breakout stays off this launch (A / Y unchanged)", why);
}

// The build check, once per player pointer: PlayerDI vfunc +816 must be +B9EA00. READY / REFUSED once.
bool CheckPlayer(uintptr_t player)
{
    if (g_resolved < 0) { return false; }
    if (g_resolved == 1 && player == g_checkedPlayer) { return true; }
    if (!g_gd) {
        const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
        if (!gd) { return false; }
        g_rSetPhaseSlot = DLVR_BREAKOUT_GD_RVA(kSetPhaseSlotRva);
        g_rLateBegin = DLVR_BREAKOUT_GD_RVA(kLateBeginRva);
        g_rSetPhase = DLVR_BREAKOUT_GD_RVA(kSetPhaseRva);
        g_rTap = DLVR_BREAKOUT_GD_RVA(kTapRva);
        g_gd = gd;
    }
    uintptr_t vt = 0, slot = 0;
    if (!ReadPtr(player, &vt) || !ReadPtr(vt + kSetPhaseSlotOff, &slot) || slot != g_gd + g_rSetPhaseSlot) {
        g_resolved = -1;
        // [PROBE: BREAKOUT] gate=breakout_physical  site=FLAT  log-only=no
        LOG("breakout: [BREAKOUT] REFUSED - player %p vfunc +%zu reads +%llX, expected +%llX: not the Steam gamedll 97046bba / the "
            "Epic build these offsets were read from. The physical breakout stays off this launch (A / Y unchanged).",
            reinterpret_cast<void*>(player), kSetPhaseSlotOff,
            static_cast<unsigned long long>(slot ? slot - g_gd : 0), static_cast<unsigned long long>(g_rSetPhaseSlot));
        return false;
    }
    if (g_resolved == 0) {
        // [PROBE: BREAKOUT] gate=breakout_physical  site=FLAT  log-only=no
        LOG("breakout: [BREAKOUT] READY - player %p, GrabController at +%zu, shove speed %.2f m/s per hand | phase setter +%llX, tap "
            "+%llX (%s) | game thread %lu",
            reinterpret_cast<void*>(player), kGcOff, static_cast<double>(BreakoutShoveSpeedMps),
            static_cast<unsigned long long>(g_rSetPhase), static_cast<unsigned long long>(g_rTap),
            g_rSetPhase == kSetPhaseRva ? "Steam rvas" : "the Epic build's rvas", GetCurrentThreadId());
    }
    g_resolved = 1;
    g_checkedPlayer = player;
    return true;
}

const char* KindName(breakout::Kind k)
{
    switch (k) {
    case breakout::Kind::Phase8: return "ACCEPTED phase 8";
    case breakout::Kind::Tap: return "ACCEPTED tap";
    case breakout::Kind::Hold: return "HELD";
    default: return "REFUSED";
    }
}

}  // namespace

bool BreakoutPlayerGrabbed() { return g_grabbed; }

// The swing's hit gate: open for the first kMeleeOpenMs of a grab (an attack as the zombie grabs still counts), closed after (a
// shove reads as a swing) until the player has won the grab (phase 6-8, Jay 2026-11-16). All three fields are written by
// BreakoutTick on the game thread - the swing's thread too. COST: three loads.
bool BreakoutMeleeGateClosed() { return breakout::MeleeGateClosed(g_grabbed, g_grabStartMs, GetTickCount64(), g_gatePhase); }

// scePad poll (~570/s). COST: off = one bool test; on = three OpenXR pose reads (cached by the controller layer) + the detector's
// two 128-sample windows (a backward scan to the sample 0.1 s old). No game reads.
void BreakoutPoll()
{
    if (!BreakoutPhysical) {
        if (g_padOn) { g_det.Reset(); g_padOn = false; }
        return;
    }
    g_padOn = true;
    if (OverlayVisible() || CalibScreenActive()) {
        g_det.Reset();
        return;
    }
    XrPosef l{}, r{}, h{};
    const bool haveH = GetHeadPose(h);
    const bool haveL = haveH && GetControllerGripPose(kLeftHand, l);
    const bool haveR = haveH && GetControllerGripPose(kRightHand, r);
    static bool s_first = false;
    if (!s_first) {
        s_first = true;
        // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
        LOG("breakout: [BREAKOUT] POLL first sample - head %s, left grip %s, right grip %s | pad thread %lu",
            haveH ? "yes" : "NO", haveL ? "yes" : "NO", haveR ? "yes" : "NO", GetCurrentThreadId());
    }
    const float pl[3] = { l.position.x, l.position.y, l.position.z };
    const float pr[3] = { r.position.x, r.position.y, r.position.z };
    const float ph[3] = { h.position.x, h.position.y, h.position.z };
    breakout::ShoveParams p;
    p.speedMps = BreakoutShoveSpeedMps;
    breakout::ShoveInfo info{};
    const double now = NowS();
    if (g_det.Push(now, pl, haveL, pr, haveR, ph, p, &info)) {
        AcquireSRWLockExclusive(&g_lock);
        g_shoveT = now;
        g_shoveInfo = info;
        ++g_seq;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

// Player-update tail, after the frame's grab update (Build constants (a)). COST: off = one bool test; on, not grabbed = 5 guarded
// reads; grabbed = + 2 virtual calls on the grab's edges; a pending shove = <= 3 virtual calls + one game call, once per shove.
void BreakoutTick(void* playerVis)
{
    if (!BreakoutPhysical) {
        // switched off (the DEV menu can): forget the grab and any held shove, so switching on mid-grab starts clean (final review)
        g_grabbed = false;
        g_wasGrabbed = false;
        g_pending = false;
        g_lastEnemy = 0;
        return;
    }
    if (g_resolved < 0 || !playerVis) { return; }
    uintptr_t player = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(playerVis) + kVisPlayerOff, &player) || !player) { return; }
    if (!CheckPlayer(player)) { return; }

    uintptr_t gc = 0, ea = 0, eb = 0;
    unsigned char active = 0;
    if (!ReadPtr(player + kGcOff, &gc) || !gc) {
        // a player being built or torn down (loading) - wait, do not switch off for the launch (final review)
        static bool s_saidNoGc = false;
        if (!s_saidNoGc) {
            s_saidNoGc = true;
            // [PROBE: BREAKOUT] gate=breakout_physical  site=FLAT  log-only=yes
            LOG("breakout: [BREAKOUT] waiting - PlayerDI + 3000 holds no GrabController yet (said once)");
        }
        g_grabbed = false;
        return;
    }
    if (!ReadByte(gc + kGcActiveOff, &active) || !ReadPtr(gc + kGcEnemyAOff, &ea) || !ReadPtr(gc + kGcEnemyBOff, &eb)) {
        TurnOff("the GrabController could not be read");
        return;
    }
    const bool grabbed = active != 0 && ea != 0 && eb != 0;
    const double now = NowS();
    int phase = -1;
    if (grabbed) { ReadInt(gc + kGcPhaseOff, &phase); }

    const breakout::GrabEdge edge = breakout::Edges(g_wasGrabbed, g_lastEnemy, grabbed, ea);
    if (edge.ended) {
        if (EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] GRAB END - type %d, last phase %d (6 / 7 / 8 = escaped, 9 = lost), shoves %d, calls %d, %.2f s%s",
                g_grabType, g_lastPhase, g_shoves, g_calls, now - g_grabStartT, grabbed ? " - a new grab follows at once" : "");
        }
        if (g_pending && EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] SHOVE dropped - the grab ended while it was held");
        }
        g_pending = false;
    }
    if (edge.started) {
        int animSet = -1;
        if (!VCallInt(player, kTypeSlotOff, &g_grabType) || !VCallInt(player, kAnimSetSlotOff, &animSet)) {
            TurnOff("the grab type / anim set virtual faulted");
            return;
        }
        g_grabStartT = now;
        g_grabStartMs = GetTickCount64();   // before g_grabbed goes true below: the gate never sees a grab without its stamp
        g_shoves = 0;
        g_calls = 0;
        if (EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] GRAB START - type %d (0-3 zombie, 4 leg, 5 Volatile), anim set %d, phase %d | game thread %lu",
                g_grabType, animSet, phase, GetCurrentThreadId());
        }
    }
    g_wasGrabbed = grabbed;
    g_lastEnemy = grabbed ? ea : 0;
    g_gatePhase = grabbed ? phase : -1;   // before g_grabbed: a reader that sees this tick's grab sees this tick's phase or the last
    g_grabbed = grabbed;
    if (grabbed) { g_lastPhase = phase; }

    // take a new shove (newest wins)
    AcquireSRWLockShared(&g_lock);
    const LONG seq = g_seq;
    const double t = g_shoveT;
    const breakout::ShoveInfo info = g_shoveInfo;
    ReleaseSRWLockShared(&g_lock);
    if (seq != g_takenSeq) {
        g_takenSeq = seq;
        g_pending = true;
        g_pendingSaidHeld = false;
        g_pendingT = t;
        g_pendingInfo = info;
        if (grabbed) { ++g_shoves; }
    }
    if (!g_pending) { return; }

    if (breakout::Expired(g_pendingT, now)) {
        g_pending = false;
        if (EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] SHOVE dropped - held %.2f s without becoming legal (phase %d)", now - g_pendingT, phase);
        }
        return;
    }
    breakout::GrabState g{};
    g.grabbed = grabbed;
    g.type = g_grabType;
    g.phase = phase;
    float gcNow = 0.0f, phaseT = 0.0f;
    unsigned char block = 0;
    int animSet = 0;
    if (grabbed) {
        // the type now, not only at the start edge - the call a shove makes depends on it (final review Important 1 (c))
        int typeNow = g_grabType;
        if (!VCallInt(player, kTypeSlotOff, &typeNow)) {
            TurnOff("the grab type virtual faulted");
            return;
        }
        if (typeNow != g_grabType) {
            if (EventLine()) {
                // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
                LOG("breakout: [BREAKOUT] grab type changed %d -> %d since GRAB START (the shove uses %d)", g_grabType, typeNow, typeNow);
            }
            g_grabType = typeNow;
            g.type = typeNow;
        }
        if (!VCallFloat(gc, kGcNowSlotOff, &gcNow) || !ReadFloat(gc + kGcPhaseTOff, &phaseT) || !ReadByte(gc + kGcBlockOff, &block)) {
            TurnOff("the grab clock / phase time / block byte could not be read");
            return;
        }
        g.elapsedS = gcNow - phaseT;
        g.winBlocked = block != 0;
        if (g_grabType >= 0 && g_grabType <= 3) {
            if (!VCallInt(player, kAnimSetSlotOff, &animSet) || animSet < 0 || animSet > 7 ||
                !ReadFloat(g_gd + g_rLateBegin + 4 * static_cast<uintptr_t>(16 * g_grabType + 1 + 2 * animSet), &g.lateBeginS)) {
                TurnOff("the late-begin threshold could not be read");
                return;
            }
        }
    }
    const breakout::Kind k = breakout::Decide(g);
    const char* why = !grabbed ? "not grabbed"
                    : (g_grabType == 4) ? "leg grab - left to A"
                    : (g_grabType < 0 || g_grabType > 5) ? "a grab type this does not handle"
                    : "the grab is finishing";
    // A shove outside a grab (any fast two-handed motion in play) has its own small cap, so it cannot use up the grab lines' budget.
    static long s_nNotGrabbed = 0;
    if (k != breakout::Kind::Hold || !g_pendingSaidHeld) {
        if (grabbed ? EventLine() : (++s_nNotGrabbed <= 20)) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] SHOVE - left %.2f right %.2f m/s, %.0f deg apart, toward head %.2f | type %d phase %d, %.2f s "
                "into it (late begin %.2f), block %d -> %s%s%s",
                static_cast<double>(g_pendingInfo.speedL), static_cast<double>(g_pendingInfo.speedR),
                static_cast<double>(g_pendingInfo.angleDeg), static_cast<double>(g_pendingInfo.towardHead), g_grabType, phase,
                static_cast<double>(g.elapsedS), static_cast<double>(g.lateBeginS), static_cast<int>(block), KindName(k),
                k == breakout::Kind::Ignore ? " " : "", k == breakout::Kind::Ignore ? why : "");
        }
        if (k == breakout::Kind::Hold) { g_pendingSaidHeld = true; }
    }
    if (k == breakout::Kind::Hold) { return; }
    g_pending = false;
    if (k == breakout::Kind::Phase8) {
        if (!CallSetPhase(player, kFastBreakPhase)) {
            TurnOff("the game's phase setter faulted");
            return;
        }
        ++g_calls;
        int after = -1;
        ReadInt(gc + kGcPhaseOff, &after);
        if (EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] CALL phase 8 - phase %d -> %d", phase, after);
        }
    } else if (k == breakout::Kind::Tap) {
        // kTapsPerShove of the game's own taps per shove, stopping at the first that changes the phase (the win). vb1_run: 0.200 a
        // tap, the win needs the bar > 1.0 = 6 taps, and the Volatile kills from ~2.1 s in; Jay 10-30 (8): "3 Shoves".
        float before = 0.0f, after = 0.0f;
        ReadFloat(gc + kGcBarOff, &before);
        int phaseAfter = phase, taps = 0;
        while (taps < kTapsPerShove && phaseAfter == phase) {
            if (!CallTap(gc)) {
                TurnOff("the game's tap handler faulted");
                return;
            }
            ++g_calls;
            ++taps;
            ReadInt(gc + kGcPhaseOff, &phaseAfter);
        }
        ReadFloat(gc + kGcBarOff, &after);
        if (EventLine()) {
            // [PROBE: BREAKOUT] gate=breakout_physical  site=HEADSET  log-only=yes
            LOG("breakout: [BREAKOUT] CALL tap x%d - bar %.3f -> %.3f, phase %d -> %d", taps, static_cast<double>(before),
                static_cast<double>(after), phase, phaseAfter);
        }
    }
}

}  // namespace dlvr
