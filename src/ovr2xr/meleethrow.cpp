// Melee throw (VR gesture) - docs/superpowers/specs/2026-09-30-melee-throw-design.md. Stage 1: the state machine and
// the game's own input-handler calls; the game's animations are NOT touched (spec §7). The decision is
// meleethrow_logic.h (unit-tested); this file reads the inputs, makes the calls it asks for, and logs [THROW].
#include "meleethrow.h"

#include "bodygrab.h"       // BodyGrabHandActive - no melee throw while the right hand holds a body
#include "breakout.h"       // BreakoutPlayerGrabbed - no melee throw while a zombie holds the player
#include "calibscreen.h"    // CalibScreenActive - no melee throw while the calibration screen is up
#include "overlay.h"        // OverlayVisible - no melee throw while the settings menu is up
#include "boneblend.h"
#include "controllers.h"
#include "holster_logic.h"
#include "meleethrow_logic.h"
#include "swing.h"
#include "swing_logic.h"
#include "throwable.h"      // ThrowableBusy - ruling R24

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../common/buildrva.h"   // the throw's EGL rvas on the Epic build (tier 2)
#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

// [DEP: throw_enable] required-for=the VR melee throw (ThrowEnable) - spec 2026-09-30-melee-throw-design §5. Also gates the
//      FAST RELEASE (2026-11-11, Jay "a - release on let go, every weapon"): the weapon leaves the hand at the let-go
//      (FastArm / FastFrame, [THROWFAST]) - no key of its own.
//      tree=INERT at 0: ThrowFrame returns false at its first line and nothing is called in the game.
//      HEADSET: runs inside the swing driver (melee_swing_mode >= 1) and reads the detector's velocity; needs
//      melee_r2_suppress=1, the right grip + head poses, the VR trigger, and RightHandItemKind (the [ITEM] capture, i.e.
//      the rig's three master enables). ThrowTick refuses by name on the settings.
bool  ThrowEnable = true;   // DEFAULT ON 2026-10-01 (Jay: Tier A ships on)
// [DEP: throw_height_y] required-for=the READY line (ThrowHeightY), metres, head-yaw frame +Y up. [UNKNOWN] starting
//      guess -0.10 - calibrate from [THROW] READYING's hand y column. tree=INERT at throw_enable=0.
float ThrowHeightY = -0.10f;
// [DEP: throw_trig_press] [DEP: throw_trig_release] required-for=the trigger hysteresis (ThrowTrigPress /
//      ThrowTrigRelease). tree=INERT at throw_enable=0.
float ThrowTrigPress = 0.6f;
float ThrowTrigRelease = 0.3f;
// [DEP: throw_release_speed] required-for=THROW vs CANCEL at the let-go (ThrowReleaseSpeed), m/s forward. [UNKNOWN]
//      starting guess 2.0 - calibrate from [THROW] THROW/CANCEL's forward column. tree=INERT at throw_enable=0.
float ThrowReleaseSpeed = 2.0f;
// [DEP: throw_swing_holdoff_ms] required-for=the swing mute after a throw (ThrowSwingHoldoffMs). tree=INERT at
//      throw_enable=0.
float ThrowSwingHoldoffMs = 300.0f;

