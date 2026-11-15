// Standalone unit test for src/src/ovr2xr/swing_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-09-29-vr-swing-attack.md Task 1. Exit code = number of failures.
#include "swing_logic.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace dlvr::swing;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b, float eps = 1e-4f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

constexpr float kMinTravel = 0.35f;     // melee_swing_min_travel_m's default
// Return-stroke rule ON at its defaults (melee_swing_return_ms 1200, melee_swing_return_deg 60 -> cos 0.5). Every
// test that does not set Feed::back feeds a ZERO back vector, which the rule must treat as "unknown - never refuse".
constexpr float kReturnMs = 1200.0f;
constexpr float kReturnCos = 0.5f;
static Params P() { return { 1.0f, 2.5f, 1.0f, 40.0f, 600.0f, kMinTravel, kReturnMs, kReturnCos }; }
static Params POff() { return { 1.0f, 2.5f, 1.0f, 40.0f, 600.0f, kMinTravel, 0.0f, kReturnCos }; }
// Option A (Jay 2026-09-30): melee_swing_refuse_toward_body = 1 - ANY stroke inside the cone is refused, no window.
static Params PTow() { return { 1.0f, 2.5f, 1.0f, 40.0f, 600.0f, kMinTravel, kReturnMs, kReturnCos, true }; }
constexpr double kDt = 1000.0 / 90.0;   // a 90 Hz frame, ms

// Feeds a path at 90 Hz. A +/-0.2 mm alternating jitter is always added so a "still" hand is not a string of
// bit-identical repeats (a real tracked controller never is).
struct Feed {
    Detector d;
    double t = 0.0;
    Vec3 p{ 0.2f, 1.2f, -0.3f };
    int frame = 0;
    int fires = 0;
    int ends = 0;
    int dropped = 0;
    float fireTravel = -1.0f;   // the LAST FIRE step's travelM
    Step lastDrop{};            // the LAST dropped-candidate step (fix round 1: reason, travel, peak, duration)
    std::vector<End> reasons;
    Vec3 back{ 0.0f, 0.0f, 0.0f };   // "toward the body" in the Feed's space; zero = unknown (every older test)
    bool ruleOff = false;             // true = melee_swing_return_ms 0
    bool towardOn = false;            // true = melee_swing_refuse_toward_body 1 (Option A)
    Step Go(const Vec3& velMps, int n, bool holster = false, bool poseOk = true)
    {
        Step st{};
        for (int i = 0; i < n; ++i) {
            t += kDt;
            const float j = (frame++ & 1) ? 0.0002f : -0.0002f;
            p = { p.x + velMps.x * static_cast<float>(kDt / 1000.0) + j,
                  p.y + velMps.y * static_cast<float>(kDt / 1000.0),
                  p.z + velMps.z * static_cast<float>(kDt / 1000.0) };
            st = d.Update({ t, p, poseOk, holster, back }, towardOn ? PTow() : (ruleOff ? POff() : P()));
            if (st.fired) { ++fires; fireTravel = st.travelM; }
            if (st.ended) { ++ends; reasons.push_back(st.reason); }
            if (st.dropped) { ++dropped; lastDrop = st; }
        }
        return st;
    }
};

static const Vec3 kStill{ 0.0f, 0.0f, 0.0f };
static const Vec3 kSlashR{ 4.0f, 0.0f, 0.0f };

// Fix round 1 (Minor 1): feeds kSlashR one frame at a time until the swing FIRES, so tests about what happens AFTER
// a FIRE do not depend on how many frames minTravelM takes. Capped at 30 frames; the FIRE itself is CHECKed.
static void SlashUntilFire(Feed& f)
{
    const int before = f.fires;
    for (int i = 0; i < 30 && f.fires == before; ++i) { f.Go(kSlashR, 1); }
    CHECK(f.fires == before + 1);
}

static void still_hand_never_fires()
{
    Feed f;
    const Step st = f.Go(kStill, 200);
    CHECK(f.fires == 0);
    CHECK(st.state == State::Armed);
}

static void one_slash_fires_once_and_ends_slow()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go(kSlashR, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
    CHECK(f.ends == 1);
    CHECK(!f.reasons.empty() && f.reasons[0] == End::Slow);
}

static void starting_in_motion_never_fires_until_armed()
{
    Feed f;
    f.Go(kSlashR, 30);
    CHECK(f.fires == 0);
    CHECK(f.d.state() == State::Idle);
    f.Go(kStill, 20);
    f.Go(kSlashR, 20);
    CHECK(f.fires == 1);
}

