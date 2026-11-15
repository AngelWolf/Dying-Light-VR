// Standalone unit test for src/src/ovr2xr/throwable_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-14-throwables-build-a.md Task 2. Exit code = number of failures.
// Compile: the same cl line as the melee throw logic test (/EHsc /std:c++17 /W4 /WX, include path src/src/ovr2xr).
#include "throwable_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::throwable;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

constexpr double kDt = 1000.0 / 90.0;   // a 90 Hz frame, ms

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static bool VecNear(Vec3 a, Vec3 b, float tol) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }

// A throwable selected, the RIGHT trigger (rt) pressed with the right hand in the right-hip zone (inHip) or not
// (spec 2026-11-01-manual-reload-design goal 2: R2 at the right hip readies; let go at the hip = cancel, elsewhere = throw).
static Input Sel(double t, float rt, bool inHip, int gameState)
{
    Input i{};
    i.tMs = t;
    i.throwableSelected = true;
    i.ltOk = true;
    i.rtOk = true;
    i.rt = rt;
    i.rtInRHip = inHip;
    i.gameState = gameState;
    return i;
}

// The LEFT trigger with a throwable selected (no hip meaning any more - it is only ever withheld).
static Input SelLt(double t, float lt, int gameState)
{
    Input i = Sel(t, 0.0f, false, gameState);
    i.lt = lt;
    return i;
}

// Idle -> Readying -> Readied, then the hand leaves the hip with RT still held (its let-go is the throw). Returns the time.
static double ToReadied(Machine& m, double t)
{
    Params p;
    Step s = m.Update(Sel(t, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    CHECK(s.holdL2);
    CHECK(s.state == State::Readying);
    t += kDt;
    s = m.Update(Sel(t, 1.0f, true, 2), p);
    CHECK(s.ev == Event::Readied);
    CHECK(s.state == State::Readied);
    CHECK(s.holdL2);
    t += kDt;
    s = m.Update(Sel(t, 1.0f, false, 2), p);   // out of the hip, RT held: the layer still holds L2
    CHECK(s.holdL2);
    CHECK(s.state == State::Readied);
    CHECK(s.ev == Event::None);
    return t;
}

static void case1_nothing_selected()
{
    Params p;
    Machine m;
    double t = 0;
    for (int ltOk = 0; ltOk < 2; ++ltOk)
        for (int lt = 0; lt < 2; ++lt)
            for (int hip = 0; hip < 2; ++hip)
                for (int rt = 0; rt < 2; ++rt)
                    for (int gs = -1; gs <= 2; ++gs) {
                        Input i{};
                        i.tMs = t;
                        i.throwableSelected = false;
                        i.ltOk = ltOk != 0;
                        i.lt = lt ? 1.0f : 0.0f;
                        i.rtInRHip = hip != 0;   // Jay: nothing selected -> R2 at the hip is plain R2 ("Let it through")
                        i.rtOk = true;
                        i.rt = rt ? 1.0f : 0.0f;
                        i.gameState = gs;
                        Step s = m.Update(i, p);
                        CHECK(!s.holdL2);
                        CHECK(!s.withholdL2);
                        CHECK(!s.withholdR2);
                        CHECK(!s.requestCancel);
                        CHECK(s.state == State::Idle);
                        t += kDt;
                    }
}

// LT anywhere, a throwable selected: withheld, never a Ready (the left hip is the ammo pouch now - LtAtLeftHipDoesNothingNow).
static void case2_lt_away_from_hip_is_withheld()
{
    Params p;
    Machine m;
    int withheld = 0;
    double t = 0;
    for (int k = 0; k < 4; ++k, t += kDt) {
        Step s = m.Update(SelLt(t, 1.0f, 0), p);
        CHECK(s.withholdL2);
        CHECK(!s.holdL2);
        CHECK(s.state == State::Idle);
        CHECK(s.ev != Event::Ready);
        if (s.ev == Event::Withheld) ++withheld;
    }
    CHECK(withheld == 1);   // once, on the press edge
    Step s = m.Update(SelLt(t, 0.0f, 0), p);   // let go: passes through
    CHECK(!s.withholdL2);
    t += kDt;
    s = m.Update(SelLt(t, 1.0f, 0), p);        // a second press = a second event
    CHECK(s.ev == Event::Withheld);
    CHECK(s.withholdL2);
}

static void case3_lt_in_hip_readies()   // now: RT at the RIGHT hip readies (RtAtRightHipReadies)
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    CHECK(s.holdL2);
    CHECK(s.withholdR2);
    CHECK(s.state == State::Readying);
    s = m.Update(Sel(kDt, 1.0f, true, 2), p);
    CHECK(s.ev == Event::Readied);
    CHECK(s.state == State::Readied);
    CHECK(s.holdL2);
}

static void case4_readied_lt_released_still_holds()   // now: Readied, RT held away from the hip - still held, nothing fires
{
    Machine m;
    double t = ToReadied(m, 0);
    Params p;
    Step s = m.Update(Sel(t + kDt, 1.0f, false, 2), p);
    CHECK(s.holdL2);
    CHECK(s.withholdR2);
    CHECK(s.ev == Event::None);
    CHECK(!s.requestCancel);
}

static void case5_rt_release_throws()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.rt = 1.0f;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::None);
    CHECK(s.holdL2);
    CHECK(!s.snapshotRelease);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.rt = 0.0f;
    s = m.Update(i, p);
    CHECK(s.ev == Event::Release);
    CHECK(s.snapshotRelease);
    CHECK(s.withholdL2);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Releasing);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.gameReleased = true;
    s = m.Update(i, p);
    CHECK(s.ev == Event::Released);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
    CHECK(!s.snapshotRelease);
}

