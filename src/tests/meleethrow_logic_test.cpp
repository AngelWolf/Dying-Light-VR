// Standalone unit test for src/src/ovr2xr/meleethrow_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-09-30-melee-throw.md Task 2. Exit code = number of failures.
#include "meleethrow_logic.h"

#include <cstdio>
#include <limits>

using namespace dlvr::mthrow;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

constexpr double kDt = 1000.0 / 90.0;   // a 90 Hz frame, ms
constexpr float  kHigh = 0.0f;          // hand above the -0.10 line
constexpr float  kLow = -0.30f;         // hand below it

// Spec §5 defaults; max velocity age 50 ms (spec §3a).
static Params P() { return { -0.10f, 0.6f, 0.3f, 2.0f, 300.0f, 50.0f }; }

static Input In(double t, float trig, float y, bool flag)
{
    Input i{};
    i.tMs = t;
    i.meleeInHand = true;
    i.triggerOk = true;
    i.trigger = trig;
    i.poseOk = true;
    i.handY = y;
    i.flag = flag;
    return i;
}

static Input WithFwd(Input i, float fwd)
{
    i.fwdValid = true;
    i.fwdSpeed = fwd;
    return i;
}

// Drives Idle -> Readying -> Ready. Returns the time of the READY frame.
static double ToReady(Machine& m, double t)
{
    Step s = m.Update(In(t, 1.0f, kHigh, false), P());
    CHECK(s.ev == Event::Readying);
    CHECK(s.sendReady);
    t += kDt;
    s = m.Update(In(t, 1.0f, kHigh, true), P());
    CHECK(s.ev == Event::Ready);
    CHECK(m.state() == State::Ready);
    return t;
}

static void below_shoulder_never_readies()
{
    Machine m;
    for (int i = 0; i < 30; ++i) {
        const Step s = m.Update(In(i * kDt, 1.0f, kLow, false), P());
        CHECK(!s.sendReady);
        CHECK(s.ev == Event::None);
        CHECK(!s.muteSwing);
    }
    CHECK(m.state() == State::Idle);
}

static void trigger_then_raise_readies()
{
    Machine m;
    double t = 0.0;
    for (int i = 0; i < 5; ++i, t += kDt) { m.Update(In(t, 1.0f, kLow, false), P()); }
    const Step s = m.Update(In(t, 1.0f, kHigh, false), P());
    CHECK(s.ev == Event::Readying);
    CHECK(s.sendReady);
    CHECK(s.muteSwing);
}

static void ready_then_fast_release_throws()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    m.Update(WithFwd(In(t, 1.0f, kHigh, true), 3.0f), P());
    t += kDt;
    const Step s = m.Update(WithFwd(In(t, 0.0f, kHigh, true), 3.0f), P());
    CHECK(s.ev == Event::Throw);
    CHECK(s.sendThrow);
    CHECK(!s.sendCancel);
    CHECK(s.relValid);
    CHECK(s.relSpeed == 3.0f);
    CHECK(s.state == State::Holdoff);
    CHECK(s.muteSwing);
}

// Jay 2026-09-30: releasing R2 before the hand-speed check passes cancels.
static void release_before_motion_cancels()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    const Step s = m.Update(WithFwd(In(t, 0.0f, kHigh, true), 0.2f), P());
    CHECK(s.ev == Event::Cancel);
    CHECK(s.sendCancel);
    CHECK(!s.sendThrow);
    CHECK(s.relValid);
    CHECK(s.state == State::Idle);
    CHECK(!s.muteSwing);
}

static void stale_velocity_cancels()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    m.Update(WithFwd(In(t, 1.0f, kHigh, true), 3.0f), P());
    for (int i = 0; i < 10; ++i) { t += kDt; m.Update(In(t, 1.0f, kHigh, true), P()); }   // 111 ms, no velocity
    t += kDt;
    const Step s = m.Update(In(t, 0.0f, kHigh, true), P());
    CHECK(s.ev == Event::Cancel);
    CHECK(!s.sendThrow);
    CHECK(!s.relValid);
}

static void trigger_unreadable_cancels_never_throws()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    Input i = WithFwd(In(t, 0.0f, kHigh, true), 5.0f);
    i.triggerOk = false;
    const Step s = m.Update(i, P());
    CHECK(s.ev == Event::Cancel);
    CHECK(!s.sendThrow);
    CHECK(!s.relValid);
}

