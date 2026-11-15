// Standalone unit test for src/src/ovr2xr/twist_logic.h (the gate flick from the RIGHT CONTROLLER's twist) - NOT part of the DLL build.
// RUN RG5 (Jay: "The flick for the gate is a wrist rotation, very quick" -> "Controller twist"). Exit code = failures.
#include "twist_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::reload;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

// a quaternion (x y z w) = rotation by deg about the controller's own +Z (the handle axis), on top of a fixed hold `base`
void AboutZ(float deg, const float base[4], float out[4])
{
    const float h = deg * 3.14159265f / 360.0f;
    const float r[4] = { 0.0f, 0.0f, std::sin(h), std::cos(h) };   // base x r = turn about the LOCAL z
    out[0] = base[3] * r[0] + base[0] * r[3] + base[1] * r[2] - base[2] * r[1];
    out[1] = base[3] * r[1] - base[0] * r[2] + base[1] * r[3] + base[2] * r[0];
    out[2] = base[3] * r[2] + base[0] * r[1] - base[1] * r[0] + base[2] * r[3];
    out[3] = base[3] * r[3] - base[0] * r[0] - base[1] * r[1] - base[2] * r[2];
}

const float kBase[4] = { 0.2f, -0.3f, 0.1f, 0.927362f };   // any hold (|q| = 1)

// RIGHT = the top of the hand rolls to the right seen from behind = a NEGATIVE turn about +Z (OpenXR grip: -Z forward through the fist)
void RightTwistReadsPositive()
{
    TwistWindow w;
    float q[4];
    for (int i = 0; i <= 10; ++i) { AboutZ(-6.0f * i, kBase, q); w.Push(q, 0.010 * i); }   // 60 deg right in 0.1 s
    const TwistPeak p = w.Peak(0.15);
    CHECK(p.rightDeg > 59.0f && p.rightDeg < 61.0f);
    CHECK(p.leftDeg < 0.5f);
}

void LeftTwistReadsLeft()
{
    TwistWindow w;
    float q[4];
    for (int i = 0; i <= 10; ++i) { AboutZ(7.0f * i, kBase, q); w.Push(q, 0.010 * i); }
    const TwistPeak p = w.Peak(0.15);
    CHECK(p.leftDeg > 69.0f && p.leftDeg < 71.0f && p.rightDeg < 0.5f);
}

// a SLOW twist of the same size (turning the gun on its side) is not a flick: only 0.15 s counts
void SlowTwistIsNotInTheWindow()
{
    TwistWindow w;
    float q[4];
    for (int i = 0; i <= 60; ++i) { AboutZ(-1.5f * i, kBase, q); w.Push(q, 0.010 * i); }   // 90 deg over 0.6 s
    const TwistPeak p = w.Peak(0.15);
    CHECK(p.rightDeg < 25.0f);
}

// the pose is sampled twice a frame on the hook (INV-165 / 168 cadence): an UNCHANGED pose is not a new sample (its time would
// shrink the window's real span) - pushing a repeat changes nothing
void RepeatedPoseIsNotANewSample()
{
    TwistWindow w;
    float q[4];
    AboutZ(0.0f, kBase, q);
    w.Push(q, 0.0);
    w.Push(q, 0.004);
    CHECK(w.Count() == 1);
    AboutZ(-10.0f, kBase, q);
    w.Push(q, 0.008);
    CHECK(w.Count() == 2);
}

// a swing of the whole arm (no turn about the handle) reads no twist: swing-twist split, twist about Z only
void SwingWithoutTwistReadsNone()
{
    TwistWindow w;
    for (int i = 0; i <= 10; ++i) {
        const float h = 4.0f * i * 3.14159265f / 360.0f;   // 40 deg about the local X
        const float r[4] = { std::sin(h), 0.0f, 0.0f, std::cos(h) };
        float q[4];
        q[0] = kBase[3] * r[0] + kBase[0] * r[3] + kBase[1] * r[2] - kBase[2] * r[1];
        q[1] = kBase[3] * r[1] - kBase[0] * r[2] + kBase[1] * r[3] + kBase[2] * r[0];
        q[2] = kBase[3] * r[2] + kBase[0] * r[1] - kBase[1] * r[0] + kBase[2] * r[3];
        q[3] = kBase[3] * r[3] - kBase[0] * r[0] - kBase[1] * r[1] - kBase[2] * r[2];
        w.Push(q, 0.010 * i);
    }
    const TwistPeak p = w.Peak(0.15);
    CHECK(p.rightDeg < 1.0f && p.leftDeg < 1.0f);
}