static void no_verdict_before_window_span()
{
    Detector d;
    const Step a = d.Update({ 0.0, { 0.0f, 1.0f, 0.0f }, true, false }, P());
    const Step b = d.Update({ 11.0, { 0.2f, 1.0f, 0.0f }, true, false }, P());   // 18 m/s if it were judged
    CHECK(!a.speedValid && !b.speedValid);
    CHECK(!a.fired && !b.fired);
}

static void repeated_samples_do_not_split_or_end_a_swing()
{
    Feed f;
    f.Go(kStill, 20);
    // 30 frames: every other frame is a bit-identical repeat; the real frames move two frames' worth (4 m/s mean).
    int endsDuring = 0;
    for (int i = 0; i < 30; ++i) {
        f.t += kDt;
        if ((i & 1) == 0) {
            f.p = { f.p.x + 4.0f * static_cast<float>(2.0 * kDt / 1000.0), f.p.y, f.p.z };
        }
        const Step st = f.d.Update({ f.t, f.p, true, false }, P());
        if (st.fired) { ++f.fires; }
        if (st.ended) { ++endsDuring; }
        if ((i & 1) == 1) { CHECK(st.repeat); }
    }
    CHECK(f.fires == 1);
    CHECK(endsDuring == 0);
    f.Go(kStill, 30);
    CHECK(f.ends == 1);
}

static void reversal_is_two_swings()
{
    // A fast back-and-forth is TWO swings (spec §2.1). Measured on the real header (Task 1 implementer, instrumented):
    // a symmetric turn-around passes through a window whose net displacement is ~0 (+3.0, then 0.0 m/s), so the
    // swing normally ends SLOW before the velocity reverses. Either END re-arms (endSpeed == armSpeed), and the
    // return stroke then FIRES - that is what this test pins. The END label of the first swing is not the claim.
    Feed f;
    f.Go(kStill, 20);
    f.Go({ 6.0f, 0.0f, 0.0f }, 15);
    f.Go({ -6.0f, 0.0f, 0.0f }, 15);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
    CHECK(f.reasons.size() == 2);
    CHECK(!f.reasons.empty() && (f.reasons[0] == End::Slow || f.reasons[0] == End::Reversal));
}

static void max_duration_ends_and_does_not_refire_while_moving()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go({ 3.0f, 0.0f, 0.0f }, 90);   // 1 s of continuous motion
    CHECK(f.fires == 1);
    CHECK(f.ends == 1 && f.reasons[0] == End::MaxDuration);
    CHECK(f.d.state() == State::Idle);
}

static void holster_zone_never_fires_and_ends_active()
{
    Feed f;
    f.Go(kStill, 20, true);
    f.Go(kSlashR, 25, true);
    CHECK(f.fires == 0);

    Feed g;
    g.Go(kStill, 20);
    SlashUntilFire(g);
    const Step st = g.Go(kSlashR, 1, true);
    CHECK(st.ended && st.reason == End::Holster && st.holsterRefused);
}

static void pose_lost_ends_swing_and_needs_rearm()
{
    Feed f;
    f.Go(kStill, 20);
    SlashUntilFire(f);
    const Step st = f.Go(kSlashR, 1, false, false);
    CHECK(st.ended && st.reason == End::PoseLost);
    f.Go(kSlashR, 20);   // comes back already moving
    CHECK(f.fires == 1);
}

static void non_finite_is_refused_like_pose_loss()
{
    Feed f;
    f.Go(kStill, 20);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    f.t += kDt;
    const Step st = f.d.Update({ f.t, { nan, 1.0f, 0.0f }, true, false }, P());
    CHECK(st.nonFinite && !st.fired);
    f.Go(kStill, 20);
    f.Go(kSlashR, 20);
    CHECK(f.fires == 1);
}

static void blocked_ends_swing_and_needs_rearm()
{
    Feed f;
    f.Go(kStill, 20);
    SlashUntilFire(f);
    f.d.Block();
    const Step st = f.Go(kSlashR, 1);
    CHECK(st.ended && st.reason == End::Blocked);
    f.Go(kSlashR, 10);
    CHECK(f.fires == 1);
}