namespace {

// gamedll RVAs, md5 97046bba5eb07a918acff82a394e8c8b (INV-313; the same build as swing.cpp's table).
constexpr uintptr_t kInputRva       = 0xDB3720;  // melee input handler (this, code, value) - INV-49 / INV-313
constexpr uintptr_t kCanReadRva     = 0xDC6A30;  // mac CanReadRightInput by shape (this) -> bool - INV-313
constexpr uintptr_t kRightAttackRva = 0xDC65F0;  // mac OnRightAttack by shape (this) -> char - INV-313
constexpr int       kReadyCode      = 276;       // the double-tap code (INV-313)
constexpr size_t    kFlagOff        = 788;       // BYTE: throw ready (mac IsThrowAiming) - INV-313
constexpr size_t    kStateOff       = 0x110;     // int: -1 = idle (swing.cpp B1Frame)
constexpr size_t    kNextHitOff     = 284;       // int: next hit type, -1 = none (INV-314, sub_180DC65F0)
// Fast release (Jay 2026-11-11 "a - release on let go, every weapon"; meleethrow_logic.h FAST RELEASE; OUR gamedll 97046bba,
// re_analysis/throw7_delay_ours.c). Controller fields, not rvas.
constexpr size_t    kHitStampOff    = 716;       // float: the queued hit's stamp (+DB3DC0 at OnRightAttack); -1.0 = none
constexpr size_t    kDelayFlagOff   = 804;       // BYTE: 0 -> the start delay is +904[type] (sub_180DBDC80)
constexpr size_t    kDelayTableOff  = 904;       // float[type]: the start delay, seconds
constexpr size_t    kEventTableOff  = 1088;      // float[5] per state: event times, seconds (< 0 = unused)
constexpr size_t    kCounterOff     = 1608;      // int: release / event counter, 0 = not released
constexpr size_t    kReleaseByteOff = 1626;      // BYTE: the game's release request (slot 10 sets it, slot 16 runs it)
constexpr size_t    kLaunchOff      = 324;       // float[3]: the launch point, filled by sub_180DC3970 in state 18/19
constexpr float     kFastMarginS    = 0.01f;     // start just past the delay; the drop window is delay + 0.15 s
constexpr float     kMaxVelAgeMs    = 50.0f;     // spec §3a
constexpr ULONGLONG kReleaseViewMs  = 1500;      // [VIEWBODY] throw/release bucket, spec §3a

using InputFn   = void(__fastcall*)(void* self, int code, float value);
using CanReadFn = bool(__fastcall*)(void* self);
using AttackFn  = char(__fastcall*)(void* self);

// Written once by ThrowTick (1 Hz thread) BEFORE g_armed is set to 1; read by ThrowFrame only after it reads 1.
InputFn   g_input = nullptr;
CanReadFn g_canRead = nullptr;
AttackFn  g_attack = nullptr;
volatile LONG g_armed = 0;
LONG g_lastArm = -1;               // ThrowTick only: the state last logged

// Game thread only (inside swing's driver gate).
mthrow::Machine g_m;
bool g_firstFrameSaid = false;
long g_nReady = 0, g_nThrow = 0, g_nCancel = 0, g_nRefused = 0, g_nLost = 0;
long g_muteFrames = 0;             // frames the swing was muted since the last READYING
mthrow::FastTracker g_fast;        // fast release: armed by a THROW the game accepted
double g_fastLetGoMs = 0.0;        // tMs of that THROW
long   g_fastN = 0;                // THROWs the fast release was armed for

// Read by ThrowViewPhase on the render thread.
volatile LONG      g_viewPhase = 0;          // 0 none, 1 ready, 2 release
volatile ULONGLONG g_viewReleaseEnd = 0;
volatile ULONGLONG g_lastFrameTick = 0;

bool FlagOf(void* self)
{
    return *reinterpret_cast<const volatile unsigned char*>(static_cast<const char*>(self) + kFlagOff) != 0;
}

int StateOf(void* self)
{
    return *reinterpret_cast<const volatile int*>(static_cast<const char*>(self) + kStateOff);
}

int NextHitOf(void* self)
{
    return *reinterpret_cast<const volatile int*>(static_cast<const char*>(self) + kNextHitOff);
}

// ONE press at a time: a 276 within 0.30 s of an earlier one readies on the FIRST call, and a second call would then
// toggle it OFF (INV-313, spec §3a).
int SendReady(void* self)
{
    int sent = 0;
    for (int i = 0; i < 2 && !FlagOf(self); ++i) {
        g_input(self, kReadyCode, 1.0f);
        ++sent;
    }
    return sent;
}

void SetViewPhase(LONG ph)
{
    if (g_viewPhase != ph) { InterlockedExchange(&g_viewPhase, ph); }
}

template <typename T>
volatile T& Field(void* self, size_t off)
{
    return *reinterpret_cast<volatile T*>(static_cast<char*>(self) + off);
}

// Fast release, step 1, at a THROW the game accepted (OnRightAttack CALLED, +788 cleared): back-date the queued hit's stamp
// so the throw starts on the next update, and arm the tracker that sets the release byte once it has. One line per THROW,
// logged unconditionally (it names why it refused, if it did). Game thread, once per throw.
void FastArm(void* self, double tMs)
{
    ++g_fastN;
    const int type = NextHitOf(self);
    if (!mthrow::FastIsThrowState(type)) {
        // [PROBE: THROWFAST] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROWFAST] #%ld NOT ARMED - the queued hit is type %d, not a throw (18 / 19) | state %d | the game's "
            "own timing applies", g_fastN, type, StateOf(self));
        return;
    }
    const float stamp = Field<float>(self, kHitStampOff);
    const int delayFlag = Field<unsigned char>(self, kDelayFlagOff);
    const float delay = Field<float>(self, kDelayTableOff + 4u * static_cast<size_t>(type));
    float ev[5];
    for (int i = 0; i < 5; ++i) {
        ev[i] = Field<float>(self, kEventTableOff + 4u * static_cast<size_t>(i + 5 * type));
    }
    float newStamp = stamp;
    const char* what = "START WAIT SKIPPED";
    if (delayFlag != 0) {
        what = "start wait left alone (+804 set: the game's delay is not the table value)";
    } else if (mthrow::FastBackdate(stamp, delay, kFastMarginS, &newStamp)) {
        Field<float>(self, kHitStampOff) = newStamp;
    } else {
        what = "start wait NOT SKIPPED - the stamp or delay is not a sane value, nothing written";
        newStamp = stamp;
    }
    g_fast.Arm(tMs);
    g_fastLetGoMs = tMs;
    // [PROBE: THROWFAST] gate=throw_enable  site=HEADSET  log-only=no
    LOG("meleethrow: [THROWFAST] #%ld ARMED | throw type %d | game start wait %.3f s (+804 %d) | event times %.3f %.3f %.3f "
        "%.3f %.3f s | stamp %.3f -> %.3f | %s | state now %d",
        g_fastN, type, delay, delayFlag, ev[0], ev[1], ev[2], ev[3], ev[4], stamp, newStamp, what, StateOf(self));
}

