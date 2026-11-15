// Standalone unit test for src/src/ovr2xr/fingertrack_logic.h (controller finger tracking on an empty hand) - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-11-controller-finger-tracking.md Task 3. Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\fingertrack_logic_test.cpp
#include "fingertrack_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::ft;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

void TargetMapsTouchAndSqueeze()
{
    const Weights a = Target(Inputs{ true, true, false, 0.3f }, 0.6f);
    NEAR(a.index, 1.0f, 1e-5f);
    NEAR(a.thumb, 0.0f, 1e-5f);
    NEAR(a.lower, 0.5f, 1e-5f);
    const Weights b = Target(Inputs{ true, false, true, 0.9f }, 0.6f);
    NEAR(b.index, 0.0f, 1e-5f);
    NEAR(b.thumb, 1.0f, 1e-5f);
    NEAR(b.lower, 1.0f, 1e-5f);   // past fistAt: clamped
}

void InactiveInputsOpenTheHand()
{
    const Weights a = Target(Inputs{ false, true, true, 1.0f }, 0.6f);
    NEAR(a.index, 0.0f, 1e-5f);
    NEAR(a.thumb, 0.0f, 1e-5f);
    NEAR(a.lower, 0.0f, 1e-5f);
    // and a curled hand eases back open from there - never frozen
    Weights w{ 1.0f, 1.0f, 1.0f };
    for (int i = 0; i < 20; ++i) { w = Ease(w, a, 0.011f, 100.0f); }
    NEAR(w.index, 0.0f, 1e-5f);
    NEAR(w.lower, 0.0f, 1e-5f);
}

void FistAtGuardsDivideByZero()
{
    const Weights a = Target(Inputs{ true, false, false, 0.0f }, 0.0f);
    CHECK(std::isfinite(a.lower));
    NEAR(a.lower, 0.0f, 1e-5f);
    const Weights b = Target(Inputs{ true, false, false, 0.01f }, 0.0f);
    NEAR(b.lower, 1.0f, 1e-5f);
}

void EaseStepsAtTheRate()
{
    const Weights w = Ease(Weights{ 0, 0, 0 }, Weights{ 1, 1, 1 }, 0.011f, 100.0f);
    NEAR(w.index, 0.11f, 1e-5f);
    NEAR(w.thumb, 0.11f, 1e-5f);
    NEAR(w.lower, 0.11f, 1e-5f);
    const Weights z = Ease(Weights{ 0, 0, 0 }, Weights{ 1, 0.5f, 1 }, 0.011f, 0.0f);
    NEAR(z.index, 1.0f, 1e-6f);
    NEAR(z.thumb, 0.5f, 1e-6f);
    // never overshoots
    const Weights o = Ease(Weights{ 0.95f, 0, 0 }, Weights{ 1, 0, 0 }, 0.011f, 100.0f);
    NEAR(o.index, 1.0f, 1e-6f);
}

void OneFrameBlipBarelyMoves()
{
    Weights w{ 0, 0, 0 };
    w = Ease(w, Weights{ 1, 0, 0 }, 0.011f, 100.0f);
    w = Ease(w, Weights{ 0, 0, 0 }, 0.011f, 100.0f);
    CHECK(w.index <= 0.11f + 1e-6f);
}

void BlendPinSlerpsAndLerps()
{
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float Rz[9] = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };   // 90 deg about z, row-major
    const float T0[3] = { 0.0f, 0.0f, 0.0f };
    const float T1[3] = { 0.02f, -0.04f, 0.06f };
    float R[9], T[3];
    BlendPin(I, T0, Rz, T1, 0.5f, R, T);
    const float c = std::cos(3.14159265f / 4.0f), s = std::sin(3.14159265f / 4.0f);
    NEAR(R[0], c, 1e-4f);  NEAR(R[1], -s, 1e-4f); NEAR(R[2], 0.0f, 1e-4f);
    NEAR(R[3], s, 1e-4f);  NEAR(R[4], c, 1e-4f);  NEAR(R[8], 1.0f, 1e-4f);
    NEAR(T[0], 0.01f, 1e-6f); NEAR(T[1], -0.02f, 1e-6f); NEAR(T[2], 0.03f, 1e-6f);
    BlendPin(I, T0, Rz, T1, 0.0f, R, T);
    for (int k = 0; k < 9; ++k) { CHECK(R[k] == I[k]); }
    for (int k = 0; k < 3; ++k) { CHECK(T[k] == T0[k]); }
    BlendPin(I, T0, Rz, T1, 1.0f, R, T);
    for (int k = 0; k < 9; ++k) { NEAR(R[k], Rz[k], 1e-6f); }
    for (int k = 0; k < 3; ++k) { NEAR(T[k], T1[k], 1e-6f); }
}

void BlendPinTakesTheShortArc()
{
    // 170 deg about z: halfway must be 85 deg the SAME way, not 95 deg the other way
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float a = 170.0f * 3.14159265f / 180.0f;
    const float R170[9] = { std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1 };
    const float T[3] = { 0, 0, 0 };
    float R[9], Tt[3];
    BlendPin(I, T, R170, T, 0.5f, R, Tt);
    const float h = 85.0f * 3.14159265f / 180.0f;
    NEAR(R[0], std::cos(h), 1e-4f);
    NEAR(R[3], std::sin(h), 1e-4f);
}

void GroupsFollowThePinOrder()
{
    // kFtPinName order: hand1, finger01, 01extra, 02, 03, 11, 12, 13, 21, 22, 23, 31, 32, 33, 41, 42, 43
    CHECK(GroupOfPin(0) == Group::Hand1);
    for (int j = 1; j <= 4; ++j) { CHECK(GroupOfPin(j) == Group::Thumb); }
    for (int j = 5; j <= 7; ++j) { CHECK(GroupOfPin(j) == Group::Index); }
    for (int j = 8; j <= 16; ++j) { CHECK(GroupOfPin(j) == Group::Lower); }
    const Weights w{ 0.25f, 0.5f, 0.75f };
    NEAR(WeightOf(w, Group::Index), 0.25f, 1e-6f);
    NEAR(WeightOf(w, Group::Thumb), 0.5f, 1e-6f);
    NEAR(WeightOf(w, Group::Lower), 0.75f, 1e-6f);
    NEAR(WeightOf(w, Group::Hand1), 0.75f, 1e-6f);   // the palm piece the ring + little hang from follows the lower fingers
}

}  // namespace

int main()
{
    RUN(TargetMapsTouchAndSqueeze);
    RUN(InactiveInputsOpenTheHand);
    RUN(FistAtGuardsDivideByZero);
    RUN(EaseStepsAtTheRate);
    RUN(OneFrameBlipBarelyMoves);
    RUN(BlendPinSlerpsAndLerps);
    RUN(BlendPinTakesTheShortArc);
    RUN(GroupsFollowThePinOrder);
    std::printf("fingertrack_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
