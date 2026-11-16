// Standalone unit test for src/src/ovr2xr/calibraw_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-b.md task 2. Exit code = number of failures.
// The fixture is tools/calib_raw_fixture.py's output: 3 R + 3 L hold samples of backups/20261013_bowhand_run1 run 1, with the
// EXPECTED body-frame T taken from the SOLVE's own logged T / anchor / yaw.
#include "calibraw_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::calibraw;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

struct Fix { int side; float ctl[3]; float headRep[3]; float q[4]; float grip[9]; float expT[3]; };
static const Fix kFix[6] = {
    { 0, { 0.21463658f, -0.22028601f, -0.42815539f }, { 0.0046863826f, 0.013528271f, 0.12998064f }, { -0.47370994f, 0.2107752f, 0.051524911f, -0.85353267f }, { 0.105576f, -0.97742999f, -0.18299299f, -0.67942297f, -0.205274f, 0.704449f, -0.72611201f, 0.049957f, -0.68576002f }, { 0.466146647f, -0.226406f, 0.164042475f } },  // R 14:59:11
    { 1, { -0.12455547f, -0.20576617f, -0.47142607f }, { 0.0056019574f, 0.01346386f, 0.13028085f }, { 0.51064789f, 0.19193687f, -0.086903982f, 0.83357465f }, { 0.32800001f, -0.930516f, -0.16296799f, -0.786322f, -0.36453301f, 0.49881399f, -0.523561f, -0.035464998f, -0.85125101f }, { 0.503431764f, -0.20716f, 0.125619273f } },  // L 14:59:11
    { 0, { 0.21538898f, -0.21967866f, -0.42723423f }, { 0.0061680689f, 0.013397565f, 0.13072535f }, { -0.47463641f, 0.20903589f, 0.051212993f, -0.85346442f }, { 0.105576f, -0.97742999f, -0.18299299f, -0.67942297f, -0.205274f, 0.704449f, -0.72611201f, 0.049957f, -0.68576002f }, { 0.465864889f, -0.225945f, 0.163518474f } },  // R 14:59:11
    { 1, { -0.12343013f, -0.20560755f, -0.47115511f }, { 0.0065919915f, 0.013416287f, 0.13117418f }, { 0.51061988f, 0.19347496f, -0.08724355f, 0.83320069f }, { 0.32800001f, -0.930516f, -0.16296799f, -0.786322f, -0.36453301f, 0.49881399f, -0.523561f, -0.035464998f, -0.85125101f }, { 0.504024203f, -0.206997f, 0.125236464f } },  // L 14:59:11
    { 0, { 0.21598637f, -0.2178611f, -0.42659917f }, { 0.0070895217f, 0.013397401f, 0.13154548f }, { -0.47656721f, 0.20769383f, 0.052120332f, -0.85266078f }, { 0.105576f, -0.97742999f, -0.18299299f, -0.67942297f, -0.205274f, 0.704449f, -0.72611201f, 0.049957f, -0.68576002f }, { 0.465865087f, -0.224567f, 0.163400549f } },  // R 14:59:11
    { 1, { -0.12173317f, -0.20471957f, -0.4700686f }, { 0.0075184102f, 0.013433605f, 0.13236506f }, { 0.51063347f, 0.19349122f, -0.086822182f, 0.83323264f }, { 0.32800001f, -0.930516f, -0.16296799f, -0.786322f, -0.36453301f, 0.49881399f, -0.523561f, -0.035464998f, -0.85125101f }, { 0.504145326f, -0.206138f, 0.124521021f } },  // L 14:59:11
};

