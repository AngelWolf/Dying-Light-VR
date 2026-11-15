// boltwheel.cpp - the game's quiver wheels opened from a quiver hold, with no X ever sent: the crossbow BOLT wheel from the bolt
// holster, and (2026-11-11, Jay: "We need to have the arrow selection on the right shoulders quiver for the regular bow, same as we do
// the crossbow" -> "Yes.") the bow's ARROW wheel from the right-shoulder quiver.
// Spec docs/superpowers/specs/2026-11-04-crossbow-bolt-wheel-design.md (approved; hook shape approved by Jay: "Sure"); plan
// docs/superpowers/plans/2026-11-04-crossbow-bolt-wheel.md Task 2. Facts: INV-678 + addendum (OUR gamedll 97046bba, ida/bwh1..bwh8);
// the arrow wheel: ida/wh4_gamedll_out.txt + ida/awh1_gamedll_out.txt (awh1_arrowwheel.py) + ida/awh2_out.txt, OUR gamedll 97046bba.
//
// THE GAME SIDE: HudBoltSelection / HudArrowsSelection (the game's own wheels) open on ONE input event of their show action (310 /
//   288) delivered to their input target (object + 88, IInputTarget vtable slot 6 = OnInputGameController +575830: target, id, xmm2
//   value, flag) and stay open only while their Update (main vtable slot 178, +575C70, shared by every wheel) reads
//   sub_181110BC0(im, show, target) == 1.0 - the lookup returns the max of the stored value over the action's bindings, which is the
//   real X (QUICK_BOLTS / QUICK_ARROWS) and so 0 here. Can-execute: the bolt wheel = held item type 37 (crossbow); the arrow wheel
//   (+511E10, awh1 (a)) = the item from held slot 1 is type 35 (bow), plus the same menu / sub_180B9BB40 / level refusals.
//
// WHAT IS HOOKED:
//   (1) the lookup +1110BC0 (10 call sites, two are tail jmps - bwh8): a 16-byte detour (boundaries 5 / 10 / 12 / 14 / 16, no
//       rip-relative operand, no branch into the range - bwh8). While a wheel's drive want is fresh, THAT wheel's show id returns 1.0;
//       every other id, and a show id without its want, goes to the original. Epic: the rva is translated by buildrva (egl_allowlist
//       0x1110BC0 -> 0x1110050, exact) in the BETA tree; any build whose 16 bytes differ is refused by name. Who else reads 288 through
//       the lookup (awh1 (b)): of 108 instructions putting 0x120 in an argument register or comparing with it, none is in a function
//       that calls the lookup or its three wrappers (+782E60 / +9FAB90 / +BA0F20) - a data-driven id is not excluded (as for 310).
//   (2) each wheel's MAIN vtable (found by RTTI name, object-locator offset 0 - never by rva) slot 178 -> UpdateHook, accepted only if
//       the slot's target starts with +575C70's 24 bytes. UpdateHook tells the wheels apart by the object's own vtable pointer, runs
//       that wheel's original FIRST (its full argument list is not proven - nothing of ours may run before it), then, if the want is
//       fresh, the wheel closed and no event sent this hold (boltwheel::OpenEventDue), calls the wheel's own
//       OnInputGameController(this + 88, show, 1.0, 0) - on the wheel's own thread, so the event never races its Update.
//   (3) 2026-11-11 (Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build it"): once a wheel's Update
//       slot is live, its INPUT TARGET vtable slot 6 (OnInputGameController +575830, byte-checked) -> OnInputHook, which drops that
//       wheel's own show event (the real X's QUICK_ARROWS / QUICK_BOLTS press) and passes every other id; the lookup then reads the show
//       action 0 without our want. So X opens neither wheel and a quick X no longer "selects next". Our open calls the original directly.
//   (4) the "X Reload" prompt (Jay: "Reload prompt only"): HudReloadNotifyDI's picker +5A8CE0 reload candidate immediate +5A8F44 -> 0
//       while manual_reload is on (PromptTick, ida/rn1..rn6); the other prompts of that box (heal, repair, fuel, traps) are untouched.
//   (5) the "Hold X to change Arrows / Bolts" tutorial hints and the combat Grapple prompt (Jay: "GRAPPLE PROMPT/HOLD X TO CHANGE
//       ARROWS" -> "Yes"): PatchTick writes boltwheel_logic.h kPatches (the hint update's two gates, HudNageWaza's show byte), each by
//       its own gate (the arrow wheel's / manual_reload / enemy_push_grab), back to the game's bytes when the gate goes off.
//   The detour is installed BEFORE any slot, and a slot only if the detour is live: an open event without the held value would be
//   closed on the next Update with the show action down < 0.2 s = the game's "select next", a change nobody chose.
//   Gates: the bolt wheel rides manual_reload, the arrow wheel bow_manual_nock + bow_manual_draw (no key of its own). Each is installed
//   when its gate is on (camhook's 1 Hz caller retries while off).
#include "boltwheel.h"

#include <windows.h>
#include <xmmintrin.h>

#include <cstdint>
#include <cstring>