// Final-review I2: Block() must end the swing even when the very next sample is a bit-identical repeat - otherwise
// the swing stays Active and the driver calls the hit again after a wall strike.
static void blocked_ends_on_a_repeat_frame()
{
    Feed f;
    f.Go(kStill, 20);
    SlashUntilFire(f);
    CHECK(f.d.state() == State::Active);
    f.d.Block();
    f.t += kDt;
    const Step st = f.d.Update({ f.t, f.p, true, false }, P());   // same position as the last sample
    CHECK(st.repeat);
    CHECK(st.ended && st.reason == End::Blocked);
    CHECK(f.d.state() != State::Active);
}

// Final-review I2: a swing whose samples turn into bit-identical repeats still ends at maxMs.
static void repeats_crossing_max_duration_end_the_swing()
{
    Feed f;
    f.Go(kStill, 20);
    SlashUntilFire(f);
    CHECK(f.d.state() == State::Active);
    Step st{};
    bool allRepeats = true;
    for (int i = 0; i < 200; ++i) {   // 200 frames = 2.2 s, far past maxMs
        f.t += kDt;
        st = f.d.Update({ f.t, f.p, true, false }, P());
        if (!st.repeat) { allRepeats = false; }
        if (st.ended) { break; }
    }
    CHECK(allRepeats);
    CHECK(st.ended && st.reason == End::MaxDuration);
    CHECK(st.durMs >= 600.0f && st.durMs < 600.0f + static_cast<float>(kDt) + 0.01f);
    CHECK(f.d.state() == State::Idle);
}

static void gap_invalidates_speed()
{
    Feed f;
    f.Go(kStill, 20);
    f.t += 200.0;   // a 200 ms stall, then a 1 m jump
    const Step st = f.d.Update({ f.t, { f.p.x + 1.0f, f.p.y, f.p.z }, true, false }, P());
    CHECK(!st.speedValid && !st.fired);
}

// B1 rebound suppression (Stage 1 run 1: 11 FIRE for 5 slashes). A real slash, then the arm's recoil the other way.
// Fix round 1 (Minor 2, the reviewer's arithmetic): with this gentle -4.5 m/s recoil the 44 ms turn-around window dips
// below endSpeed first, so the slash ends SLOW (-> Armed, below armSpeed), not reversal; the recoil then crosses
// fireSpeed and starts a candidate whose path (~0.2 m) never reaches minTravelM - dropped. The reversal-END case is
// reversal_end_then_rebound_fires_once below.
static void slash_then_rebound_fires_once()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go({ 6.0f, 0.0f, 0.0f }, 13);    // ~0.87 m
    f.Go({ -4.5f, 0.0f, 0.0f }, 4);    // ~0.2 m - the rebound
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
    CHECK(f.ends == 1);
    CHECK(f.dropped >= 1);
}

// Fix round 1 (Minor 2): the Stage 1 run 1 shape - a real stroke ends REVERSAL (immediate re-arm) and a sharp recoil
// crosses fireSpeed at once. Arithmetic (4-frame, 44 ms window): +6 x13 then -9 m/s: window +2.25 (continue), -1.5 (>= endSpeed,
// opposite vFire -> REVERSAL -> Armed), -5.25 (candidate), then still -> dropped slow at ~0.1 m of path.
static void reversal_end_then_rebound_fires_once()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go({ 6.0f, 0.0f, 0.0f }, 13);    // ~0.87 m, fires
    f.Go({ -9.0f, 0.0f, 0.0f }, 3);    // ~0.3 m of sharp recoil
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
    CHECK(!f.reasons.empty() && f.reasons[0] == End::Reversal);
    CHECK(f.dropped >= 1);
    CHECK(f.lastDrop.dropWhy == Drop::Slow);
    CHECK(f.lastDrop.travelM > 0.0f && f.lastDrop.travelM < kMinTravel);
}

static void short_flick_never_fires()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go({ 5.0f, 0.0f, 0.0f }, 5);     // ~0.28 m
    f.Go(kStill, 30);
    CHECK(f.fires == 0);
    CHECK(f.ends == 0);
    CHECK(f.dropped >= 1);
    // Fix round 1 (Minor 8): the dropped candidate's own numbers, for the [SWING] candidate dropped line.
    CHECK(f.lastDrop.dropWhy == Drop::Slow);
    CHECK(std::string(DropName(f.lastDrop.dropWhy)) == "slow");
    CHECK(f.lastDrop.travelM > 0.2f && f.lastDrop.travelM < kMinTravel);
    CHECK(f.lastDrop.peak >= P().fireSpeed);
    CHECK(f.lastDrop.durMs > 0.0f && f.lastDrop.durMs < 200.0f);
}