static float Dist(const float a[3], const float b[3])
{
    const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// post = identity: the fixture's solve (before trace 20261116_1126) stepped back with cq . grip alone.
static const float kI[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void raw_t_matches_the_solve_on_every_fixture_sample()
{
    for (const Fix& f : kFix) {
        float t[3];
        RawT(f.ctl, f.headRep, f.q, f.grip, kI, f.side, t);
        const float mm = Dist(t, f.expT) * 1000.0f;
        if (mm >= 0.1f) { std::printf("  side %d: %.4f mm off\n", f.side, mm); }
        CHECK(mm < 0.1f);
    }
}
static void the_side_matters_deliberate_miss()
{
    // A RIGHT sample fed as LEFT: the handholder offset and the out-axis sign both change - must be far off.
    const Fix& f = kFix[0];
    float t[3];
    RawT(f.ctl, f.headRep, f.q, f.grip, kI, 1, t);
    CHECK(Dist(t, f.expT) * 1000.0f > 1.0f);
}
// Trace 20261116_1126: with the authored rest as post, T moves by |post t - t| - 0.03955 m right (the archived [ACHIEVED] FIRST USE
// 0.0395 / 0.0396 m), ~0.0378 m left (scratchpad palm_offset.py) - and identity reproduces the old T exactly.
static void raw_t_with_the_rest_moves_by_the_drawn_offset()
{
    for (const Fix& f : kFix) {
        float t0[3], t1[3];
        RawT(f.ctl, f.headRep, f.q, f.grip, kI, f.side, t0);
        RawT(f.ctl, f.headRep, f.q, f.grip, kRestAuthored[f.side], f.side, t1);
        const float mm = Dist(t0, t1) * 1000.0f;
        const float want = f.side ? 37.8f : 39.55f;
        if (std::fabs(mm - want) >= 0.5f) { std::printf("  side %d: moved %.3f mm, want %.2f\n", f.side, mm, want); }
        CHECK(std::fabs(mm - want) < 0.5f);
    }
}
static void the_baked_rest_is_a_rotation()
{
    for (int s = 0; s < 2; ++s) {
        const float* m = kRestAuthored[s];
        for (int c = 0; c < 3; ++c) {
            const float l = std::sqrt(m[c] * m[c] + m[3 + c] * m[3 + c] + m[6 + c] * m[6 + c]);
            CHECK(std::fabs(l - 1.0f) < 5e-4f);
        }
        const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        CHECK(std::fabs(det - 1.0f) < 1e-3f);
    }
    // the two sides are mirrors (INV-150): same tilt column 0 x, opposite roll
    CHECK(kRestAuthored[0][0] == kRestAuthored[1][0] && kRestAuthored[0][5] == -kRestAuthored[1][5]);
}
static void matches_against_the_standin()
{
    const float fwd[3] = { kStandinS[0] + 0.6f, kStandinS[1], kStandinS[2] };
    CHECK(RawMatches(fwd, 0) == 1);
    CHECK(RawMatches(fwd, 2) == -1);
    const float shortT[3] = { kStandinS[0] + 0.3f, kStandinS[1], kStandinS[2] };
    CHECK(RawMatches(shortT, 0) == -2);
    const float up[3] = { kStandinS[0], kStandinS[1] + 0.6f, kStandinS[2] };
    CHECK(RawMatches(up, 2) == 1);
    const float down[3] = { kStandinS[0], kStandinS[1] - 0.6f, kStandinS[2] };
    CHECK(RawMatches(down, 3) == 1);
    const float side[3] = { kStandinS[0], kStandinS[1], kStandinS[2] + 0.6f };
    CHECK(RawMatches(side, 1) == 1);
    // Build C: pose 4 = seated, down and OUT at 45 deg. Straight down / straight out are 45 deg off its axis -> outside the
    // cos 0.80 cone (must MISS); 45 deg inward misses; 30 deg from vertical (15 deg off the axis) is in.
    const float d45[3] = { kStandinS[0], kStandinS[1] - 0.4243f, kStandinS[2] + 0.4243f };
    CHECK(RawMatches(d45, 4) == 1);
    CHECK(RawMatches(d45, 3) == -1);
    CHECK(RawMatches(down, 4) == -1);
    CHECK(RawMatches(side, 4) == -1);
    const float in45[3] = { kStandinS[0], kStandinS[1] - 0.4243f, kStandinS[2] - 0.4243f };
    CHECK(RawMatches(in45, 4) == -1);
    const float d30[3] = { kStandinS[0], kStandinS[1] - 0.5196f, kStandinS[2] + 0.3f };
    CHECK(RawMatches(d30, 4) == 1);
    const float d45short[3] = { kStandinS[0], kStandinS[1] - 0.25f, kStandinS[2] + 0.25f };
    CHECK(RawMatches(d45short, 4) == -2);
}
static void forearm_in_hand_is_basis_transpose_times_direction()
{
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float T[3] = { 0, 0, 2 }, o[3] = { 0, 0, 0 };
    float v[3];
    CHECK(ForearmInHand(I, T, o, v));
    CHECK(std::fabs(v[0]) < 1e-6f && std::fabs(v[1]) < 1e-6f && std::fabs(v[2] - 1.0f) < 1e-6f);
    // A basis whose X column points along body +Y: a direction along +Y reads as hand +X.
    const float B[9] = { 0, 1, 0, 1, 0, 0, 0, 0, 1 };   // row-major, columns = axes: col0 = (0,1,0), col1 = (1,0,0)
    const float T2[3] = { 0, 3, 0 };
    CHECK(ForearmInHand(B, T2, o, v));
    CHECK(std::fabs(v[0] - 1.0f) < 1e-6f && std::fabs(v[1]) < 1e-6f);
    CHECK(!ForearmInHand(I, o, o, v));   // zero length refused
}
static void hand_basis_maps_like_raw_t()
{
    // RawHandBasis's column 0 must be the body-frame image of cq.grip column 0 - check via the wrist step-back:
    // RawT(ctl = headRep) = -(M . cq . grip . t), so -RawT == RawHandBasis . t.
    const Fix& f = kFix[1];
    float t[3], B[9];
    RawT(f.headRep, f.headRep, f.q, f.grip, kI, f.side, t);
    RawHandBasis(f.q, f.grip, f.side, B);
    const float* h = kHhAuthored[f.side];
    const float bt[3] = { B[0] * h[0] + B[1] * h[1] + B[2] * h[2], B[3] * h[0] + B[4] * h[1] + B[5] * h[2],
                          B[6] * h[0] + B[7] * h[1] + B[8] * h[2] };
    const float neg[3] = { -t[0], -t[1], -t[2] };
    CHECK(Dist(bt, neg) < 1e-6f);
}

static_assert(kHhAuthored[0][0] == 0.09999989f && kHhAuthored[0][2] == -0.024999849f, "INV-161 right handholder offset");
static_assert(kHhAuthored[1][0] == 0.096248291f && kHhAuthored[1][2] == -0.023768237f, "INV-161 left handholder offset");
static_assert(kStandinS[0] == -0.1302f && kStandinS[1] == -0.2487f && kStandinS[2] == 0.1909f, "build B ruling B1");

int main()
{
    raw_t_matches_the_solve_on_every_fixture_sample();
    the_side_matters_deliberate_miss();
    raw_t_with_the_rest_moves_by_the_drawn_offset();
    the_baked_rest_is_a_rotation();
    matches_against_the_standin();
    forearm_in_hand_is_basis_transpose_times_direction();
    hand_basis_maps_like_raw_t();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