static void game_clear_is_lost_without_cancel()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    Step s = m.Update(In(t, 1.0f, kHigh, false), P());   // the game cleared +788, trigger still held
    CHECK(s.ev == Event::Lost);
    CHECK(!s.sendCancel);
    CHECK(s.state == State::WaitRelease);
    for (int i = 0; i < 5; ++i) {
        t += kDt;
        s = m.Update(In(t, 1.0f, kHigh, false), P());
        CHECK(!s.sendReady);
    }
    t += kDt;
    s = m.Update(In(t, 0.0f, kHigh, false), P());
    CHECK(s.state == State::Idle);
    t += kDt;
    s = m.Update(In(t, 1.0f, kHigh, false), P());
    CHECK(s.ev == Event::Readying);
}

static void refused_waits_for_release()
{
    Machine m;
    Step s = m.Update(In(0.0, 1.0f, kHigh, false), P());
    CHECK(s.ev == Event::Readying);
    s = m.Update(In(kDt, 1.0f, kHigh, false), P());   // +788 still 0 after the presses
    CHECK(s.ev == Event::Refused);
    CHECK(s.state == State::WaitRelease);
    CHECK(!s.sendCancel);
    s = m.Update(In(2 * kDt, 1.0f, kHigh, false), P());
    CHECK(!s.sendReady);
}

// A 276 while +788 is already 1 would toggle it OFF (INV-313).
static void already_ready_sends_nothing()
{
    Machine m;
    const Step s = m.Update(In(0.0, 1.0f, kHigh, true), P());
    CHECK(s.ev == Event::AlreadyReady);
    CHECK(!s.sendReady);
    CHECK(s.state == State::Ready);
}

static void hysteresis_no_chatter()
{
    Machine m;
    double t = ToReady(m, 0.0);
    const float wobble[] = { 0.55f, 0.35f, 0.5f, 0.31f, 0.59f, 0.4f };
    for (float w : wobble) {
        t += kDt;
        const Step s = m.Update(WithFwd(In(t, w, kHigh, true), 3.0f), P());
        CHECK(s.ev == Event::None);
    }
    t += kDt;
    const Step s = m.Update(WithFwd(In(t, 0.25f, kHigh, true), 3.0f), P());
    CHECK(s.ev == Event::Throw);
}

static void weapon_gone_cancels()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    Input i = In(t, 1.0f, kHigh, true);
    i.meleeInHand = false;
    const Step s = m.Update(i, P());
    CHECK(s.ev == Event::CancelNoMelee);
    CHECK(s.sendCancel);
    CHECK(s.state == State::WaitRelease);
}

static void height_ignored_once_ready()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    m.Update(WithFwd(In(t, 1.0f, -0.6f, true), 4.0f), P());
    t += kDt;
    const Step s = m.Update(WithFwd(In(t, 0.0f, -0.6f, true), 4.0f), P());
    CHECK(s.ev == Event::Throw);
}

static void holdoff_mutes_then_ends()
{
    Machine m;
    double t = ToReady(m, 0.0);
    t += kDt;
    Step s = m.Update(WithFwd(In(t, 0.0f, kHigh, true), 3.0f), P());
    CHECK(s.ev == Event::Throw);
    const double thrown = t;
    t += kDt;
    s = m.Update(In(t, 1.0f, kHigh, false), P());   // trigger pulled again during the hold-off
    CHECK(s.muteSwing);
    CHECK(!s.sendReady);
    while (t < thrown + 300.0) {
        t += kDt;
        s = m.Update(In(t, 1.0f, kHigh, false), P());
    }
    CHECK(s.state == State::WaitRelease);   // hold-off over, trigger still held: no re-ready
    CHECK(!s.muteSwing);
    t += kDt;
    s = m.Update(In(t, 0.0f, kHigh, false), P());
    CHECK(s.state == State::Idle);
}

static void pose_lost_does_not_ready()
{
    Machine m;
    Input i = In(0.0, 1.0f, kHigh, false);
    i.poseOk = false;
    const Step s = m.Update(i, P());
    CHECK(s.ev == Event::None);
    CHECK(!s.sendReady);
}

// ---- Fast release (Jay 2026-11-11: "a - release on let go, every weapon"; re_analysis/throw7_delay_ours.c) ----

static FastObs Obs(int state, int nextHit, int count, bool rel, bool launch)
{
    FastObs o{};
    o.state = state;
    o.nextHit = nextHit;
    o.count1608 = count;
    o.rel1626 = rel;
    o.launchSet = launch;
    return o;
}