#include "../common/log.h"
#include "boltwheel_logic.h"
#include "boneblend.h"    // InstallSingleGameDetour - the shared verify / suspend / commit path; BowManualDraw
#include "bownock.h"      // BowManualNock - the arrow wheel's master
#include "pushgrab.h"     // EnemyPushGrab - the grapple prompt's gate (the native grabs replace the Grapple skill)
#include "reload.h"       // ManualReload - the bolt wheel's master
#include "rtti_vtable.h"  // RttiFindVtable

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_BW_GD(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_BW_GD(rva) (rva)
#endif

namespace dlvr {
namespace {

constexpr uintptr_t kLookupRva = 0x1110BC0;   // sub_181110BC0 (Steam 97046bba; EGL 0x1110050 through buildrva)
const unsigned char kLookupHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57 };
constexpr size_t    kUpdateSlot = 178;        // main vtable slot 178 = Update +575C70 (shared HudItemSelectionBase; bolts + arrows - wh4)
const unsigned char kUpdateHead[24] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1,
                                        0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x48, 0x89, 0x58, 0xE0, 0x4C };   // Steam == EGL (protoscan)
constexpr size_t    kTargetOff = 88;          // the IInputTarget sub-object (RTTI locator offset 88; arrows: +14CAC08 - awh1 (a))
constexpr size_t    kOpenOff = 216;           // the wheel's open byte (OnInput sets target + 128 = object + 216; Update reads object + 216)
constexpr size_t    kOnInputSlot = 6;         // IInputTarget slot 6 = OnInputGameController +575830 (arrows too - awh1 (a) control)
// OnInputGameController +575830's first 24 bytes: Steam 97046bba == EGL (one hit each, 2026-11-11 scan; engine 0 hits). Its jnz rel32
// at +3 is the same in both. The slot is accepted for the X block only if its target starts with these.
const unsigned char kOnInputHead[24] = { 0x45, 0x84, 0xC9, 0x0F, 0x85, 0x28, 0x04, 0x00, 0x00, 0x55, 0x41, 0x56,
                                         0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0xA0 };
// The reload prompt's candidate immediate (boltwheel_logic.h kPromptImm*; ida/rn6_gamedll_out.txt): the instruction
// `8B C6 | 41 C7 04 87 [01] 00 00 00 | 41 8B F5` = mov eax, esi; mov dword ptr [r15+rax*4], 1; mov esi, r13d. The rva is of the
// immediate byte (EGL through buildrva, allowlist kind imm); the 13 bytes around it are checked before every write.
constexpr uintptr_t kPromptImmRva = 0x5A8F44;
constexpr size_t    kPromptCtxBefore = 6;     // the immediate is 6 bytes into the 13 checked
const unsigned char kPromptCtx[13] = { 0x8B, 0xC6, 0x41, 0xC7, 0x04, 0x87, 0x01, 0x00, 0x00, 0x00, 0x41, 0x8B, 0xF5 };

using LookupFn = __m128(__fastcall*)(void* im, int id, void* target);
using UpdateFn = long long(__fastcall*)(void* self, long long a2, int a3);
using OnInputFn = void(__fastcall*)(void* target, int id, float value, char flag);

constexpr int kBolts = 0, kArrows = 1;

struct Wheel {
    const char* cls;                  // the RTTI type name
    int show;                         // its show action
    uintptr_t mainVt = 0;             // the main vtable (locator offset 0) - the live object's +0 points at it
    uintptr_t targetVt = 0;           // the IInputTarget vtable (locator offset 88) - the live object's +88 must point at it
    UpdateFn updateTrue = nullptr;
    volatile LONG state = 0;          // 0 not tried, 1 live, -1 refused (written by camhook's 1 Hz thread, read by the pad thread)
    bool saidOff = false;
    volatile LONG want = 0;           // the pad thread's want ...
    volatile LONG64 wantMs = 0;       // ... and its stamp
    volatile LONG sent = 0;           // an open event was sent this hold (game thread)
    volatile LONG opens = 0;
    volatile LONG firstUpdate = 0, firstForced = 0, forcing = 0, badTarget = 0;
    // The X block (2026-11-11): the input target vtable's slot 6 -> OnInputHook, so the real X's show event never opens the wheel.
    OnInputFn onInputTrue = nullptr;
    volatile LONG xBlock = 0;         // 1 = the slot is ours (read by the game's input / lookup threads)
    volatile LONG xDropped = 0;       // the real X's show presses dropped (value != 0, flag 0 - the events that would open it)
};
Wheel g_w[2] = { { ".?AVHudBoltSelection@@", boltwheel::kShowAction }, { ".?AVHudArrowsSelection@@", boltwheel::kArrowShowAction } };

LookupFn g_lookupTrue = nullptr;
volatile LONG g_detour = 0;               // 0 not tried, 1 live, -1 refused
char g_detourWhy[256] = {};               // why the detour was refused (each wanted wheel's REFUSED line quotes it)
uintptr_t g_detourAt = 0;
int g_detourAttempts = 0;
volatile LONG g_unknownObj = 0;

bool WantFreshNow(const Wheel& w)
{
    return boltwheel::WantFresh(w.want != 0, static_cast<unsigned long long>(w.wantMs), GetTickCount64());
}

// COST per call (every input value read of the game, many a frame): two int compares on the common path; for id 310 / 288 only, two
// volatile loads and a GetTickCount64.
__m128 __fastcall LookupHook(void* im, int id, void* target)
{
    if (id == boltwheel::kShowAction || id == boltwheel::kArrowShowAction) {
        const int k = id == boltwheel::kShowAction ? kBolts : kArrows;
        Wheel& w = g_w[k];
        const int read = boltwheel::ShowLookup(id, w.show, WantFreshNow(w), w.xBlock != 0);
        if (read == 0) { return _mm_set_ss(0.0f); }   // the X block: the real X never holds the wheel open
        if (read == 1) {
            InterlockedExchange(&w.forcing, 1);
            if (InterlockedCompareExchange(&w.firstForced, 1, 0) == 0) {
                if (k == kBolts) {
                    // [PROBE: BOLTWHEEL] gate=manual_reload  site=HEADSET  log-only=yes
                    LOG("boltwheel: [BOLTWHEEL] FIRST FORCED READ of the bolt wheel's show action 310 = 1.0 (thread %lu) - the bolt holster "
                        "hold is down.", GetCurrentThreadId());
                } else {
                    // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=HEADSET  log-only=yes
                    LOG("boltwheel: [ARROWWHEEL] FIRST FORCED READ of the arrow wheel's show action 288 = 1.0 (thread %lu) - the quiver "
                        "hold is down.", GetCurrentThreadId());
                }
            }
            return _mm_set_ss(1.0f);
        }
    }
    return g_lookupTrue(im, id, target);
}

bool ReadByte(uintptr_t a, unsigned char& v)
{
    __try { v = *reinterpret_cast<const volatile unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadPtr(uintptr_t a, uintptr_t& v)
{
    __try { v = *reinterpret_cast<const volatile uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SendOpen(uintptr_t target, OnInputFn fn, int show)
{
    __try { fn(reinterpret_cast<void*>(target), show, 1.0f, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The X block (2026-11-11, Jay: "I'd like holding X to no longer bring up the arrow/bolt wheels" -> "Yes, build it"): each wheel's
// input target vtable slot 6 (OnInputGameController +575830, called by the input dispatcher sub_1811928D0 through vfunc +48 - INV-678)
// lands here. The wheel's own show event is dropped (DropShowEvent); every other id goes to the original. Our quiver open never
// comes through here - UpdateHook calls the original directly.
// COST per call (one per input event delivered to these two wheels - on a value change only, INV-678): one guarded pointer read, two
// compares; a dropped X press adds one interlocked increment and, for the first ones, a log line.
void __fastcall OnInputHook(void* target, int id, float value, char flag)
{
    uintptr_t vt = 0;
    ReadPtr(reinterpret_cast<uintptr_t>(target), vt);
    const int k = (vt && vt == g_w[kBolts].targetVt) ? kBolts : ((vt && vt == g_w[kArrows].targetVt) ? kArrows : -1);
    if (k >= 0) {
        Wheel& w = g_w[k];
        if (boltwheel::DropShowEvent(id, w.show, w.xBlock != 0)) {
            if (!flag && value != 0.0f) {
                const LONG n = InterlockedIncrement(&w.xDropped);
                if (n <= 50 || n % 100 == 0) {
                    if (k == kBolts) {
                        // [PROBE: BOLTWHEEL] gate=manual_reload  site=HEADSET  log-only=yes
                        LOG("boltwheel: [BOLTWHEEL] X BLOCKED #%ld (thread %lu): the real X's show action 310 (value %.2f) was dropped "
                            "before the bolt wheel's input handler - X does not open the bolt wheel (the bolt holster hold does).",
                            static_cast<long>(n), GetCurrentThreadId(), value);
                    } else {
                        // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=HEADSET  log-only=yes
                        LOG("boltwheel: [ARROWWHEEL] X BLOCKED #%ld (thread %lu): the real X's show action 288 (value %.2f) was dropped "
                            "before the arrow wheel's input handler - X does not open the arrow wheel (the quiver hold does).",
                            static_cast<long>(n), GetCurrentThreadId(), value);
                    }
                }
            }
            return;
        }
        if (w.onInputTrue) { w.onInputTrue(target, id, value, flag); }
        return;
    }
    // Not expected (only the two patched vtables reach here): run an original - both are +575830 by their bytes.
    const OnInputFn f = g_w[kBolts].onInputTrue ? g_w[kBolts].onInputTrue : g_w[kArrows].onInputTrue;
    if (f) { f(target, id, value, flag); }
}

// COST per wheel Update (one object per wheel, once a frame while the HUD updates it): one guarded pointer read (which wheel), the
// original, two volatile loads + a tick read; while the want is fresh, one guarded byte read (and, once per hold, two guarded pointer
// reads + the game's own OnInput).
long long __fastcall UpdateHook(void* self, long long a2, int a3)
{
    const uintptr_t obj = reinterpret_cast<uintptr_t>(self);
    uintptr_t vt0 = 0;
    ReadPtr(obj, vt0);
    int k = -1;
    if (vt0 && vt0 == g_w[kBolts].mainVt) { k = kBolts; }
    else if (vt0 && vt0 == g_w[kArrows].mainVt) { k = kArrows; }
    if (k < 0) {
        // Only objects whose vtable we patched reach here, so this is not expected: run an original (both are +575C70 by their bytes).
        const UpdateFn f = g_w[kBolts].updateTrue ? g_w[kBolts].updateTrue : g_w[kArrows].updateTrue;
        if (InterlockedIncrement(&g_unknownObj) == 1) {
            // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw,manual_reload  site=HEADSET  log-only=yes
            LOG("boltwheel: [ARROWWHEEL] *** UNKNOWN WHEEL OBJECT *** %p (vtable %p is neither patched wheel's) reached the Update hook - "
                "ran the original, sent nothing.", self, reinterpret_cast<void*>(vt0));
        }
        return f ? f(self, a2, a3) : 0;
    }
    Wheel& w = g_w[k];
    const long long r = w.updateTrue(self, a2, a3);
    if (InterlockedCompareExchange(&w.firstUpdate, 1, 0) == 0) {
        if (k == kBolts) {
            // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
            LOG("boltwheel: [BOLTWHEEL] FIRST UPDATE of the bolt wheel through our slot (logged unconditionally) - object %p, thread %lu.",
                self, GetCurrentThreadId());
        } else {
            // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=FLAT  log-only=yes
            LOG("boltwheel: [ARROWWHEEL] FIRST UPDATE of the arrow wheel through our slot (logged unconditionally) - object %p, thread "
                "%lu.", self, GetCurrentThreadId());
        }
    }
    const bool fresh = WantFreshNow(w);
    if (!fresh) {
        if (w.forcing) {
            InterlockedExchange(&w.forcing, 0);
            const LONG64 age = static_cast<LONG64>(GetTickCount64()) - w.wantMs;
            if (w.want) {
                if (k == kBolts) {
                    // [PROBE: BOLTWHEEL] gate=manual_reload  site=HEADSET  log-only=yes
                    LOG("boltwheel: [BOLTWHEEL] WANT EXPIRED (stale %lld ms, limit %u) - the show action 310 is released; the pad thread "
                        "stopped stamping the hold.", static_cast<long long>(age), boltwheel::kWantFreshMs);
                } else {
                    // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=HEADSET  log-only=yes
                    LOG("boltwheel: [ARROWWHEEL] WANT EXPIRED (stale %lld ms, limit %u) - the show action 288 is released; the pad "
                        "thread stopped stamping the hold.", static_cast<long long>(age), boltwheel::kWantFreshMs);
                }
            }
        }
        InterlockedExchange(&w.sent, 0);
        return r;
    }
    unsigned char open = 0;
    if (!ReadByte(obj + kOpenOff, open)) { return r; }
    if (!boltwheel::OpenEventDue(true, open != 0, w.sent != 0)) { return r; }
    InterlockedExchange(&w.sent, 1);   // once per hold, whatever happens below (plan B3)
    const uintptr_t target = obj + kTargetOff;
    uintptr_t vt = 0, fn = 0;
    if (!ReadPtr(target, vt) || vt != w.targetVt || !ReadPtr(vt + 8 * kOnInputSlot, fn) || !fn) {
        if (InterlockedIncrement(&w.badTarget) == 1) {
            if (k == kBolts) {
                // [PROBE: BOLTWHEEL] gate=manual_reload  site=HEADSET  log-only=yes
                LOG("boltwheel: [BOLTWHEEL] OPEN EVENT REFUSED - the object's input target vtable %p is not the bolt wheel's %p (or its slot "
                    "6 is unreadable); nothing sent.", reinterpret_cast<void*>(vt), reinterpret_cast<void*>(w.targetVt));
            } else {
                // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=HEADSET  log-only=yes
                LOG("boltwheel: [ARROWWHEEL] OPEN EVENT REFUSED - the object's input target vtable %p is not the arrow wheel's %p (or its "
                    "slot 6 is unreadable); nothing sent.", reinterpret_cast<void*>(vt), reinterpret_cast<void*>(w.targetVt));
            }
        }
        return r;
    }
    // With the X block in, slot 6 is OnInputHook (which would drop this very event): our open goes to the original directly.
    const OnInputFn openFn = w.onInputTrue ? w.onInputTrue : reinterpret_cast<OnInputFn>(fn);
    const bool ok = SendOpen(target, openFn, w.show);
    const LONG n = InterlockedIncrement(&w.opens);
    unsigned char after = 0;
    ReadByte(obj + kOpenOff, after);
    if (k == kBolts) {
        // [PROBE: BOLTWHEEL] gate=manual_reload  site=HEADSET  log-only=yes
        LOG("boltwheel: [BOLTWHEEL] OPEN EVENT SENT #%ld (thread %lu): the wheel was closed, the hold is down -> its own input handler "
            "with the show action 310 = 1.0 %s; the wheel's open flag now %u (0 = the game refused it - no crossbow held / a menu).",
            static_cast<long>(n), GetCurrentThreadId(), ok ? "returned" : "*** FAULTED ***", static_cast<unsigned>(after));
    } else {
        // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=HEADSET  log-only=yes
        LOG("boltwheel: [ARROWWHEEL] OPEN EVENT SENT #%ld (thread %lu): the wheel was closed, the quiver hold is down -> its own input "
            "handler with the show action 288 = 1.0 %s; the wheel's open flag now %u (0 = the game refused it - no bow held / a menu).",
            static_cast<long>(n), GetCurrentThreadId(), ok ? "returned" : "*** FAULTED ***", static_cast<unsigned>(after));
    }
    return r;
}

bool HeadIs(uintptr_t a, const unsigned char* want, size_t n, unsigned char* got)
{
    __try { memcpy(got, reinterpret_cast<const void*>(a), n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return memcmp(got, want, n) == 0;
}

void Refuse(int k, const char* why)
{
    g_w[k].state = -1;
    if (k == kBolts) {
        // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("boltwheel: [BOLTWHEEL] REFUSED - %s. The bolt holster hold opens no wheel this launch (taking a bolt works as before).", why);
    } else {
        // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=FLAT  log-only=yes
        LOG("boltwheel: [ARROWWHEEL] REFUSED - %s. The quiver hold opens no arrow wheel this launch (taking an arrow and the X-held arrow "
            "wheel work as before).", why);
    }
}

// The lookup detour, once for both wheels. False = not live (g_detourWhy says why; -1 is final, 0 = the gamedll is not mapped yet).
bool DetourLive(uintptr_t gd)
{
    if (g_detour != 0) { return g_detour == 1; }
    const uintptr_t at = gd + DLVR_BW_GD(kLookupRva);
    unsigned char got[24] = {};
    if (!HeadIs(at, kLookupHead, sizeof(kLookupHead), got)) {
        _snprintf_s(g_detourWhy, sizeof(g_detourWhy), _TRUNCATE,
                    "the input lookup's first 16 bytes at gamedll +%zX (%02X %02X %02X %02X %02X %02X %02X %02X ...) are not this build's "
                    "- an unknown gamedll build", static_cast<size_t>(at - gd), got[0], got[1], got[2], got[3], got[4], got[5], got[6],
                    got[7]);
        g_detour = -1;
        return false;
    }
    int attempts = 0;
    if (!InstallSingleGameDetour(at, kLookupHead, sizeof(kLookupHead), reinterpret_cast<void*>(&LookupHook),
                                 reinterpret_cast<void**>(&g_lookupTrue), "quiver wheel input lookup", &attempts)) {
        _snprintf_s(g_detourWhy, sizeof(g_detourWhy), _TRUNCATE, "the input lookup detour write was refused after %d attempt(s)", attempts);
        g_detour = -1;
        return false;
    }
    g_detourAt = at;
    g_detourAttempts = attempts;
    g_detour = 1;
    return true;
}

// The X block for one wheel (2026-11-11), only once its quiver hooks are live (the quiver hold is then the way to open it): the input
// target vtable's slot 6 -> OnInputHook, accepted only if it holds OnInputGameController's bytes. A refusal leaves X opening the wheel
// exactly as before (named). Logged unconditionally either way.
void InstallXBlock(int k, uintptr_t gd)
{
    Wheel& w = g_w[k];
    const uintptr_t slot = w.targetVt + 8 * kOnInputSlot;
    uintptr_t have = 0;
    unsigned char got[24] = {};
    const char* why = nullptr;
    if (!ReadPtr(slot, have) || !have) {
        why = "the input target's slot 6 is unreadable";
    } else if (!HeadIs(have, kOnInputHead, sizeof(kOnInputHead), got)) {
        why = "the input target's slot 6 target's first 24 bytes are not the wheel input handler's";   // OnInputGameController +575830
    }
    DWORD old = 0;
    if (!why) {
        w.onInputTrue = reinterpret_cast<OnInputFn>(have);
        MemoryBarrier();
        // [LOADBEARING: X block] (manual_reload / bow_manual_nock) HudBoltSelection + HudArrowsSelection input target vtable slot 6
        //      (OURS +14D59A8 / the arrows' locator-88 vtable, slot -> +575830) -> OnInputHook. Without it the real X opens the wheel.
        if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
            why = "VirtualProtect on the input target's slot 6 failed";
        }
    }
    if (!why) {
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&OnInputHook));
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
        uintptr_t back = 0;
        ReadPtr(slot, back);
        if (back != reinterpret_cast<uintptr_t>(&OnInputHook)) { why = "the slot did not read back as ours"; }
    }
    InterlockedExchange(&w.xBlock, why ? 0 : 1);
    if (k == kBolts) {
        // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("boltwheel: [BOLTWHEEL] X BLOCK %s (logged unconditionally) - input target slot 6 at gamedll +%zX held gamedll +%zX%s%s. %s",
            why ? "*** REFUSED ***" : "ARMED", static_cast<size_t>(slot - gd), static_cast<size_t>(have ? have - gd : 0), why ? ": " : "",
            why ? why : "", why ? "The real X still opens the bolt wheel." : "The real X no longer opens the bolt wheel; its show action "
            "reads 0 without the bolt holster hold.");
    } else {
        // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=FLAT  log-only=yes
        LOG("boltwheel: [ARROWWHEEL] X BLOCK %s (logged unconditionally) - input target slot 6 at gamedll +%zX held gamedll +%zX%s%s. %s",
            why ? "*** REFUSED ***" : "ARMED", static_cast<size_t>(slot - gd), static_cast<size_t>(have ? have - gd : 0), why ? ": " : "",
            why ? why : "", why ? "The real X still opens the arrow wheel." : "The real X no longer opens the arrow wheel; its show "
            "action reads 0 without the quiver hold.");
    }
}

// The reload prompt (2026-11-11, Jay: "Reload prompt only"): the picker's reload candidate immediate set to PromptImmWanted(manual_reload)
// - checked against its 13 bytes before every write, re-applied when manual_reload changes. Steam + EGL (buildrva, kind imm).
// COST: camhook's 1 Hz caller - one bool compare while nothing changed; on a change one 13-byte guarded read + one byte write.
int g_promptState = 0;            // 0 not tried, 1 live, -1 refused (final)
int g_promptHave = -1;            // the byte last written / found (-1 = none yet)
void PromptTick(uintptr_t gd)
{
    if (g_promptState == -1) { return; }
    const unsigned char want = boltwheel::PromptImmWanted(ManualReload);
    if (g_promptHave == want) { return; }
    const uintptr_t imm = gd + DLVR_BW_GD(kPromptImmRva);
    unsigned char got[sizeof(kPromptCtx)] = {};
    bool same = false;
    __try {
        memcpy(got, reinterpret_cast<const void*>(imm - kPromptCtxBefore), sizeof(got));
        same = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
    if (same) {
        for (size_t i = 0; i < sizeof(kPromptCtx); ++i) {
            if (i != kPromptCtxBefore && got[i] != kPromptCtx[i]) { same = false; }
        }
        if (!boltwheel::PromptSiteOk(got[kPromptCtxBefore])) { same = false; }
    }
    if (!same) {
        g_promptState = -1;
        // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("boltwheel: [BOLTWHEEL] RELOAD PROMPT REFUSED (logged unconditionally) - the 13 bytes around gamedll +%zX are not the "
            "prompt picker's reload candidate store (%02X %02X %02X %02X %02X %02X [%02X] %02X %02X %02X %02X %02X %02X): the game's "
            "\"X Reload\" prompt is left as it is.", static_cast<size_t>(imm - gd), got[0], got[1], got[2], got[3], got[4], got[5],
            got[6], got[7], got[8], got[9], got[10], got[11], got[12]);
        return;
    }
    const unsigned char was = got[kPromptCtxBefore];
    bool wrote = was == want;
    if (!wrote) {
        DWORD old = 0;
        // [LOADBEARING: reload prompt] (manual_reload) gamedll +5A8F44 = the immediate of the reload candidate store in HudReloadNotifyDI's
        //      picker +5A8CE0 (ida/rn6). 0 = no reload candidate = no "X Reload" prompt; 1 = the game's.
        if (VirtualProtect(reinterpret_cast<void*>(imm), 1, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<volatile unsigned char*>(imm) = want;
            DWORD unused = 0;
            VirtualProtect(reinterpret_cast<void*>(imm), 1, old, &unused);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(imm), 1);
            wrote = *reinterpret_cast<volatile unsigned char*>(imm) == want;
        }
    }
    g_promptHave = wrote ? want : -1;
    if (!wrote) { g_promptState = -1; }
    else { g_promptState = 1; }
    // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
    LOG("boltwheel: [BOLTWHEEL] RELOAD PROMPT %s (logged unconditionally) - manual_reload=%d: the reload candidate byte at gamedll +%zX "
        "%02X -> %02X%s.", wrote ? (want == boltwheel::kPromptImmNone ? "HIDDEN" : "SHOWN (the game's)") : "*** WRITE FAILED ***",
        ManualReload ? 1 : 0, static_cast<size_t>(imm - gd), was, want,
        wrote ? (want == boltwheel::kPromptImmNone ? " - the \"X Reload\" prompt never shows; heal / repair / fuel / trap prompts do"
                                                   : " - the \"X Reload\" prompt shows as in the game")
              : " - the prompt is left as the game has it");
}

// The hint / grapple prompt patches (2026-11-11 (5), Jay: "GRAPPLE PROMPT/HOLD X TO CHANGE ARROWS" -> "Yes"; boltwheel_logic.h
// kPatches, ida/hp1-hp10): each site written to ours while its gate is on, back to the game's when it goes off; checked (13-24 bytes,
// PatchSiteState) before every write, a site that is neither refused for good (named). Each write is one 1- or 2-byte store; every
// half-state of a jnz / jz pair is valid code (only the jz made a jmp = always skip; only the jnz nopped = the game's second test).
// COST: camhook's 1 Hz caller - five bool compares while nothing changes; on a change a guarded read + one store per site.
int g_patchState[boltwheel::kPatchCount] = {};    // 0 not tried, 1 live, -1 refused (final)
int g_patchHave[boltwheel::kPatchCount] = { -1, -1, -1, -1, -1 };   // 1 game / 2 ours last found or written, -1 none yet
bool PatchGate(int gate)
{
    return gate == 0 ? (BowManualNock && BowManualDraw) : gate == 1 ? ManualReload : EnemyPushGrab;
}
const char* PatchGateName(int gate)
{
    return gate == 0 ? "bow_manual_nock + bow_manual_draw" : gate == 1 ? "manual_reload" : "enemy_push_grab";
}
void PatchTick(uintptr_t gd)
{
    for (int i = 0; i < boltwheel::kPatchCount; ++i) {
        const boltwheel::PatchSite& s = boltwheel::kPatches[i];
        if (g_patchState[i] == -1) { continue; }
        const bool on = PatchGate(s.gate);
        const int want = on ? 2 : 1;
        if (g_patchHave[i] == want) { continue; }
        const uintptr_t at = gd + DLVR_BW_GD(s.rva);
        unsigned char got[24] = {};
        bool read = false;
        __try {
            memcpy(got, reinterpret_cast<const void*>(at - s.at), s.ctxLen);
            read = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { read = false; }
        const int was = read ? boltwheel::PatchSiteState(s, got) : 0;
        if (was == 0) {
            g_patchState[i] = -1;
            // [PROBE: PROMPTHIDE] gate=bow_manual_nock,bow_manual_draw,manual_reload,enemy_push_grab  site=FLAT  log-only=yes
            LOG("boltwheel: [PROMPTHIDE] %s REFUSED (logged unconditionally) - the %u bytes around gamedll +%zX are neither the game's "
                "nor ours (%s; patched bytes %02X %02X): left as they are.", s.name, static_cast<unsigned>(s.ctxLen),
                static_cast<size_t>(at - gd), read ? "read" : "UNREADABLE", got[s.at], s.len > 1 ? got[s.at + 1] : 0);
            continue;
        }
        bool ok = was == want;
        if (!ok) {
            const unsigned char* src = want == 2 ? s.ours : s.game;
            DWORD old = 0;
            // [LOADBEARING: prompt hide] (bow_manual_nock / manual_reload / enemy_push_grab) the hint gate jnz / jz of TutorialManager's
            //      bow / crossbow update +F9F6A0 (+F9FBB6 / +F9FBCD / +F9FD72 / +F9FD89) and HudNageWaza's show byte +57E2CE (ida/hp5,
            //      hp10). Ours = "Hold X to change Arrows / Bolts" and the Grapple prompt never show; the game's = they do.
            if (VirtualProtect(reinterpret_cast<void*>(at), s.len, PAGE_EXECUTE_READWRITE, &old)) {
                if (s.len == 2) {
                    *reinterpret_cast<volatile unsigned short*>(at) = static_cast<unsigned short>(src[0] | (src[1] << 8));
                } else {
                    *reinterpret_cast<volatile unsigned char*>(at) = src[0];
                }
                DWORD unused = 0;
                VirtualProtect(reinterpret_cast<void*>(at), s.len, old, &unused);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), s.len);
                ok = memcmp(reinterpret_cast<const void*>(at), src, s.len) == 0;
            }
        }
        g_patchHave[i] = ok ? want : -1;
        g_patchState[i] = ok ? 1 : -1;
        // [PROBE: PROMPTHIDE] gate=bow_manual_nock,bow_manual_draw,manual_reload,enemy_push_grab  site=FLAT  log-only=yes
        LOG("boltwheel: [PROMPTHIDE] %s %s (logged unconditionally) - %s=%d: gamedll +%zX %s -> %s.", s.name,
            !ok ? "*** WRITE FAILED ***" : (want == 2 ? "HIDDEN" : "SHOWN (the game's)"), PatchGateName(s.gate), on ? 1 : 0,
            static_cast<size_t>(at - gd), was == 1 ? "game" : "ours", want == 2 ? "ours" : "game");
    }
}

// One wheel's Update slot, with the detour live. A refusal leaves the detour in: it forces nothing without a want, and the drive
// never wants without the wheel's Live().
void InstallSlot(int k, uintptr_t gd)
{
    Wheel& w = g_w[k];
    unsigned char got[24] = {};
    int stMain = 0, stTarget = 0;
    const uintptr_t vt = RttiFindVtable(gd, w.cls, 0, &stMain);
    const uintptr_t tvt = RttiFindVtable(gd, w.cls, static_cast<uint32_t>(kTargetOff), &stTarget);
    if (!vt || !tvt) {
        char why[200];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the %s wheel's vtables were not found by name (main stage %d, input target stage %d; "
                    "0 no name, 1 no locator, 2 no vtable, -1 faulted)", k == kBolts ? "bolt" : "arrow", stMain, stTarget);
        Refuse(k, why);
        return;
    }
    const uintptr_t slot = vt + 8 * kUpdateSlot;
    uintptr_t have = 0;
    if (!ReadPtr(slot, have) || !have || !HeadIs(have, kUpdateHead, sizeof(kUpdateHead), got)) {
        char why[200];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "the %s wheel's Update slot at gamedll +%zX holds gamedll +%zX, whose first 24 bytes "
                    "are not the wheel Update both known builds share", k == kBolts ? "bolt" : "arrow", static_cast<size_t>(slot - gd),
                    static_cast<size_t>(have - gd));
        Refuse(k, why);
        return;
    }
    // Set before the slot is live: UpdateHook tells the wheels apart by mainVt and calls updateTrue.
    w.updateTrue = reinterpret_cast<UpdateFn>(have);
    w.targetVt = tvt;
    w.mainVt = vt;
    MemoryBarrier();
    DWORD old = 0;
    // [LOADBEARING: bolt wheel hooks] (manual_reload) HudBoltSelection main vtable slot 178 -> UpdateHook (OURS vtable +14D5A08, slot
    //      +575C70). Without it the bolt holster hold sends no open event: no bolt wheel.
    // [LOADBEARING: arrow wheel hooks] (bow_manual_nock) HudArrowsSelection main vtable slot 178 -> UpdateHook (OURS vtable +14CA538,
    //      slot +575C70 - wh4 / awh1). Without it the quiver hold sends no open event: no arrow wheel from the quiver.
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) {
        char why[120];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "VirtualProtect on the Update slot failed (error %lu)", GetLastError());
        Refuse(k, why);
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<PVOID>(&UpdateHook));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &unused);
    uintptr_t back = 0;
    ReadPtr(slot, back);
    w.state = back == reinterpret_cast<uintptr_t>(&UpdateHook) ? 1 : -1;
    if (k == kBolts) {
        // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
        LOG("boltwheel: [BOLTWHEEL] %s (logged unconditionally) - input lookup detour at gamedll +%zX (16 bytes, %d attempt(s)); bolt wheel "
            "Update slot %zu at gamedll +%zX (main vtable +%zX, was +%zX, read back %s); input target vtable +%zX. Holding the left grip at "
            "the bolt holster opens the game's bolt wheel; X is never sent.",
            w.state == 1 ? "ARMED" : "*** NOT ARMED ***", static_cast<size_t>(g_detourAt - gd), g_detourAttempts, kUpdateSlot,
            static_cast<size_t>(slot - gd), static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd),
            w.state == 1 ? "OK" : "NOT OUR FUNCTION", static_cast<size_t>(tvt - gd));
    } else {
        // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=FLAT  log-only=yes
        LOG("boltwheel: [ARROWWHEEL] %s (logged unconditionally) - input lookup detour at gamedll +%zX (16 bytes, %d attempt(s)); arrow "
            "wheel Update slot %zu at gamedll +%zX (main vtable +%zX, was +%zX, read back %s); input target vtable +%zX. Holding RT at the "
            "right-shoulder quiver with the bow out opens the game's arrow wheel; X is never sent.",
            w.state == 1 ? "ARMED" : "*** NOT ARMED ***", static_cast<size_t>(g_detourAt - gd), g_detourAttempts, kUpdateSlot,
            static_cast<size_t>(slot - gd), static_cast<size_t>(vt - gd), static_cast<size_t>(have - gd),
            w.state == 1 ? "OK" : "NOT OUR FUNCTION", static_cast<size_t>(tvt - gd));
    }
    if (w.state == 1) { InstallXBlock(k, gd); }
}

