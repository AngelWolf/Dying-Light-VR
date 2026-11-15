// Standalone unit test for src/src/ovr2xr/cinyaw_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-25-cutscene-camera-design.md (a) + (c). Exit code = number of failures.
#include "cinyaw_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::cinyaw;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// (a): the addend comes out only when the slot returned +4460 itself, for the object D2 wrote, outside D2's own chase.
static void remove_only_on_4460_branch()
{
    CHECK(Near(RemoveAddend(10.0f, 10.0f, 3.0f, true, false), 7.0f));
    CHECK(Near(RemoveAddend(10.0f, 11.0f, 3.0f, true, false), 10.0f));   // the movie's own look deviation
    CHECK(Near(RemoveAddend(10.0f, 10.0f, 3.0f, false, false), 10.0f));  // another object
    CHECK(Near(RemoveAddend(10.0f, 10.0f, 3.0f, true, true), 10.0f));    // inside D2's chase: already removed
}

static void snap_at_start()
{
    State s;
    const Step st = Update(s, 1, true, 40.0f);
    CHECK(st.started);
    CHECK(Near(st.rootAddDeg, 0.0f));
    CHECK(!st.handback);
}

static void turn_after_start()
{
    State s;
    Update(s, 1, true, 40.0f);
    const Step st = Update(s, 1, true, 70.0f);
    CHECK(!st.started);
    CHECK(Near(st.rootAddDeg, 30.0f));
}

static void wrap_far_turn()
{
    State s;
    Update(s, 1, true, 170.0f);
    const Step st = Update(s, 1, true, -170.0f);
    CHECK(Near(st.rootAddDeg, 20.0f));
}

static void handback_once()
{
    State s;
    Update(s, 1, true, 170.0f);
    Update(s, 1, true, 100.0f);
    const Step st = Update(s, 1, false, 100.0f);
    CHECK(st.handback);
    CHECK(Near(st.handbackDeg, -170.0f));
    CHECK(Near(st.rootAddDeg, 0.0f));
    const Step st2 = Update(s, 1, false, 100.0f);
    CHECK(!st2.handback);
}

static void skip_is_same_handback()
{
    State s;
    Update(s, 1, true, 25.0f);   // one cinematic update, then the owner changes (a skip)
    const Step st = Update(s, 1, false, 25.0f);
    CHECK(st.handback);
    CHECK(Near(st.handbackDeg, -25.0f));
}

static void back_to_back()
{
    State s;
    int starts = 0, handbacks = 0;
    const float hs[] = { 10.0f, 20.0f, 30.0f, 40.0f };
    for (float h : hs) {
        const Step st = Update(s, 1, true, h);
        starts += st.started ? 1 : 0;
        handbacks += st.handback ? 1 : 0;
    }
    const Step end = Update(s, 1, false, 40.0f);
    handbacks += end.handback ? 1 : 0;
    CHECK(starts == 1);
    CHECK(handbacks == 1);
    CHECK(Near(end.handbackDeg, -10.0f));
}

static void new_player_drops()
{
    State s;
    Update(s, 1, true, 30.0f);
    const Step st = Update(s, 2, true, 50.0f);   // a new PlayerDI mid-cutscene: drop, then start fresh for it
    CHECK(!st.handback);
    CHECK(st.started);
    CHECK(Near(st.rootAddDeg, 0.0f));
    State t;
    Update(t, 1, true, 30.0f);
    const Step u = Update(t, 2, false, 50.0f);    // new player, not cinematic: nothing for either
    CHECK(!u.handback);
    CHECK(!t.active);
}

// After Stop writes the look, the camera is fields + (h - folded) whether the fold resumes or is switched off (headfollow_logic.h), so
// the hand-back that keeps the view at anim + (h - y0) is folded - y0.
static void handback_includes_fold()
{
    State s;
    Update(s, 1, true, 30.0f, 12.0f);
    const Step st = Update(s, 1, false, 50.0f, 12.0f);
    CHECK(st.handback);
    CHECK(Near(st.handbackDeg, -18.0f));
    State t;
    Update(t, 1, true, 170.0f, -170.0f);
    const Step u = Update(t, 1, false, 0.0f, -170.0f);
    CHECK(Near(u.handbackDeg, 20.0f));   // wrapped: -170 - 170 = -340 -> +20
}

// ADDENDUM 2: the split of q = A(yaw) B(pitch) C(roll) into its angles, every axis order, pitch to +-89.5 deg (the bunk shot).
static void QMul(const float a[4], const float b[4], float o[4])
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void AxQ(int ax, float ang, float o[4])
{
    o[0] = o[1] = o[2] = 0.0f;
    o[3] = std::cos(ang * 0.5f);
    o[ax] = std::sin(ang * 0.5f);
}
static bool NearRad(float a, float b, float tol)
{
    float d = a - b;
    while (d > 3.14159265f) { d -= 6.28318531f; }
    while (d < -3.14159265f) { d += 6.28318531f; }
    return std::fabs(d) < tol;
}
static void split_round_trip()
{
    const int perms[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    const float ys[] = { -2.9f, -1.0f, 0.0f, 0.7f, 2.5f };
    const float ps[] = { -1.55f, -0.8f, 0.0f, 0.4f, 1.2f, 1.55f };
    const float rs[] = { -2.0f, 0.0f, 0.3f };
    for (const auto& pm : perms) {
        for (float y : ys) {
            for (float p : ps) {
                for (float r : rs) {
                    float qa[4], qb[4], qc[4], t[4], q[4];
                    AxQ(pm[0], y, qa); AxQ(pm[1], p, qb); AxQ(pm[2], r, qc);
                    QMul(qa, qb, t); QMul(t, qc, q);
                    float y2 = 0, p2 = 0, r2 = 0;
                    CHECK(SplitYawPitchRoll(q, pm[0], pm[1], pm[2], &y2, &p2, &r2));
                    CHECK(NearRad(y2, y, 2e-3f));
                    CHECK(std::fabs(p2 - p) < 2e-3f);
                    CHECK(NearRad(r2, r, 2e-3f));
                }
            }
        }
        // the bunk shot: steep pitch, no roll - the yaw must survive
        const float steep[] = { 1.4835f, 1.5621f, -1.5621f };
        for (float p : steep) {
            float qa[4], qb[4], q[4];
            AxQ(pm[0], 0.7f, qa); AxQ(pm[1], p, qb); QMul(qa, qb, q);
            float y2 = 0, p2 = 0, r2 = 0;
            SplitYawPitchRoll(q, pm[0], pm[1], pm[2], &y2, &p2, &r2);
            CHECK(NearRad(y2, 0.7f, 2e-3f));
            CHECK(std::fabs(p2 - p) < 2e-3f);
        }
    }
    float y = 0, p = 0, r = 0;
    const float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(!SplitYawPitchRoll(q, 1, 1, 2, &y, &p, &r));   // axes not distinct: refused
}

static void dialog_never_acts()
{
    State s;
    const float hs[] = { 0.0f, 45.0f, -45.0f, 0.0f };
    for (float h : hs) {
        const Step st = Update(s, 1, false, h);
        CHECK(!st.started);
        CHECK(!st.handback);
        CHECK(Near(st.rootAddDeg, 0.0f));
    }
}

int main()
{
    remove_only_on_4460_branch();
    snap_at_start();
    turn_after_start();
    wrap_far_turn();
    handback_once();
    skip_is_same_handback();
    back_to_back();
    new_player_drops();
    handback_includes_fold();
    split_round_trip();
    dialog_never_acts();
    return g_fail;
}