// Fast release, step 2, every frame while armed (COST: a few field reads, only between a THROW and its release - nothing
// otherwise): once the throw state is running and the game has filled its launch point, set the game's own release byte.
// We run after the game's slot-16 update (swing.cpp SwingUpdateThunk), so the game's next update runs the release itself.
void FastFrame(void* self, double tMs)
{
    if (!g_fast.active()) { return; }
    mthrow::FastObs o;
    o.state = StateOf(self);
    o.nextHit = NextHitOf(self);
    o.count1608 = Field<int>(self, kCounterOff);
    o.rel1626 = Field<unsigned char>(self, kReleaseByteOff) != 0;
    const float lx = Field<float>(self, kLaunchOff);
    const float ly = Field<float>(self, kLaunchOff + 4);
    const float lz = Field<float>(self, kLaunchOff + 8);
    o.launchSet = lx != 0.0f || ly != 0.0f || lz != 0.0f;
    const mthrow::FastStep st = g_fast.Step(tMs, o);
    if (st.writeRelease) { Field<unsigned char>(self, kReleaseByteOff) = 1; }
    if (st.ev == mthrow::FastEv::None) { return; }
    // [PROBE: THROWFAST] gate=throw_enable  site=HEADSET  log-only=no
    LOG("meleethrow: [THROWFAST] #%ld %s | %.0f ms after the let-go | state %d next hit %d | +1608 %d +1626 %d | launch "
        "point %s%s",
        g_fastN, mthrow::FastEvName(st.ev), tMs - g_fastLetGoMs,
        o.state, o.nextHit, o.count1608, o.rel1626 ? 1 : 0, o.launchSet ? "set" : "not set",
        st.writeRelease ? " | RELEASE BYTE SET - the game releases on its next update" : "");
}

}  // namespace