void SetWant(int k, bool want)
{
    Wheel& w = g_w[k];
    InterlockedExchange64(&w.wantMs, static_cast<LONG64>(GetTickCount64()));
    InterlockedExchange(&w.want, want ? 1 : 0);
}

}  // namespace

void BoltWheelInstall()
{
    // The reload prompt follows manual_reload both ways (on: hidden, off: the game's), so it runs before the wheels' early returns.
    if (const uintptr_t gdp = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"))) {
        PromptTick(gdp);
        PatchTick(gdp);   // the "Hold X to change Arrows / Bolts" hints and the Grapple prompt, each by its own gate
    }
    const bool gate[2] = { ManualReload, BowManualNock && BowManualDraw };
    bool any = false;
    for (int k = 0; k < 2; ++k) {
        if (g_w[k].state != 0) { continue; }
        if (gate[k]) { any = true; continue; }
        if (g_w[k].saidOff) { continue; }
        g_w[k].saidOff = true;
        if (k == kBolts) {
            // [PROBE: BOLTWHEEL] gate=manual_reload  site=FLAT  log-only=yes
            LOG("boltwheel: [BOLTWHEEL] NOT INSTALLED - manual_reload=0 (the bolt holster exists only with it); nothing patched.");
        } else {
            // [PROBE: ARROWWHEEL] gate=bow_manual_nock,bow_manual_draw  site=FLAT  log-only=yes
            LOG("boltwheel: [ARROWWHEEL] NOT INSTALLED - bow_manual_nock=%d bow_manual_draw=%d (the quiver take exists only with both); "
                "nothing patched for the arrow wheel.", BowManualNock ? 1 : 0, BowManualDraw ? 1 : 0);
        }
    }
    if (!any) { return; }
    const uintptr_t gd = reinterpret_cast<uintptr_t>(GetModuleHandleA("gamedll_x64_rwdi.dll"));
    if (!gd) { return; }   // not mapped yet - the 1 Hz caller retries
    // (1) the lookup detour FIRST.
    const bool live = DetourLive(gd);
    for (int k = 0; k < 2; ++k) {
        if (g_w[k].state != 0 || !gate[k]) { continue; }
        if (!live) { Refuse(k, g_detourWhy); continue; }
        InstallSlot(k, gd);   // (2) the slot, only with the detour live
    }
}

void BoltWheelSetWant(bool want)
{
    // The bolt drive's want (rides manual_reload - no key of its own); read by LookupHook / UpdateHook on the game's threads.
    SetWant(kBolts, want);
}

bool BoltWheelLive() { return g_w[kBolts].state == 1; }

long BoltWheelOpensSent() { return static_cast<long>(g_w[kBolts].opens); }

void ArrowWheelSetWant(bool want)
{
    // The quiver drive's want (rides bow_manual_nock + bow_manual_draw - no key of its own).
    SetWant(kArrows, want);
}

bool ArrowWheelLive() { return g_w[kArrows].state == 1; }

bool ArrowWheelXBlocked() { return g_w[kArrows].xBlock != 0; }

long ArrowWheelOpensSent() { return static_cast<long>(g_w[kArrows].opens); }

}  // namespace dlvr