// RtReleaseAtHipCancels (Jay, plan time: "Let go at the hip"): back in the right-hip zone, RT let go = put it back, never a throw.
static void case6_lt_in_hip_again_cancels()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);   // RT held, out of the hip
    Step s = m.Update(Sel(t += kDt, 1.0f, true, 2), p);   // back at the hip, still held: nothing yet
    CHECK(s.ev == Event::None);
    CHECK(s.state == State::Readied);
    s = m.Update(Sel(t += kDt, 0.0f, true, 2), p);        // let go AT the hip
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(s.passSwitch);
    CHECK(!s.snapshotRelease);
    s = m.Update(Sel(t += kDt, 0.0f, true, 2), p);   // the game still shows state 2: L2 stays held
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    CHECK(s.state == State::Readied);
    s = m.Update(Sel(t += kDt, 0.0f, true, 0), p);   // the game reads 0: drop the hold
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    CHECK(!s.requestCancel);
    CHECK(s.ev == Event::None);
}

// RtReleaseDuringReadyingCancels (plan Review Focus 4): RT let go before the game has the item in hand = a cancel, not a throw.
static void case28_rt_release_during_readying_cancels()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    s = m.Update(Sel(kDt, 0.0f, true, 1), p);   // let go while the game shows 1
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);   // never a dropped L2 mid-start
    CHECK(!s.snapshotRelease);
    s = m.Update(Sel(2 * kDt, 0.0f, true, 0), p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    CHECK(s.ev != Event::Release);
    // let go away from the hip during Readying: the same cancel
    Machine m2;
    m2.Update(Sel(0, 1.0f, true, 0), p);
    s = m2.Update(Sel(kDt, 0.0f, false, 1), p);
    CHECK(s.ev == Event::Cancel);
    CHECK(!s.snapshotRelease);
}

// RtAtHipWithheldWhateverIsHeld (Jay: "Withheld from whatever the right hand holds"): with a throwable selected, R2 at the hip
// never reaches the game - even when the ready cannot start (the controller unreadable).
static void case29_rt_at_hip_withheld_whatever_is_held()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, -1), p);   // cannot ready (state unreadable)
    CHECK(s.ev != Event::Ready);
    CHECK(s.withholdR2);
    s = m.Update(Sel(kDt, 1.0f, true, -1), p);
    CHECK(s.withholdR2);
    s = m.Update(Sel(2 * kDt, 0.0f, true, -1), p);
    CHECK(!s.withholdR2);
    s = m.Update(Sel(3 * kDt, 1.0f, false, 0), p);  // away from the hip: plain R2
    CHECK(!s.withholdR2);
    CHECK(s.ev != Event::Ready);
}

// LtAtLeftHipDoesNothingNow: the left trigger never readies (the left hip is the ammo pouch).
static void case30_lt_at_left_hip_does_nothing_now()
{
    Params p;
    Machine m;
    Step s = m.Update(SelLt(0, 1.0f, 0), p);
    CHECK(s.ev == Event::Withheld);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
}

