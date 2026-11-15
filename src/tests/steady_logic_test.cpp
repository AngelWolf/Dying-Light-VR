// Standalone unit test for src/src/ovr2xr/steady_logic.h - NOT part of the DLL build.
// docs/SPEC-VR-COMFORT-2026-09-30.md §6 (C1, hand steadiness). Exit code = number of failures.
#include "steady_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::steady;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const float kDt = 1.0f / 90.0f;   // a 90 Hz headset frame
static const Q kIdent{ 0.0f, 0.0f, 0.0f, 1.0f };

static Q Yaw(float deg)
{
    const float h = deg * 0.00872664626f;
    return { 0.0f, std::sin(h), 0.0f, std::cos(h) };
}
static float YawDeg(const Q& q) { return 2.0f * std::atan2(q.y, q.w) * 57.2957795f; }
static float AngleBetweenDeg(const Q& a, const Q& b)
{
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0f) { d = 1.0f; }
    return 2.0f * std::acos(d) * 57.2957795f;
}
static float Norm(const Q& q) { return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); }

static void first_sample_passes_through()
{
    PoseFilter f;
    V3 op; Q oq;
    f.Update({ 0.1f, 1.2f, -0.3f }, Yaw(20.0f), kDt, FromSliders(50, 50), op, oq);
    CHECK(op.x == 0.1f && op.y == 1.2f && op.z == -0.3f);
    CHECK(std::fabs(YawDeg(oq) - 20.0f) < 1e-3f);
}
static void constant_input_stays_constant()
{
    PoseFilter f;
    V3 op; Q oq;
    for (int i = 0; i < 200; ++i) { f.Update({ 0.2f, 1.0f, -0.4f }, Yaw(10.0f), kDt, FromSliders(50, 50), op, oq); }
    CHECK(std::fabs(op.x - 0.2f) < 1e-5f && std::fabs(op.z + 0.4f) < 1e-5f);
    CHECK(std::fabs(YawDeg(oq) - 10.0f) < 1e-3f);
}
// Held still with tracking noise: +-1 mm alternating every frame. The output's swing must be well under the input's.
static float PositionJitterOut(int strength)
{
    PoseFilter f;
    V3 op; Q oq;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 400; ++i) {
        const float x = (i & 1) ? 0.001f : -0.001f;
        f.Update({ x, 1.0f, 0.0f }, kIdent, kDt, FromSliders(strength, 50), op, oq);
        if (i >= 200) { lo = op.x < lo ? op.x : lo; hi = op.x > hi ? op.x : hi; }
    }
    return hi - lo;   // input peak-to-peak is 0.002
}
static void still_position_jitter_is_reduced()
{
    CHECK(PositionJitterOut(50) < 0.3f * 0.002f);
    CHECK(PositionJitterOut(100) < PositionJitterOut(0));   // stronger = steadier
}
static float RotationJitterOut(int strength)
{
    PoseFilter f;
    V3 op; Q oq;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 400; ++i) {
        f.Update({ 0.0f, 1.0f, 0.0f }, Yaw((i & 1) ? 0.2f : -0.2f), kDt, FromSliders(strength, 50), op, oq);
        if (i >= 200) { const float y = YawDeg(oq); lo = y < lo ? y : lo; hi = y > hi ? y : hi; }
    }
    return hi - lo;   // input peak-to-peak is 0.4 deg
}
static void still_rotation_jitter_is_reduced()
{
    CHECK(RotationJitterOut(50) < 0.3f * 0.4f);
    CHECK(RotationJitterOut(100) < RotationJitterOut(0));
}
// A deliberate move at 1 m/s: the output must keep up (the "response" half of the 1-euro filter).
static float PositionLagAt1ms(int response)
{
    PoseFilter f;
    V3 op; Q oq;
    float x = 0.0f;
    for (int i = 0; i < 60; ++i) {
        x += 1.0f * kDt;
        f.Update({ x, 1.0f, 0.0f }, kIdent, kDt, FromSliders(50, response), op, oq);
    }
    return x - op.x;
}
static void fast_move_has_small_lag()
{
    CHECK(PositionLagAt1ms(50) < 0.020f);                      // under 2 cm at 1 m/s
    CHECK(PositionLagAt1ms(100) < PositionLagAt1ms(0));        // more response = less lag
}
static float RotationLagAt180dps(int response)
{
    PoseFilter f;
    V3 op; Q oq;
    float yaw = 0.0f;
    for (int i = 0; i < 45; ++i) {   // half a second, 90 deg total
        yaw += 180.0f * kDt;
        f.Update({ 0.0f, 1.0f, 0.0f }, Yaw(yaw), kDt, FromSliders(50, response), op, oq);
    }
    return yaw - YawDeg(oq);
}
static void fast_turn_has_small_lag()
{
    CHECK(RotationLagAt180dps(50) < 4.0f);
    CHECK(RotationLagAt180dps(50) > 0.0f);                     // it IS a filter
    CHECK(RotationLagAt180dps(100) < RotationLagAt180dps(0));
}
// q and -q are the same rotation; a runtime may hand either. The output must not swing through 360.
static void hemisphere_flip_is_not_a_rotation()
{
    PoseFilter f;
    V3 op; Q oq;
    const Q a = Yaw(30.0f);
    const Q b{ -a.x, -a.y, -a.z, -a.w };
    for (int i = 0; i < 100; ++i) { f.Update({ 0.0f, 1.0f, 0.0f }, (i & 1) ? b : a, kDt, FromSliders(50, 50), op, oq); }
    CHECK(AngleBetweenDeg(oq, a) < 0.01f);
}
// Slerp on its own: halfway from a to -a (the same rotation) is a, not a 180-degree swing through identity.
static void slerp_takes_the_short_way()
{
    const Q a = Yaw(40.0f);
    const Q m = Slerp(a, { -a.x, -a.y, -a.z, -a.w }, 0.5f);
    CHECK(AngleBetweenDeg(m, a) < 0.01f);
    const Q h = Slerp(Yaw(0.0f), Yaw(90.0f), 0.5f);
    CHECK(std::fabs(YawDeg(h) - 45.0f) < 0.01f);
}
static void output_quaternion_is_unit()
{
    PoseFilter f;
    V3 op; Q oq;
    for (int i = 0; i < 100; ++i) { f.Update({ 0.0f, 1.0f, 0.0f }, Yaw(static_cast<float>(i) * 3.0f), kDt, FromSliders(80, 20), op, oq); }
    CHECK(std::fabs(Norm(oq) - 1.0f) < 1e-4f);
}
// A long gap (tracking lost, a stall, a pause) must not blend from a stale pose: the first sample after it is raw.
static void long_gap_resets_to_raw()
{
    PoseFilter f;
    V3 op; Q oq;
    for (int i = 0; i < 50; ++i) { f.Update({ 0.0f, 1.0f, 0.0f }, kIdent, kDt, FromSliders(50, 50), op, oq); }
    f.Update({ 0.5f, 1.0f, 0.0f }, Yaw(45.0f), kMaxDt + 0.01f, FromSliders(50, 50), op, oq);
    CHECK(op.x == 0.5f);
    CHECK(std::fabs(YawDeg(oq) - 45.0f) < 1e-3f);
}
// A second locate for the same display time (dt 0) must not divide by zero or move the output.
static void zero_dt_holds_output()
{
    PoseFilter f;
    V3 op; Q oq;
    f.Update({ 0.0f, 1.0f, 0.0f }, kIdent, kDt, FromSliders(50, 50), op, oq);
    f.Update({ 0.3f, 1.0f, 0.0f }, Yaw(30.0f), 0.0f, FromSliders(50, 50), op, oq);
    CHECK(op.x == 0.0f && std::isfinite(op.x));
    CHECK(AngleBetweenDeg(oq, kIdent) < 1e-3f);
}
static void reset_forgets_state()
{
    PoseFilter f;
    V3 op; Q oq;
    for (int i = 0; i < 50; ++i) { f.Update({ 0.0f, 1.0f, 0.0f }, kIdent, kDt, FromSliders(50, 50), op, oq); }
    f.Reset();
    f.Update({ 0.7f, 1.0f, 0.0f }, kIdent, kDt, FromSliders(50, 50), op, oq);
    CHECK(op.x == 0.7f);
}
static void slider_mapping()
{
    const Params lo = FromSliders(0, 50), mid = FromSliders(50, 50), hi = FromSliders(100, 50);
    CHECK(std::fabs(lo.minCutoffHz - 5.0f) < 1e-3f);
    CHECK(std::fabs(hi.minCutoffHz - 0.3f) < 1e-3f);
    CHECK(mid.minCutoffHz < lo.minCutoffHz && mid.minCutoffHz > hi.minCutoffHz);
    CHECK(FromSliders(50, 100).betaPos > FromSliders(50, 0).betaPos);
    CHECK(FromSliders(-20, 500).minCutoffHz == hi.minCutoffHz || FromSliders(-20, 500).minCutoffHz == lo.minCutoffHz);
}