// Fix round 1 (Minor 8): the driver's relatch drops a pending candidate through Abandon(), with the same numbers.
static void abandon_drops_a_pending_candidate_only()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go(kSlashR, 4);                   // a candidate, well under minTravelM
    CHECK(f.d.state() == State::Candidate);
    const Step a = f.d.Abandon(f.t + kDt);
    CHECK(a.dropped && a.dropWhy == Drop::Relatch && !a.fired && !a.ended);
    CHECK(a.travelM > 0.0f && a.travelM < kMinTravel && a.durMs > 0.0f);
    CHECK(f.d.state() == State::Idle);
    Feed g;
    g.Go(kStill, 20);                   // Armed, no candidate: nothing to drop
    const Step b = g.d.Abandon(g.t + kDt);
    CHECK(!b.dropped && b.dropWhy == Drop::None);
}

static void long_stroke_fires_once_at_travel()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go(kSlashR, 30);
    CHECK(f.fires == 1);
    // FIRE is the FIRST sample whose path reaches minTravelM, so it overshoots by at most one frame's step: the
    // stroke's own step plus the Feed's 0.2 mm jitter, plus float rounding.
    const float oneFrame = kSlashR.x * static_cast<float>(kDt / 1000.0) + 0.0002f;
    CHECK(f.fireTravel >= kMinTravel);
    CHECK(f.fireTravel <= kMinTravel + oneFrame + 1e-4f);
}

// Brief: "blocked during a candidate -> drop it" (no FIRE, no END), next state Idle like a Blocked END - so a hand
// still moving after the drop needs a re-arm before anything fires.
static void blocked_candidate_is_dropped_not_fired()
{
    Feed f;
    f.Go(kStill, 20);
    f.Go(kSlashR, 4);                   // candidate started (~3 m/s window), path well under minTravelM
    CHECK(f.d.state() == State::Candidate);
    f.d.Block();
    const Step st = f.Go(kSlashR, 1);
    CHECK(st.dropped && !st.fired && !st.ended);
    CHECK(st.dropWhy == Drop::Blocked);
    CHECK(f.d.state() == State::Idle);
    f.Go(kSlashR, 20);                  // still moving: no re-arm, so no FIRE
    CHECK(f.fires == 0);
    CHECK(f.ends == 0);
}

// ---- Return-stroke rule (Stage 1 redo 4, Jay 2026-09-30). Measured: 9 slashes AWAY from the body (head-yaw z
// -0.91..-1.00, 5.9-6.9 m/s) and 5 returns TOWARD it (z +0.69..+0.99, 3.1-3.5 m/s, 162-913 ms after the slash's END).
// Test space: the body is at +Z, so back = (0,0,1); a slash moves -Z, a return +Z.
static const Vec3 kBack{ 0.0f, 0.0f, 1.0f };
static const Vec3 kSlashAway{ 0.0f, 0.0f, -6.0f };
static const Vec3 kReturnToward{ 0.0f, 0.0f, 3.5f };

// A slash, a short pause (the redo-4 returns started 162-913 ms after END), then a return toward the body: ONE FIRE,
// the return DROPPED with reason Return, and - next state Idle - the rest of the return cannot start a new candidate.
static void return_toward_body_after_slash_is_dropped()
{
    Feed f;
    f.back = kBack;
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);               // ~0.87 m, fires
    f.Go(kStill, 20);                   // ~220 ms: ends slow, re-arms
    CHECK(f.fires == 1 && f.ends == 1);
    f.Go(kReturnToward, 25);            // ~0.97 m back toward the body
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
    CHECK(f.ends == 1);
    CHECK(f.dropped == 1);
    CHECK(f.lastDrop.dropWhy == Drop::Return);
    CHECK(std::string(DropName(f.lastDrop.dropWhy)) == "return");
}

// The slash AFTER a refused return still fires - the refused return never becomes "the previous swing".
static void slash_after_refused_return_fires()
{
    Feed f;
    f.back = kBack;
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
    CHECK(f.ends == 2);
}

// A stroke toward the body LATER than melee_swing_return_ms after the last END is a swing, not a return.
static void toward_body_after_the_window_fires()
{
    Feed f;
    f.back = kBack;
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 150);                  // ~1.67 s > 1200 ms
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
}

