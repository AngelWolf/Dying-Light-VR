// Standalone unit test for src/src/ovr2xr/pivotfit_logic.h - NOT part of the DLL build.
// Plan docs/superpowers/plans/2026-11-20-holster-pivot-posture-rings.md Task 1. Exit code = number of failures.
#include "pivotfit_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::pivotfit;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const float kShipped[3] = { 0.008f, 0.112f, -0.084f };

// R = Ry(yaw) * Rx(pitch), row-major. Yaw + turns left (HeadYaw convention), pitch + looks up.
static void Rot(double yaw, double pitch, float R[9])
{
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const double m[9] = { cy, sy * sp, sy * cp,
                          0.0, cp, -sp,
                          -sy, cy * sp, cy * cp };
    for (int i = 0; i < 9; ++i) { R[i] = static_cast<float>(m[i]); }
}

// Deterministic noise in [-a, a].
static unsigned g_seed = 12345u;
static float Noise(float a)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return a * (static_cast<float>((g_seed >> 8) & 0xFFFF) / 32767.5f - 1.0f);
}

struct Sim {
    float nTrue[3] = { 0.012f, 0.125f, -0.095f };
    bool  nods = true;          // a nod every 20 s
    float slideM = 0.0f;        // P slides sideways this far over every 3 s (sawtooth) - a body that moves
    double t = 0.0;
};

static Sample Next(Sim& s)
{
    Sample x{};
    x.t = s.t;
    const double yaw = 40.0 * 0.0174532925 * std::sin(2.0 * 3.14159265 * 0.3 * s.t);
    double pitch = 0.0;
    if (s.nods) {
        const double ph = std::fmod(s.t, 20.0);
        if (ph < 3.0) { pitch = 25.0 * 0.0174532925 * std::sin(2.0 * 3.14159265 * ph / 3.0); }
    }
    Rot(yaw, pitch, x.R);
    const double slide = s.slideM > 0.0f ? s.slideM * (std::fmod(s.t, 3.0) / 3.0) : 0.0;
    const double P[3] = { slide, 1.6, 0.0 };
    for (int r = 0; r < 3; ++r) {
        const double rn = x.R[3 * r] * s.nTrue[0] + x.R[3 * r + 1] * s.nTrue[1] + x.R[3 * r + 2] * s.nTrue[2];
        x.eye[r] = static_cast<float>(P[r] + rn) + Noise(0.001f);
    }
    s.t += 1.0 / 30.0;
    return x;
}

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void RecoversKnownPivot()
{
    State st; Reset(st, kShipped);
    Sim sim;
    int commits = 0;
    while (sim.t < 360.0) { if (Step(st, Next(sim)) == Event::Committed) { ++commits; } }
    CHECK(commits >= 1);
    CHECK(Near(st.n[0], sim.nTrue[0], 0.002f));
    CHECK(Near(st.n[1], sim.nTrue[1], 0.002f));
    CHECK(Near(st.n[2], sim.nTrue[2], 0.002f));
}

static void YawOnlyNeverMovesY()
{
    State st; Reset(st, kShipped);
    Sim sim; sim.nods = false;
    while (sim.t < 360.0) { Step(st, Next(sim)); }
    CHECK(st.n[1] == kShipped[1]);
    CHECK(st.acceptedPitch == 0);
    CHECK(Near(st.n[0], sim.nTrue[0], 0.002f));
    CHECK(Near(st.n[2], sim.nTrue[2], 0.002f));
}

static void TranslatingHeadRefused()
{
    State st; Reset(st, kShipped);
    Sim sim; sim.slideM = 0.08f;
    while (sim.t < 120.0) { Step(st, Next(sim)); }
    CHECK(st.tried > 0);
    CHECK(st.acceptedYaw == 0);
    CHECK(st.acceptedPitch == 0);
}