bool ThrowFrame(void* self, const float gripPos[3], const float headPos[3], const float headQ[4], bool poseOk,
                bool velValid, float vx, float vy, float vz, double tMs)
{
    if (!ThrowEnable || !g_armed || !self) {
        SetViewPhase(0);
        return false;
    }
    g_lastFrameTick = GetTickCount64();
    FastFrame(self, tMs);   // fast release: returns at once unless a THROW armed it

    mthrow::Input in{};
    in.tMs = tMs;
    const char* cls = nullptr;
    in.meleeInHand = RightHandItemKind(&cls) == 1;
    // Ruling R24: while a throwable is readied / in flight / being cancelled (throwable.cpp's decision not Idle) the
    // melee throw never readies - fed as "no melee in hand", so a READY already open ends as the machine's own
    // CANCEL (no melee). COST: one volatile load.
    {
        static bool s_busySaid = false;
        const bool busy = ThrowableBusy();
        if (busy && in.meleeInHand) {
            in.meleeInHand = false;
            if (!s_busySaid) {
                s_busySaid = true;
                // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
                LOG("meleethrow: [THROW] SUPPRESSED - a throwable is readied / in flight (ThrowableBusy, ruling R24): the "
                    "melee throw is held off until it is Idle (right hand %s) | logged once per throwable",
                    cls ? cls : "(null)");
            }
        } else if (!busy) {
            s_busySaid = false;
        }
    }
    // Body grab (plan 2026-10-23-body-grab Task 4): the right hand holds a body - its weapon is hidden. Fed as "no melee in hand",
    // as ruling R24 above, so a READY already open ends as the machine's own CANCEL (no melee). COST: one volatile load.
    if (BodyGrabHandActive(1)) { in.meleeInHand = false; }
    // Physical breakout (plan 2026-10-24-physical-breakout, Build constants (e)): the same while a zombie holds the player - a shove
    // must not ready a throw. breakout_physical=0 -> always false. COST: one volatile load.
    if (BreakoutPlayerGrabbed()) { in.meleeInHand = false; }
    // The settings menu / the calibration screen (plan 2026-10-30-holster-place-by-hand Task 3 step 2a; Jay 2026-10-30: "Block it
    // while the menu is up"): the right trigger there works the menu or places a holster - it must not ready a throw. Fed as "no
    // melee in hand", as above, so a READY already open ends as the machine's own CANCEL (no melee). COST: two calls, two loads
    // (OverlayVisible's g.visible is a plain bool, CalibScreenActive's g_open a volatile).
    if (OverlayVisible() || CalibScreenActive()) { in.meleeInHand = false; }
    float trig = 0.0f;
    in.triggerOk = GetControllerTrigger(kRightHand, trig);
    in.trigger = trig;
    in.poseOk = poseOk;
    if (poseOk) {
        const holster::Vec3 local = holster::LocalOffset({ headPos[0], headPos[1], headPos[2] }, headQ[0], headQ[1],
                                                         headQ[2], headQ[3], { gripPos[0], gripPos[1], gripPos[2] });
        in.handY = local.y;
        if (velValid) {
            const swing::Vec3 hy = swing::InHeadYawFrame(headQ[0], headQ[1], headQ[2], headQ[3], { vx, vy, vz });
            in.fwdValid = true;
            in.fwdSpeed = -hy.z;   // head-yaw frame: -Z is ahead (swing_logic.h:52)
        }
    }
    in.flag = FlagOf(self);

    if (!g_firstFrameSaid) {
        g_firstFrameSaid = true;
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] FIRST FRAME (logged unconditionally) | right hand %s (melee %d) | trigger ok %d "
            "value %.2f | pose ok %d hand y %.3f m | velocity valid %d | +788 %d | +0x110 %d",
            cls ? cls : "(null)", in.meleeInHand ? 1 : 0, in.triggerOk ? 1 : 0, in.trigger, in.poseOk ? 1 : 0,
            in.handY, in.fwdValid ? 1 : 0, in.flag ? 1 : 0, StateOf(self));
    }

    const mthrow::Params p{ ThrowHeightY, ThrowTrigPress, ThrowTrigRelease, ThrowReleaseSpeed, ThrowSwingHoldoffMs,
                            kMaxVelAgeMs };
    const mthrow::Step st = g_m.Update(in, p);
    if (st.muteSwing) { ++g_muteFrames; }

    switch (st.ev) {
    case mthrow::Event::Readying: {
        g_muteFrames = 1;
        const int sent = SendReady(self);
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] READYING | hand y %.3f m (line %.3f) | trigger %.2f | 276 presses sent %d | +788 now "
            "%d",
            in.handY, ThrowHeightY, in.trigger, sent, FlagOf(self) ? 1 : 0);
        break;
    }
    case mthrow::Event::Ready:
    case mthrow::Event::AlreadyReady:
        ++g_nReady;
        if (st.ev == mthrow::Event::AlreadyReady) { g_muteFrames = 1; }
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] %s #%ld | +788 1 | %.0f ms after READYING%s", mthrow::EventName(st.ev), g_nReady,
            st.readyMs,
            st.ev == mthrow::Event::AlreadyReady ? " | +788 was ALREADY 1 on entry - nothing sent" : "");
        break;
    case mthrow::Event::Refused:
        ++g_nRefused;
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] REFUSED #%ld - +788 still 0 after the 276 presses (the game's CanReadRightInput or "
            "UpdateThrowInput said no, INV-313) | +0x110 %d | %s",
            g_nRefused, StateOf(self),
            st.state == mthrow::State::WaitRelease ? "trigger held - waiting for it to be let go" : "trigger up");
        break;
    case mthrow::Event::Lost:
        ++g_nLost;
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] LOST #%ld - the game cleared +788 %.0f ms into READY (UpdateThrowInput's conditions, "
            "INV-313) | nothing sent | +0x110 %d | swing muted %ld frame(s)",
            g_nLost, st.readyMs, StateOf(self), g_muteFrames);
        break;
    case mthrow::Event::Throw: {
        ++g_nThrow;
        const int before = FlagOf(self) ? 1 : 0;
        // Jay 2026-10-01 (INV-314, change 1): OnRightAttack throws only from an IDLE controller. Mid-attack it would
        // queue the next hit instead - so a busy controller is not called, and the let-go ends as a cancel below.
        const int stateNow = StateOf(self);
        const int nextHit = NextHitOf(self);
        const bool idle = stateNow == -1 && nextHit == -1;
        bool can = false;
        int ret = -1;
        if (idle) {
            can = g_canRead(self);
            if (can) { ret = static_cast<int>(g_attack(self)); }
        }
        // Jay 2026-10-01 (INV-314, change 2): +DC65F0 returns with +788 STILL SET when sub_180DC07D0 (mac CanThrowWeapon
        // by shape) fails, and the game has no timeout (INV-313). Whatever stopped the throw, never leave it ready.
        const int after = FlagOf(self) ? 1 : 0;
        int cancelSent = 0;
        if (after) {
            g_input(self, kReadyCode, 1.0f);
            cancelSent = 1;
        }
        const char* outcome = !idle ? "NOT CALLED - controller BUSY, treated as a CANCEL"
                              : !can ? "NOT CALLED - CanReadRightInput said no"
                              : after ? "CALLED - THROW REFUSED BY THE GAME (+788 still 1 after it)"
                                      : "CALLED";
        if (idle && can && !after) {
            g_viewReleaseEnd = GetTickCount64() + kReleaseViewMs;
            InterlockedExchange(&g_viewPhase, 2);
        }
        const bool fastArm = idle && can && !after;   // armed AFTER the THROW line below, so the log reads in order
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] THROW #%ld | forward %.2f m/s (threshold %.2f, age %.0f ms) | %.0f ms in READY | "
            "+0x110 %d +284 %d before | CanReadRightInput %d | OnRightAttack %s (ret %d) | +788 before %d after %d | "
            "cancel 276 sent %d, +788 now %d | +0x110 after %d | swing muted %ld frame(s) | ms to EMPTY = the next "
            "[ITEM] RIGHT hand EMPTY line's timestamp minus this one",
            g_nThrow, st.relSpeed, ThrowReleaseSpeed, st.relAgeMs, st.readyMs, stateNow, nextHit, can ? 1 : 0, outcome,
            ret, before, after, cancelSent, FlagOf(self) ? 1 : 0, StateOf(self), g_muteFrames);
        if (fastArm) { FastArm(self, tMs); }
        break;
    }
    case mthrow::Event::Cancel:
    case mthrow::Event::CancelNoMelee: {
        ++g_nCancel;
        const int before = FlagOf(self) ? 1 : 0;
        int sent = 0;
        if (before) {   // only while set: a 276 while clear stamps the tap time (spec §3a)
            g_input(self, kReadyCode, 1.0f);
            sent = 1;
        }
        char fwd[96];
        if (st.relValid) {
            std::snprintf(fwd, sizeof(fwd), "%.2f m/s (age %.0f ms)", st.relSpeed, st.relAgeMs);
        } else {
            std::snprintf(fwd, sizeof(fwd), "NOT MEASURED (no velocity within %.0f ms, or trigger unreadable)",
                          kMaxVelAgeMs);
        }
        // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
        LOG("meleethrow: [THROW] %s #%ld | forward %s, threshold %.2f | %.0f ms in READY | 276 sent %d | +788 before "
            "%d after %d | swing muted %ld frame(s)",
            mthrow::EventName(st.ev), g_nCancel, fwd, ThrowReleaseSpeed, st.readyMs, sent, before,
            FlagOf(self) ? 1 : 0, g_muteFrames);
        break;
    }
    default:
        break;
    }

    if (st.state == mthrow::State::Readying || st.state == mthrow::State::Ready) {
        SetViewPhase(1);
    } else if (g_viewPhase == 1) {
        SetViewPhase(0);
    }
    return st.muteSwing;
}