// D-155's concern: a genuine fast BACKHAND (opposite the last swing, but sideways - not toward the body) must fire.
static void backhand_sideways_is_not_a_return()
{
    Feed f;
    f.back = kBack;
    f.Go(kStill, 20);
    f.Go({ 6.0f, 0.0f, 0.0f }, 13);
    f.Go(kStill, 10);
    f.Go({ -6.0f, 0.0f, 0.0f }, 13);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
    CHECK(f.dropped == 0 || f.lastDrop.dropWhy != Drop::Return);
}

// A stroke toward the body with no previous swing at all (nothing to return FROM) fires.
static void toward_body_with_no_previous_swing_fires()
{
    Feed f;
    f.back = kBack;
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
}

// melee_swing_return_ms = 0 turns the rule off: the redo-4 shape fires twice, as before this rule.
static void return_rule_off_fires_the_return()
{
    Feed f;
    f.back = kBack;
    f.ruleOff = true;
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
}

// An unknown body direction (zero back vector - no head pose) never refuses: fail OPEN to the pre-rule behaviour.
static void unknown_back_never_refuses()
{
    Feed f;                              // back stays zero
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
}

// ---- Option A (Jay 2026-09-30): strokes toward the body NEVER fire. Evidence: redo 4 + 5, all 6 toward-body FIREs were
// returns or a wind-up; 18 slashes + 15 stabs (stab run) had toward-body <= -0.39.

// The redo-5 wind-up: toward the body 2.9 s after the last swing - outside the window, so the window rule let it FIRE.
static void toward_on_refuses_a_late_wind_up()
{
    Feed f;
    f.back = kBack;
    f.towardOn = true;
    f.Go(kStill, 20);
    f.Go(kSlashAway, 13);
    f.Go(kStill, 260);                  // ~2.9 s
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
    CHECK(f.dropped >= 1);
    CHECK(f.lastDrop.dropWhy == Drop::TowardBody);
    CHECK(std::string(DropName(f.lastDrop.dropWhy)) == "toward-body");
}

// With no previous swing at all - the window rule's case toward_body_with_no_previous_swing_fires - Option A refuses.
static void toward_on_refuses_with_no_previous_swing()
{
    Feed f;
    f.back = kBack;
    f.towardOn = true;
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 0);
    CHECK(f.lastDrop.dropWhy == Drop::TowardBody);
}

// The stab run's shapes: straight forward, forward-right (block B), forward-left (block C, toward-body -0.39..-0.66).
// All must fire under Option A.
static void toward_on_stabs_fire()
{
    const Vec3 stabs[3] = { { 0.0f, 0.0f, -5.0f }, { 4.0f, 0.0f, -3.0f }, { -4.6f, 0.0f, -2.0f } };
    for (const Vec3& v : stabs) {
        Feed f;
        f.back = kBack;
        f.towardOn = true;
        f.Go(kStill, 20);
        f.Go(v, 13);
        f.Go(kStill, 30);
        CHECK(f.fires == 1);
    }
}

// D-155: a fast sideways backhand must still fire with Option A on (sideways = toward-body 0, outside the cone).
static void toward_on_backhand_fires()
{
    Feed f;
    f.back = kBack;
    f.towardOn = true;
    f.Go(kStill, 20);
    f.Go({ 6.0f, 0.0f, 0.0f }, 13);
    f.Go(kStill, 10);
    f.Go({ -6.0f, 0.0f, 0.0f }, 13);
    f.Go(kStill, 30);
    CHECK(f.fires == 2);
}

// Option A with no head pose (zero back) still never refuses - fail OPEN, like the window rule.
static void toward_on_unknown_back_never_refuses()
{
    Feed f;
    f.towardOn = true;
    f.Go(kStill, 20);
    f.Go(kReturnToward, 25);
    f.Go(kStill, 30);
    CHECK(f.fires == 1);
}

// BackInApp is the head-yaw frame's +Z ("behind") expressed in app space: InHeadYawFrame maps it to (0,0,1).
static void back_in_app_math()
{
    CHECK(NearV(BackInApp(0.0f, 0.0f, 0.0f, 1.0f), { 0.0f, 0.0f, 1.0f }));
    const float s = std::sin(0.785398163f);
    const float c = std::cos(0.785398163f);
    const Vec3 b = BackInApp(0.0f, s, 0.0f, c);   // head yawed +90 about Y
    CHECK(NearV(InHeadYawFrame(0.0f, s, 0.0f, c, b), { 0.0f, 0.0f, 1.0f }));
    CHECK(Near(Len(b), 1.0f));
}