// MELEE (Jay 2026-09-30: "VERY SLIGHT ... to stop the most MINUTE of hand movements"). The swing's hit sweep reads the
// rig's solve, so at swing speed the melee filter must be as good as raw.
static void melee_mapping_is_the_max_response_curve()
{
    CHECK(MeleeFromSlider(15).minCutoffHz == FromSliders(15, 100).minCutoffHz);
    CHECK(MeleeFromSlider(15).betaPos == FromSliders(15, 100).betaPos);
    CHECK(MeleeFromSlider(kMeleeDefaultStrength).minCutoffHz > FromSliders(50, 50).minCutoffHz);   // lighter than the gun's
}
static float MeleeStillJitterOut(int strength)
{
    PoseFilter f;
    V3 op; Q oq;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 400; ++i) {
        f.Update({ (i & 1) ? 0.001f : -0.001f, 1.0f, 0.0f }, kIdent, kDt, MeleeFromSlider(strength), op, oq);
        if (i >= 200) { lo = op.x < lo ? op.x : lo; hi = op.x > hi ? op.x : hi; }
    }
    return hi - lo;   // input 0.002
}
static void melee_still_hand_is_steadied_slightly()
{
    const float m = MeleeStillJitterOut(kMeleeDefaultStrength);
    CHECK(m < 0.8f * 0.002f);                 // it does something
    CHECK(m > PositionJitterOut(50));         // but less than the gun's filter
}
// A swing: from rest, accelerate to 4 m/s and ~860 deg/s (15 rad/s) over 60 ms, then hold that for 45 ms - the stretch the
// sweep samples around a FIRE. Lag measured at the end.
static void melee_swing_is_nearly_raw()
{
    PoseFilter f;
    V3 op; Q oq;
    for (int i = 0; i < 90; ++i) { f.Update({ 0.0f, 1.0f, 0.0f }, kIdent, kDt, MeleeFromSlider(kMeleeDefaultStrength), op, oq); }
    float x = 0.0f, yawDeg = 0.0f, t = 0.0f;
    const int steps = static_cast<int>(0.105f / kDt + 0.5f);
    for (int i = 0; i < steps; ++i) {
        t += kDt;
        const float k = t < 0.060f ? t / 0.060f : 1.0f;
        x += 4.0f * k * kDt;
        yawDeg += 860.0f * k * kDt;
        f.Update({ x, 1.0f, 0.0f }, Yaw(yawDeg), kDt, MeleeFromSlider(kMeleeDefaultStrength), op, oq);
    }
    CHECK(x - op.x < 0.005f);
    CHECK(yawDeg - YawDeg(oq) < 2.0f);
}

int main()
{
    first_sample_passes_through();
    constant_input_stays_constant();
    still_position_jitter_is_reduced();
    still_rotation_jitter_is_reduced();
    fast_move_has_small_lag();
    fast_turn_has_small_lag();
    hemisphere_flip_is_not_a_rotation();
    slerp_takes_the_short_way();
    output_quaternion_is_unit();
    long_gap_resets_to_raw();
    zero_dt_holds_output();
    reset_forgets_state();
    slider_mapping();
    melee_mapping_is_the_max_response_curve();
    melee_still_hand_is_steadied_slightly();
    melee_swing_is_nearly_raw();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail;
}