// Ruling (Task 3): a cancel that ends with RT still held must not hand that R2 to the game (the gun / melee weapon would fire the
// moment the throwable is put away) - withheld until RT is let go, then plain R2 again.
static void case31_rt_held_after_cancel_is_withheld_until_let_go()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);   // RT held
    Input i = Sel(t += kDt, 1.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = false;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::CancelForSwitch);
    i = Sel(t += kDt, 1.0f, false, 0);
    i.switchPress = true;
    s = m.Update(i, p);   // the cancel finishes, RT still held
    CHECK(s.state == State::Idle);
    CHECK(s.withholdR2);
    i = Sel(t += kDt, 1.0f, false, 0);
    i.throwableSelected = false;     // even after the switch landed on a non-throwable
    s = m.Update(i, p);
    CHECK(s.withholdR2);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.throwableSelected = false;
    s = m.Update(i, p);              // let go
    CHECK(!s.withholdR2);
    i = Sel(t += kDt, 1.0f, false, 0);
    i.throwableSelected = false;
    s = m.Update(i, p);              // a new press: plain R2
    CHECK(!s.withholdR2);
}

static void case7_switch_to_non_throwable()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = false;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::CancelForSwitch);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(!s.passSwitch);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(!s.passSwitch);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.switchPress = true;
    s = m.Update(i, p);
    CHECK(s.passSwitch);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    CHECK(!s.reReady);
    i = Sel(t += kDt, 0.0f, false, 0);   // the switch went through: nothing re-readies
    i.throwableSelected = false;
    s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(s.state == State::Idle);
}

static void case8_switch_to_throwable_rereadies()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = true;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::CancelForSwitch);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(!s.passSwitch);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(!s.passSwitch);
    CHECK(!s.reReady);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.switchPress = true;
    i.switchToThrowable = true;
    i.selectedItem = 0xA;   // the old item, latched on the poll the switch passes
    s = m.Update(i, p);
    CHECK(s.passSwitch);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    CHECK(!s.reReady);
    i = Sel(t += kDt, 0.0f, false, 0);   // the switch has not landed: still the old item
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(!s.holdL2);
    i = Sel(t += kDt, 0.0f, false, 0);   // the new item is selected and the game shows 0
    i.selectedItem = 0xB;
    s = m.Update(i, p);
    CHECK(s.reReady);
    CHECK(s.ev == Event::Ready);
    CHECK(s.holdL2);
    CHECK(s.state == State::Readying);
}

static void case9_menu_blocking_cancels()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.menuBlocking = true;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(s.passSwitch);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    i = Sel(t += kDt, 1.0f, true, 0);   // a press at the hip while a menu blocks input: no Ready
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
}

static void case10_release_timeout()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.rt = 1.0f;
    m.Update(i, p);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.rt = 0.0f;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::Release);
    const double t0 = t;
    s = m.Update(Sel(t0 + p.releaseTimeoutMs - 1.0, 0.0f, false, 2), p);
    CHECK(s.state == State::Releasing);
    CHECK(s.ev == Event::None);
    s = m.Update(Sel(t0 + p.releaseTimeoutMs, 0.0f, false, 2), p);
    CHECK(s.ev == Event::ReleaseTimeout);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
}

static void case11_ready_refused()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    s = m.Update(Sel(p.readyTimeoutMs - 500.0, 1.0f, true, 0), p);
    CHECK(s.state == State::Readying);
    CHECK(s.holdL2);
    s = m.Update(Sel(p.readyTimeoutMs, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Refused);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
    CHECK(!s.requestCancel);   // the game is already at 0
}

static void case12_game_cancels_by_itself()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Step s = m.Update(Sel(t + kDt, 0.0f, false, 0), p);
    CHECK(s.ev == Event::Lost);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
    CHECK(!s.requestCancel);
}

static void case13_thrown_velocity()
{
    Params p;
    CHECK(VecNear(ThrownVelocity({0, 0, -5.0f}, 13.0f, p), {0, 0, -13.0f}, 1e-4f));
    CHECK(VecNear(ThrownVelocity({0, 0, -1.0f}, 13.0f, p), {0, 0, -13.0f / 5.0f}, 1e-4f));
    CHECK(VecNear(ThrownVelocity({0, 0, -50.0f}, 13.0f, p), {0, 0, -13.0f * 1.25f}, 1e-3f));
    CHECK(VecNear(ThrownVelocity({0, 0, 0.01f}, 13.0f, p), {0, -0.5f, 0}, 1e-6f));
    CHECK(VecNear(ThrownVelocity({0, 0, -5.0f}, 0.0f, p), {0, 0, 0}, 1e-6f));
}