static void blade_direction_math()
{
    const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    CHECK(NearV(BladeDirWorld(ident, 0.0f), { 0.0f, 0.0f, -1.0f }));
    CHECK(NearV(BladeDirWorld(ident, 90.0f), { 0.0f, 1.0f, 0.0f }));
    // Ry(+90), row-major: [[0,0,1],[0,1,0],[-1,0,0]]. Ry(90)*(0,0,-1) = (-1,0,0).
    const float ry90[9] = { 0, 0, 1, 0, 1, 0, -1, 0, 0 };
    CHECK(NearV(BladeDirWorld(ry90, 0.0f), { -1.0f, 0.0f, 0.0f }));
}

static void quaternion_rotate_and_head_yaw_frame()
{
    const float s = std::sin(0.785398163f);   // half of 90 deg
    const float c = std::cos(0.785398163f);
    CHECK(NearV(Rotate(0.0f, s, 0.0f, c, { 0.0f, 0.0f, -1.0f }), { -1.0f, 0.0f, 0.0f }));
    // head yawed +90 about Y: world (-1,0,0) is straight ahead in the head-yaw frame
    CHECK(NearV(InHeadYawFrame(0.0f, s, 0.0f, c, { -1.0f, 0.0f, 0.0f }), { 0.0f, 0.0f, -1.0f }));
}

// Driver gate (threads 7, 2026-09-30): a second entry while held is REFUSED and COUNTED - never waits - and the gate
// admits again once left. Deterministic, single thread: this is the test that proves the overlap counter CAN fire.
static void gate_refuses_and_counts_while_held()
{
    Gate g;
    CHECK(g.Overlaps() == 0);
    CHECK(g.TryEnter());
    CHECK(!g.TryEnter());
    CHECK(!g.TryEnter());
    CHECK(g.Overlaps() == 2);
    g.Leave();
    CHECK(g.TryEnter());
    CHECK(g.Overlaps() == 2);   // an admitted entry is not an overlap
    g.Leave();
}

// Four threads hammer one gate. Mutual exclusion: no admitted entry ever finds another inside. Accounting: every
// attempt is either admitted or counted as an overlap - nothing is lost.
static void gate_excludes_across_threads()
{
    Gate g;
    std::atomic<int> inside{ 0 };
    std::atomic<long> violations{ 0 };
    std::atomic<long> admitted{ 0 };
    constexpr int kThreads = 4;
    constexpr int kIters = 200000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                if (!g.TryEnter()) { continue; }
                if (inside.fetch_add(1) != 0) { violations.fetch_add(1); }
                admitted.fetch_add(1);
                inside.fetch_sub(1);
                g.Leave();
            }
        });
    }
    for (auto& th : ts) { th.join(); }
    CHECK(violations.load() == 0);
    CHECK(admitted.load() + g.Overlaps() == static_cast<long>(kThreads) * kIters);
    CHECK(admitted.load() > 0);
}

// HoldLatch (Jay 2026-10-27: a body thrown dismembered on release; "gate the grab too"): a swing that began while a body grab
// or a zombie grab was on keeps its hit off to its END - releasing mid-swing does not open the gate. The next swing is normal.
static Step At(State s, bool fired = false, bool ended = false)
{
    Step st;
    st.state = s;
    st.fired = fired;
    st.ended = ended;
    return st;
}