static void NoCommitBefore40Yaw()
{
    State st; Reset(st, kShipped);
    Sim sim; sim.nods = false;
    bool committed = false;
    while (st.acceptedYaw < kYawNeed - 1 && sim.t < 600.0) { if (Step(st, Next(sim)) == Event::Committed) { committed = true; } }
    CHECK(st.acceptedYaw == kYawNeed - 1);
    CHECK(!committed);
    CHECK(st.n[0] == kShipped[0] && st.n[1] == kShipped[1] && st.n[2] == kShipped[2]);
}

static void OutOfBoxRefused()
{
    State st; Reset(st, kShipped);
    Sim sim; sim.nods = false; sim.nTrue[2] = -0.200f;
    bool refused = false;
    while (sim.t < 360.0) { if (Step(st, Next(sim)) == Event::RefusedOutOfBox) { refused = true; } }
    CHECK(refused);
    CHECK(st.n[2] == kShipped[2]);
}

static void CommitRateLimited()
{
    State st; Reset(st, kShipped);
    Sim sim;
    double first = -1.0;
    while (sim.t < 360.0 && first < 0.0) { if (Step(st, Next(sim)) == Event::Committed) { first = sim.t; } }
    CHECK(first > 0.0);
    sim.nTrue[0] += 0.030f;   // the fit now sees a very different n.x
    bool early = false;
    const double until = first + kCommitMinS - 1.0;
    while (sim.t < until) { if (Step(st, Next(sim)) == Event::Committed) { early = true; } }
    CHECK(!early);
}

static void LatchShiftKeepsVirtualHead()
{
    float Rq[9];
    Rot(0.7, -0.3, Rq);
    const float eye[3] = { 0.3f, 1.55f, -0.2f };
    const float n0[3] = { 0.008f, 0.112f, -0.084f }, n1[3] = { 0.015f, 0.125f, -0.100f };
    const float f = 0.4f;   // facing yaw
    const float latch0[3] = { 0.01f, 0.11f, -0.08f };
    float d[3];
    LatchShift(f, Rq, n0, n1, d);
    const float latch1[3] = { latch0[0] + d[0], latch0[1] + d[1], latch0[2] + d[2] };
    float Rf[9];
    Rot(f, 0.0, Rf);
    for (int r = 0; r < 3; ++r) {
        const float neck0 = eye[r] - (Rq[3 * r] * n0[0] + Rq[3 * r + 1] * n0[1] + Rq[3 * r + 2] * n0[2]);
        const float neck1 = eye[r] - (Rq[3 * r] * n1[0] + Rq[3 * r + 1] * n1[1] + Rq[3 * r + 2] * n1[2]);
        const float vh0 = neck0 + Rf[3 * r] * latch0[0] + Rf[3 * r + 1] * latch0[1] + Rf[3 * r + 2] * latch0[2];
        const float vh1 = neck1 + Rf[3 * r] * latch1[0] + Rf[3 * r + 1] * latch1[1] + Rf[3 * r + 2] * latch1[2];
        CHECK(Near(vh0, vh1, 1e-6f));
    }
}

static void KeyedStoreRoundTrip()
{
    State st; Reset(st, kShipped);
    st.n[0] = 0.0123f; st.n[1] = 0.1188f; st.n[2] = -0.0911f;
    st.acceptedYaw = 77; st.acceptedPitch = 12;
    char buf[256];
    CHECK(Serialize(st, "Meta Quest 3", buf, static_cast<int>(sizeof(buf))) > 0);
    float n[3] = {};
    int counts[2] = {};
    CHECK(Parse(buf, "Meta Quest 3", n, counts));
    CHECK(Near(n[0], 0.0123f, 1e-4f) && Near(n[1], 0.1188f, 1e-4f) && Near(n[2], -0.0911f, 1e-4f));
    CHECK(counts[0] == 77 && counts[1] == 12);
    CHECK(!Parse(buf, "Valve Index", n, counts));
}

int main()
{
    RecoversKnownPivot();
    YawOnlyNeverMovesY();
    TranslatingHeadRefused();
    NoCommitBefore40Yaw();
    OutOfBoxRefused();
    CommitRateLimited();
    LatchShiftKeepsVirtualHead();
    KeyedStoreRoundTrip();
    std::printf("pivotfit_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