static void case14_hand_history()
{
    {
        HandHistory h;
        Vec3 v; double at = 0;
        CHECK(!h.Fastest(100, 500, v, at));
        Vec3 d;
        CHECK(!h.Stroke(100, 100, 40, d));
    }
    {
        HandHistory h;
        for (int k = 0; k <= 20; ++k) h.Push(k * 10.0, {2.0f * (k * 10.0f) / 1000.0f, 0, 0});
        Vec3 v; double at = 0;
        CHECK(h.Fastest(200, 500, v, at));
        CHECK(Near(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), 2.0f, 0.01f));
        CHECK(Near(v.x, 2.0f, 0.01f));
    }
    {
        // 1 m/s along +x for 400 ms, except a 100 ms flick at 6 m/s along -z from t=250 to t=350.
        HandHistory h;
        Vec3 pos{};
        h.Push(0, pos);
        for (int k = 1; k <= 40; ++k) {
            const double t = k * 10.0;
            if (t > 250.0 && t <= 350.0) pos.z -= 0.06f; else pos.x += 0.01f;
            h.Push(t, pos);
        }
        Vec3 v; double at = 0;
        CHECK(h.Fastest(400, 500, v, at));
        CHECK(Near(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), 6.0f, 0.2f));
        CHECK(Near(v.z, -6.0f, 0.2f));
        CHECK(Near(static_cast<float>(at), 350.0f, 15.0f));
        Vec3 d;
        CHECK(h.Stroke(at, 100, 40, d));
        CHECK(d.z < -0.95f);
        CHECK(Near(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), 1.0f, 1e-3f));
    }
}

static void case16_cancel_stuck()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.menuBlocking = true;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::Cancel);
    const double t0 = t;
    i = Sel(t0 + p.cancelTimeoutMs - 1.0, 0.0f, false, 2);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.ev == Event::None);
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    i = Sel(t0 + p.cancelTimeoutMs, 0.0f, false, 2);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.ev == Event::CancelStuck);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);   // never dropped: that would throw at the feet
    CHECK(s.state == State::Readied);
    i = Sel(t0 + p.cancelTimeoutMs + 100.0, 0.0f, false, 2);   // the timer restarted
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.ev == Event::None);
    CHECK(s.holdL2);
    i = Sel(t0 + p.cancelTimeoutMs + 200.0, 0.0f, false, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
}

// R9: a Readying timeout with the game off 0 goes through the cancel wait; no Ready while the state is unreadable.
static void case17_refused_waits_for_zero()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    s = m.Update(Sel(100, 1.0f, true, 1), p);   // the game shows 1 (restarts the ready timer once)
    CHECK(s.state == State::Readying);
    s = m.Update(Sel(100 + p.readyTimeoutMs, 1.0f, true, 1), p);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(s.ev != Event::Refused);
    s = m.Update(Sel(100 + p.readyTimeoutMs + kDt, 1.0f, true, 1), p);   // still 1: L2 stays held
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    s = m.Update(Sel(100 + p.readyTimeoutMs + 2 * kDt, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Refused);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
    // gameState -1 at the timeout: the same wait, never a drop.
    Machine m2;
    m2.Update(Sel(0, 1.0f, true, 0), p);
    s = m2.Update(Sel(p.readyTimeoutMs, 1.0f, true, -1), p);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(s.ev != Event::Refused);
    // LT at the hip while the state is unreadable: no Ready, no hold.
    Machine m3;
    s = m3.Update(Sel(0, 1.0f, true, -1), p);
    CHECK(s.ev != Event::Ready);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
}

// R10: an unreadable trigger is no information, never a let-go.
static void case18_trigger_dropout_is_not_a_release()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.rt = 1.0f;
    m.Update(i, p);   // armed
    for (int k = 0; k < 5; ++k) {
        i = Sel(t += kDt, 0.0f, false, 2);
        i.rtOk = false;
        Step s = m.Update(i, p);
        CHECK(s.ev == Event::None);
        CHECK(s.holdL2);
        CHECK(!s.snapshotRelease);
        CHECK(s.state == State::Readied);
    }
    i = Sel(t += kDt, 0.0f, false, 2);   // readable again, still held
    i.rt = 1.0f;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::None);
    CHECK(s.holdL2);
    i = Sel(t += kDt, 0.0f, false, 2);   // now really let go
    i.rt = 0.0f;
    s = m.Update(i, p);
    CHECK(s.ev == Event::Release);
    CHECK(s.snapshotRelease);
}