static void hold_latch_keeps_a_held_swing_off_after_the_release()
{
    HoldLatch l;
    // the control: no hold at all - every Active frame may hit
    CHECK(HoldLatchStep(l, false, At(State::Active, true)));
    CHECK(HoldLatchStep(l, false, At(State::Active)));
    CHECK(HoldLatchStep(l, false, At(State::Armed, false, true)));
    // FIRE while holding, released two frames later, the swing runs on: no hit until its END
    CHECK(HoldLatchStep(l, false, At(State::Armed)));               // between swings: allowed (B1Frame ignores non-Active anyway)
    CHECK(!HoldLatchStep(l, true, At(State::Active, true)));        // FIRE, holding
    CHECK(!HoldLatchStep(l, true, At(State::Active)));
    CHECK(!HoldLatchStep(l, false, At(State::Active)));             // released: still off
    CHECK(!HoldLatchStep(l, false, At(State::Active)));
    CHECK(!HoldLatchStep(l, false, At(State::Armed, false, true))); // END frame
    // the next swing, nothing held: hits again
    CHECK(HoldLatchStep(l, false, At(State::Candidate)));
    CHECK(HoldLatchStep(l, false, At(State::Active, true)));
    CHECK(HoldLatchStep(l, false, At(State::Armed, false, true)));
    // a CANDIDATE begun while holding that FIREs after the release is the same throw: off to its END
    CHECK(!HoldLatchStep(l, true, At(State::Candidate)));
    CHECK(!HoldLatchStep(l, false, At(State::Active, true)));
    CHECK(!HoldLatchStep(l, false, At(State::Armed, false, true)));
    // a candidate begun while holding and DROPPED (no FIRE, no END): the latch clears with it
    CHECK(!HoldLatchStep(l, true, At(State::Candidate)));
    CHECK(HoldLatchStep(l, false, At(State::Armed)));
    CHECK(HoldLatchStep(l, false, At(State::Active, true)));
    // a hold that begins mid-swing (grabbed during a swing): off from then to its END
    CHECK(!HoldLatchStep(l, true, At(State::Active)));
    CHECK(!HoldLatchStep(l, false, At(State::Active)));
    CHECK(!HoldLatchStep(l, false, At(State::Idle, false, true)));
    CHECK(HoldLatchStep(l, false, At(State::Active, true)));
}

// The light stagger (INV-639): the game's damage reaction is controller group 30 and stops the melee controller; only that group
// feeds the swing from the inactive update (Jay 2026-10-27: "It should count" -> "Just light"). The knockdown (16), the melee
// default (1), a grab or any other group never does; an unreadable group (-1 / out of range) never does.
static void stagger_feed_only_in_the_light_reaction_group()
{
    static_assert(kLightStaggerGroup == 30, "the damage reaction's controller group (INV-639)");
    CHECK(FeedFromInactive(30));
    CHECK(!FeedFromInactive(16));   // knockdown - Jay: "Just light"
    CHECK(!FeedFromInactive(1));    // the melee default - slot 16 runs there
    CHECK(!FeedFromInactive(31));   // the neighbouring groups 31-33 are other controllers (37 / 38 / 42)
    CHECK(!FeedFromInactive(-1));
    CHECK(!FeedFromInactive(0x7FFFFFFF));
}

int main()
{
    stagger_feed_only_in_the_light_reaction_group();
    hold_latch_keeps_a_held_swing_off_after_the_release();
    still_hand_never_fires();
    one_slash_fires_once_and_ends_slow();
    starting_in_motion_never_fires_until_armed();
    no_verdict_before_window_span();
    repeated_samples_do_not_split_or_end_a_swing();
    reversal_is_two_swings();
    max_duration_ends_and_does_not_refire_while_moving();
    holster_zone_never_fires_and_ends_active();
    pose_lost_ends_swing_and_needs_rearm();
    non_finite_is_refused_like_pose_loss();
    blocked_ends_swing_and_needs_rearm();
    blocked_ends_on_a_repeat_frame();
    repeats_crossing_max_duration_end_the_swing();
    gap_invalidates_speed();
    slash_then_rebound_fires_once();
    reversal_end_then_rebound_fires_once();
    short_flick_never_fires();
    abandon_drops_a_pending_candidate_only();
    long_stroke_fires_once_at_travel();
    blocked_candidate_is_dropped_not_fired();
    return_toward_body_after_slash_is_dropped();
    slash_after_refused_return_fires();
    toward_body_after_the_window_fires();
    backhand_sideways_is_not_a_return();
    toward_body_with_no_previous_swing_fires();
    return_rule_off_fires_the_return();
    unknown_back_never_refuses();
    toward_on_refuses_a_late_wind_up();
    toward_on_refuses_with_no_previous_swing();
    toward_on_stabs_fire();
    toward_on_backhand_fires();
    toward_on_unknown_back_never_refuses();
    back_in_app_math();
    blade_direction_math();
    quaternion_rotate_and_head_yaw_frame();
    gate_refuses_and_counts_while_held();
    gate_excludes_across_threads();
    std::printf(g_fail ? "FAILED: %d failure(s)\n" : "PASSED: %d failure(s)\n", g_fail);
    return g_fail;
}