void ThrowTick()
{
    LONG want = 1;
    const char* why = "";
    if (!ThrowEnable) {
        want = 0;
        why = "throw_enable=0";
    } else if (MeleeSwingMode == 0) {
        want = 0;
        why = "melee_swing_mode=0 (the throw runs inside the swing driver's slot-16 hook and reads its velocity)";
    } else if (!MeleeR2SuppressOn()) {
        want = 0;
        why = "melee_r2_suppress=0 (the game's own R2 would attack and use READY up as a throw)";
    } else if (!g_input) {
        const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");   // until resolved only - never per frame
        if (!gd) {
            want = 0;
            why = "gamedll_x64_rwdi.dll not mapped yet (retried each second)";
        } else {
            const uintptr_t b = reinterpret_cast<uintptr_t>(gd);
            // The loaded build's rvas (common/buildrva.h): called from the swing hook, which can now install on EGL.
            g_canRead = reinterpret_cast<CanReadFn>(b + ::dlvr::buildrva::Gd(kCanReadRva));
            g_attack = reinterpret_cast<AttackFn>(b + ::dlvr::buildrva::Gd(kRightAttackRva));
            MemoryBarrier();
            g_input = reinterpret_cast<InputFn>(b + ::dlvr::buildrva::Gd(kInputRva));
        }
    }
    if (want != g_lastArm) {
        g_lastArm = want;
        if (want) {
            // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
            LOG("meleethrow: [THROW] ARMED (logged unconditionally) | line %.3f m | trigger %.2f / %.2f | release speed "
                "%.2f m/s | swing hold-off %.0f ms | velocity max age %.0f ms | handler gamedll+0x%zX, "
                "CanReadRightInput +0x%zX, OnRightAttack +0x%zX | melee_swing_mode %d, melee_r2_suppress 1",
                ThrowHeightY, ThrowTrigPress, ThrowTrigRelease, ThrowReleaseSpeed, ThrowSwingHoldoffMs, kMaxVelAgeMs,
                static_cast<size_t>(kInputRva), static_cast<size_t>(kCanReadRva),
                static_cast<size_t>(kRightAttackRva), MeleeSwingMode);
        } else {
            // [PROBE: THROW] gate=throw_enable  site=HEADSET  log-only=no
            LOG("meleethrow: [THROW] NOT ARMED - %s. Nothing is called in the game.", why);
        }
    }
    InterlockedExchange(&g_armed, want);
}

int ThrowViewPhase()
{
    const LONG ph = g_viewPhase;
    if (ph == 0) { return 0; }
    const ULONGLONG now = GetTickCount64();
    if (ph == 2) { return now < g_viewReleaseEnd ? 2 : 0; }
    return now <= g_lastFrameTick + 100 ? 1 : 0;   // READY, and the driver ran within 100 ms
}

}  // namespace dlvr