// R11: a cancel wait with the state unreadable for cancelTimeoutMs = the controller is gone; stuck at 1/2 passes the switch.
static void case19_cancel_gone_and_stuck_switch()
{
    Params p;
    {
        Machine m;
        double t = ToReadied(m, 0);
        Input i = Sel(t += kDt, 0.0f, false, 2);
        i.switchPress = true;
        i.switchToThrowable = false;
        Step s = m.Update(i, p);
        CHECK(s.ev == Event::CancelForSwitch);
        const double t0 = t;
        i = Sel(t0 + 10.0, 0.0f, false, -1);
        i.switchPress = true;
        s = m.Update(i, p);
        CHECK(s.holdL2);
        CHECK(!s.passSwitch);
        i = Sel(t0 + p.cancelTimeoutMs + 1.0, 0.0f, false, -1);
        i.switchPress = true;
        s = m.Update(i, p);
        CHECK(s.ev == Event::CancelStuck);   // the plain cancel timeout (500 from the cancel start) fires first
        CHECK(s.holdL2);
        CHECK(s.passSwitch);
        i = Sel(t0 + 10.0 + p.cancelTimeoutMs + 2.0, 0.0f, false, -1);   // unreadable for the whole timeout
        i.switchPress = true;
        s = m.Update(i, p);
        CHECK(s.ev == Event::Lost);
        CHECK(!s.holdL2);
        CHECK(s.passSwitch);
        CHECK(s.state == State::Idle);
    }
    {
        Machine m;   // stuck at 2: CancelStuck passes the held switch, keeps L2
        double t = ToReadied(m, 0);
        Input i = Sel(t += kDt, 0.0f, false, 2);
        i.switchPress = true;
        m.Update(i, p);
        const double t0 = t;
        i = Sel(t0 + p.cancelTimeoutMs - 1.0, 0.0f, false, 2);
        i.switchPress = true;
        Step s = m.Update(i, p);
        CHECK(!s.passSwitch);
        i = Sel(t0 + p.cancelTimeoutMs, 0.0f, false, 2);
        i.switchPress = true;
        s = m.Update(i, p);
        CHECK(s.ev == Event::CancelStuck);
        CHECK(s.requestCancel);
        CHECK(s.holdL2);
        CHECK(s.passSwitch);
    }
}