static void fast_backdate()
{
    float out = 0.0f;
    CHECK(FastBackdate(100.0f, 0.6f, 0.01f, &out));
    CHECK(out > 99.3899f && out < 99.3901f);
    CHECK(FastBackdate(100.0f, -1.0f, 0.01f, &out));   // a negative delay counts as 0
    CHECK(out > 99.9899f && out < 99.9901f);
    CHECK(!FastBackdate(100.0f, 50.0f, 0.01f, &out));  // beyond the sane bound: refused, nothing written
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!FastBackdate(100.0f, nan, 0.01f, &out));
    CHECK(!FastBackdate(nan, 0.5f, 0.01f, &out));
    CHECK(!FastBackdate(-1.0f, 0.5f, 0.01f, &out));    // the stamp was not set (-1.0 = the game's "none")
}

static void fast_waits_for_start_then_launch_then_releases()
{
    FastTracker f;
    CHECK(!f.active());
    f.Arm(0.0);
    CHECK(f.active());
    FastStep s = f.Step(10.0, Obs(-1, 19, 0, false, false));   // queued, not started
    CHECK(s.ev == FastEv::None && !s.writeRelease);
    s = f.Step(20.0, Obs(19, -1, 0, false, false));            // started, launch point not filled by the game yet
    CHECK(s.ev == FastEv::Started && !s.writeRelease);
    s = f.Step(30.0, Obs(19, -1, 0, false, true));
    CHECK(s.ev == FastEv::Released && s.writeRelease);
    CHECK(!f.active());
    s = f.Step(40.0, Obs(19, -1, 0, false, true));             // never twice
    CHECK(s.ev == FastEv::None && !s.writeRelease);
}

static void fast_start_and_release_same_frame()
{
    FastTracker f;
    f.Arm(0.0);
    const FastStep s = f.Step(10.0, Obs(18, -1, 0, false, true));
    CHECK(s.ev == FastEv::Released && s.writeRelease);
    CHECK(s.startMs > 9.9 && s.startMs < 10.1);
}

static void fast_game_already_released()
{
    FastTracker f;
    f.Arm(0.0);
    FastStep s = f.Step(10.0, Obs(18, -1, 1, false, true));
    CHECK(s.ev == FastEv::GameReleased && !s.writeRelease);
    CHECK(!f.active());
    f.Arm(100.0);
    s = f.Step(110.0, Obs(18, -1, 0, true, true));   // the game's own event byte is already set this frame
    CHECK(s.ev == FastEv::GameReleased && !s.writeRelease);
}

static void fast_dropped_timeout_other()
{
    FastTracker f;
    f.Arm(0.0);
    FastStep s = f.Step(10.0, Obs(-1, -1, 0, false, false));   // the game dropped the queued throw
    CHECK(s.ev == FastEv::Dropped && !s.writeRelease && !f.active());

    f.Arm(0.0);
    s = f.Step(100.0, Obs(-1, 19, 0, false, false));
    CHECK(s.ev == FastEv::None);
    s = f.Step(kFastTimeoutMs + 1.0, Obs(-1, 19, 0, false, false));
    CHECK(s.ev == FastEv::Timeout && !s.writeRelease && !f.active());

    f.Arm(0.0);
    s = f.Step(10.0, Obs(5, -1, 0, false, true));              // some other attack started: not a throw
    CHECK(s.ev == FastEv::NotThrow && !s.writeRelease && !f.active());

    f.Arm(0.0);
    s = f.Step(10.0, Obs(19, -1, 0, false, false));
    CHECK(s.ev == FastEv::Started);
    s = f.Step(20.0, Obs(-1, -1, 0, false, false));            // the throw ended before a launch point appeared
    CHECK(s.ev == FastEv::Ended && !s.writeRelease && !f.active());
}

static void fast_idle_does_nothing()
{
    FastTracker f;
    const FastStep s = f.Step(10.0, Obs(18, -1, 0, false, true));   // not armed: a throw we did not start
    CHECK(s.ev == FastEv::None && !s.writeRelease);
}

int main()
{
    fast_backdate();
    fast_waits_for_start_then_launch_then_releases();
    fast_start_and_release_same_frame();
    fast_game_already_released();
    fast_dropped_timeout_other();
    fast_idle_does_nothing();
    below_shoulder_never_readies();
    trigger_then_raise_readies();
    ready_then_fast_release_throws();
    release_before_motion_cancels();
    stale_velocity_cancels();
    trigger_unreadable_cancels_never_throws();
    game_clear_is_lost_without_cancel();
    refused_waits_for_release();
    already_ready_sends_nothing();
    hysteresis_no_chatter();
    weapon_gone_cancels();
    height_ignored_once_ready();
    holdoff_mutes_then_ends();
    pose_lost_does_not_ready();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