void ClearForgetsTheTwist()
{
    TwistWindow w;
    float q[4];
    for (int i = 0; i <= 10; ++i) { AboutZ(-6.0f * i, kBase, q); w.Push(q, 0.010 * i); }
    w.Clear();
    AboutZ(-62.0f, kBase, q);
    w.Push(q, 0.11);
    CHECK(w.Peak(0.15).rightDeg < 0.5f);
}

// a twist out and back inside the window: the peak still counts (the flick happened), the net would not
void OutAndBackStillPeaks()
{
    TwistWindow w;
    float q[4];
    const float a[] = { 0, -25, -50, -55, -30, -5 };
    for (int i = 0; i < 6; ++i) { AboutZ(a[i], kBase, q); w.Push(q, 0.015 * i); }
    CHECK(w.Peak(0.15).rightDeg > 54.0f);
}

// RUN RG6: toggles read ~175 deg BOTH ways (e.g. CLOSE 21:41:31.947 "right 174 / left 179") - the swing-twist split is meaningless when
// the handle axis itself swings far. A pair whose grip Z axes are more than kTwistMaxSwingDeg apart is skipped.
void Mul(const float a[4], const float b[4], float o[4])
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void BigSwingIsNotATwist()
{
    // the RG6 artifact: the handle swung ~178 deg about its X with ~10 deg about Y (no turn about the handle) reads as a ~157 deg "twist"
    // through the swing-twist split. CONTROL (2026-11-09): with the filter off (180) this check FAILED.
    TwistWindow w;
    for (int i = 0; i <= 10; ++i) {
        const float hx = 17.8f * i * 3.14159265f / 360.0f, hy = 1.0f * i * 3.14159265f / 360.0f;
        const float rx[4] = { std::sin(hx), 0.0f, 0.0f, std::cos(hx) }, ry[4] = { 0.0f, std::sin(hy), 0.0f, std::cos(hy) };
        float r[4], q[4];
        Mul(rx, ry, r);
        Mul(kBase, r, q);
        w.Push(q, 0.010 * i);
    }
    const TwistPeak p = w.Peak(0.15);
    CHECK(p.rightDeg < 45.0f && p.leftDeg < 45.0f);   // never a flick
    CHECK(std::fabs(kTwistMaxSwingDeg - 40.0f) < 1e-6f);
}

// a real twist with a modest swing (the wrist rotates while the forearm moves a little) still counts in full
void TwistWithSmallSwingCounts()
{
    TwistWindow w;
    for (int i = 0; i <= 10; ++i) {
        const float h = 2.0f * i * 3.14159265f / 360.0f;   // 20 deg of swing over the flick
        const float r[4] = { std::sin(h), 0.0f, 0.0f, std::cos(h) };
        float q[4], q2[4];
        q[0] = kBase[3] * r[0] + kBase[0] * r[3] + kBase[1] * r[2] - kBase[2] * r[1];
        q[1] = kBase[3] * r[1] - kBase[0] * r[2] + kBase[1] * r[3] + kBase[2] * r[0];
        q[2] = kBase[3] * r[2] + kBase[0] * r[1] - kBase[1] * r[0] + kBase[2] * r[3];
        q[3] = kBase[3] * r[3] - kBase[0] * r[0] - kBase[1] * r[1] - kBase[2] * r[2];
        AboutZ(-6.0f * i, q, q2);
        w.Push(q2, 0.010 * i);
    }
    CHECK(w.Peak(0.15).rightDeg > 55.0f);
}

}  // namespace

int main()
{
    RUN(RightTwistReadsPositive);
    RUN(LeftTwistReadsLeft);
    RUN(SlowTwistIsNotInTheWindow);
    RUN(RepeatedPoseIsNotANewSample);
    RUN(SwingWithoutTwistReadsNone);
    RUN(ClearForgetsTheTwist);
    RUN(OutAndBackStillPeaks);
    RUN(BigSwingIsNotATwist);
    RUN(TwistWithSmallSwingCounts);
    std::printf("twist_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