// R12: a re-ready waits for the item to change, bounded by reReadyTimeoutMs.
static void case20_reready_timeout()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = true;
    i.selectedItem = 0xA;
    m.Update(i, p);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.switchPress = true;
    i.selectedItem = 0xA;
    Step s = m.Update(i, p);   // the switch passes, item A latched
    CHECK(s.passSwitch);
    const double t0 = t;
    i = Sel(t0 + p.reReadyTimeoutMs - 1.0, 0.0f, false, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(s.ev == Event::None);
    i = Sel(t0 + p.reReadyTimeoutMs + 1.0, 0.0f, false, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(s.ev == Event::ReReadyTimeout);
    CHECK(s.state == State::Idle);
    CHECK(!s.holdL2);
    i = Sel(t0 + p.reReadyTimeoutMs + 20.0, 0.0f, false, 0);   // the item changing now no longer re-readies
    i.selectedItem = 0xB;
    s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(s.state == State::Idle);
}

// Readied -> throwable switch cancel -> the finish poll with `latched` as selectedItem. Returns the finish poll's time.
static double ToSwitchPassed(Machine& m, std::uintptr_t latched)
{
    Params p;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.switchPress = true;
    i.switchToThrowable = true;
    i.selectedItem = latched;
    m.Update(i, p);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.switchPress = true;
    i.switchToThrowable = true;
    i.selectedItem = latched;
    Step s = m.Update(i, p);
    CHECK(s.passSwitch);
    CHECK(s.state == State::Idle);
    return t;
}

static Step IdlePoll(Machine& m, double t, std::uintptr_t item)
{
    Params p;
    Input i = Sel(t, 0.0f, false, 0);
    i.selectedItem = item;
    return m.Update(i, p);
}

// N1: selectedItem 0 = unknown is never a change and never a baseline.
static void case21_unknown_item_is_not_a_change()
{
    Params p;
    {   // finish poll reads 0, then the old item, then a new one: a latched 0 can only time out
        Machine m;
        double t = ToSwitchPassed(m, 0);
        CHECK(!IdlePoll(m, t + kDt, 0xA).reReady);
        CHECK(!IdlePoll(m, t + 2 * kDt, 0xB).reReady);
        Step s = IdlePoll(m, t + p.reReadyTimeoutMs + 1.0, 0xB);
        CHECK(s.ev == Event::ReReadyTimeout);
        CHECK(!s.reReady);
    }
    {   // latched old item, then 0 (unknown): no re-ready
        Machine m;
        double t = ToSwitchPassed(m, 0xA);
        CHECK(!IdlePoll(m, t + kDt, 0).reReady);
        CHECK(!IdlePoll(m, t + 2 * kDt, 0xA).reReady);
        Step s = IdlePoll(m, t + 3 * kDt, 0xB);   // old -> new nonzero
        CHECK(s.reReady);
        CHECK(s.ev == Event::Ready);
        CHECK(s.holdL2);
    }
}

// N2: the ReReadyTimeout poll still applies the Idle LT handling; no poll passes raw L2 with a throwable selected.
static void case22_timeout_poll_still_withholds()
{
    Params p;
    Machine m;
    double t = ToSwitchPassed(m, 0xA);
    // LT pressed after the switch, held across the timeout.
    Step s = IdlePoll(m, t + kDt, 0xA);
    Input i = SelLt(t + 2 * kDt, 1.0f, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(s.withholdL2);
    i = SelLt(t + p.reReadyTimeoutMs - 1.0, 1.0f, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(s.withholdL2);
    CHECK(s.ev == Event::None);
    i = SelLt(t + p.reReadyTimeoutMs + 1.0, 1.0f, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(s.ev == Event::ReReadyTimeout);
    CHECK(s.withholdL2);
    CHECK(!s.holdL2);
    i = SelLt(t + p.reReadyTimeoutMs + 20.0, 1.0f, 0);
    i.selectedItem = 0xA;
    s = m.Update(i, p);
    CHECK(s.withholdL2);
}

// Ruling R21: the throwable stops being "selected" while Readied (a vehicle entered) or Readying = a cancel: requestCancel
// with L2 still held, held while the game shows 2, dropped only when 0 reads back. Never a dropped L2 (a throw at the feet),
// never L2 held after 0 (the buggy's brake).
static void case23_deselected_while_readied_cancels()
{
    Params p;
    Machine m;
    double t = ToReadied(m, 0);
    Input i = Sel(t += kDt, 0.0f, false, 2);
    i.throwableSelected = false;
    Step s = m.Update(i, p);
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    CHECK(!s.withholdL2);
    i = Sel(t += kDt, 0.0f, false, 2);
    i.throwableSelected = false;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.throwableSelected = false;
    s = m.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(!s.withholdL2);
    CHECK(s.state == State::Idle);

    // The same from Readying (the game shows 1).
    Machine m2;
    s = m2.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    i = Sel(kDt, 1.0f, true, 1);
    i.throwableSelected = false;
    s = m2.Update(i, p);
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    i = Sel(2 * kDt, 1.0f, true, 0);
    i.throwableSelected = false;
    s = m2.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
}

// Ruling R23: a cancel in Readying while the game still reads 0 (its start may be pending) - L2 stays held through the
// 0 -> 1 start (the cancel is asked for again when 1 appears) and drops only at the final 0.
static void case24_readying_cancel_through_a_late_start()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    CHECK(m.busy());
    Input i = Sel(kDt, 1.0f, true, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.ev == Event::Cancel);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    i = Sel(2 * kDt, 1.0f, true, 0);   // still 0: the start may be pending - keep holding
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    CHECK(s.state == State::Readying);
    i = Sel(3 * kDt, 1.0f, true, 1);   // the start landed: ask again at once, keep holding
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(s.requestCancel);
    i = Sel(4 * kDt, 1.0f, true, 1);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(!s.requestCancel);
    i = Sel(5 * kDt, 1.0f, true, 0);   // cancelled after the start: drop
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
    CHECK(!m.busy());
}

// Ruling R23: a cancel in Readying with 0 throughout - held until readyTimeoutMs since the Ready, then dropped.
static void case25_readying_cancel_zero_throughout()
{
    Params p;
    Machine m;
    Step s = m.Update(Sel(0, 1.0f, true, 0), p);
    CHECK(s.ev == Event::Ready);
    Input i = Sel(kDt, 1.0f, true, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.requestCancel);
    CHECK(s.holdL2);
    i = Sel(p.readyTimeoutMs - 1.0, 1.0f, true, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(s.holdL2);
    CHECK(s.ev == Event::None);   // not CANCEL STUCK: a pending start is not stuck
    CHECK(m.busy());
    i = Sel(p.readyTimeoutMs, 1.0f, true, 0);
    i.menuBlocking = true;
    s = m.Update(i, p);
    CHECK(!s.holdL2);
    CHECK(s.state == State::Idle);
}

// INV-574: after a switch the throw controller keeps its Start-time item until the game's hand change lands; a re-ready
// before that restarts the OLD item. The new item alone is not enough - the re-ready waits for switchSettled too.
static void case26_reready_waits_for_the_hand_change()
{
    Params p;
    Machine m;
    double t = ToSwitchPassed(m, 0xA);
    Input i = Sel(t += kDt, 0.0f, false, 0);
    i.selectedItem = 0xB;            // the HUD already shows the new item...
    i.switchSettled = false;         // ...but the controller still holds the old one
    Step s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(s.ev == Event::None);
    CHECK(!s.holdL2);
    i = Sel(t += 200.0, 0.0f, false, 0);
    i.selectedItem = 0xB;
    i.switchSettled = false;
    s = m.Update(i, p);
    CHECK(!s.reReady);
    i = Sel(t += kDt, 0.0f, false, 0);
    i.selectedItem = 0xB;
    i.switchSettled = true;          // the hand change landed
    s = m.Update(i, p);
    CHECK(s.reReady);
    CHECK(s.ev == Event::Ready);
    CHECK(s.holdL2);
}

// INV-574: a switch that never settles is dropped at reReadyTimeoutMs, never readied.
static void case27_unsettled_switch_times_out()
{
    Params p;
    Machine m;
    double t0 = ToSwitchPassed(m, 0xA);
    Input i = Sel(t0 + p.reReadyTimeoutMs - 1.0, 0.0f, false, 0);
    i.selectedItem = 0xB;
    i.switchSettled = false;
    Step s = m.Update(i, p);
    CHECK(!s.reReady);
    i = Sel(t0 + p.reReadyTimeoutMs + 1.0, 0.0f, false, 0);
    i.selectedItem = 0xB;
    i.switchSettled = false;
    s = m.Update(i, p);
    CHECK(s.ev == Event::ReReadyTimeout);
    CHECK(!s.reReady);
    i = Sel(t0 + p.reReadyTimeoutMs + 20.0, 0.0f, false, 0);
    i.selectedItem = 0xB;
    i.switchSettled = true;          // settling after the timeout no longer re-readies
    s = m.Update(i, p);
    CHECK(!s.reReady);
    CHECK(s.state == State::Idle);
}

int main()
{
    case1_nothing_selected();
    case2_lt_away_from_hip_is_withheld();
    case3_lt_in_hip_readies();
    case4_readied_lt_released_still_holds();
    case5_rt_release_throws();
    case6_lt_in_hip_again_cancels();
    case7_switch_to_non_throwable();
    case8_switch_to_throwable_rereadies();
    case9_menu_blocking_cancels();
    case10_release_timeout();
    case11_ready_refused();
    case12_game_cancels_by_itself();
    case13_thrown_velocity();
    case14_hand_history();
    case16_cancel_stuck();
    case17_refused_waits_for_zero();
    case18_trigger_dropout_is_not_a_release();
    case19_cancel_gone_and_stuck_switch();
    case20_reready_timeout();
    case21_unknown_item_is_not_a_change();
    case22_timeout_poll_still_withholds();
    case23_deselected_while_readied_cancels();
    case24_readying_cancel_through_a_late_start();
    case25_readying_cancel_zero_throughout();
    case26_reready_waits_for_the_hand_change();
    case27_unsettled_switch_times_out();
    case28_rt_release_during_readying_cancels();
    case29_rt_at_hip_withheld_whatever_is_held();
    case30_lt_at_left_hip_does_nothing_now();
    case31_rt_held_after_cancel_is_withheld_until_let_go();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
