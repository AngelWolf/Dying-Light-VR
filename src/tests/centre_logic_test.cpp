// Standalone unit test for src/src/ovr2xr/centre_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 2. Exit code = number of failures.
#include "centre_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::centre;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

const long long kS = 1000000000LL;   // one second in XrTime nanoseconds

static void session_start_centres_once()
{
    Tracker t;
    OnState(t, 1, 0);
    OnState(t, 2, 0);
    OnState(t, 3, 0);
    OnState(t, 4, 0);
    CHECK(t.pending == Reason::None);
    OnState(t, 5, 10 * kS);
    CHECK(t.pending == Reason::SessionStart);
    CHECK(t.armedAt == 10 * kS);
    Done(t);
    CHECK(t.pending == Reason::None);
    CHECK(t.everCentred);
    OnState(t, 5, 20 * kS);   // FOCUSED again with no drop out: nothing
    CHECK(t.pending == Reason::None);
}

static void refocus_after_dropout()
{
    Tracker t;
    OnState(t, 5, 0);
    Done(t);
    OnState(t, 4, 5 * kS);
    CHECK(t.pending == Reason::None);
    OnState(t, 5, 6 * kS);
    CHECK(t.pending == Reason::Refocus);
    CHECK(t.armedAt == 6 * kS);
    Done(t);
    OnState(t, 3, 7 * kS);
    OnState(t, 5, 8 * kS);
    CHECK(t.pending == Reason::Refocus);
}

static void no_centre_without_dropout()
{
    Tracker t;
    OnState(t, 5, 0);
    Done(t);
    OnState(t, 5, kS);
    OnState(t, 5, 2 * kS);
    CHECK(t.pending == Reason::None);
}

static void session_start_before_first_centre_survives_dropout()
{
    // Focus lost before the first centre landed: the pending start stays a SessionStart, re-armed at the new focus.
    Tracker t;
    OnState(t, 5, 0);
    OnState(t, 4, kS / 2);
    CHECK(t.pending == Reason::SessionStart);
    OnState(t, 5, 3 * kS);
    CHECK(t.pending == Reason::SessionStart);
    CHECK(t.armedAt == 3 * kS);
}

static void centre_settle_and_give_up()
{
    Tracker t;
    OnState(t, 5, 100 * kS);
    CHECK(!Due(t, 100 * kS + kSettleNs - 1000000LL, false, true));
    CHECK(Due(t, 100 * kS + kSettleNs, false, true));
    CHECK(!Due(t, 100 * kS + kSettleNs, false, false));   // no tracked head
    CHECK(!GiveUp(t, 100 * kS + kGiveUpNs));
    CHECK(GiveUp(t, 100 * kS + kGiveUpNs + 1000000LL));
    Tracker idle;
    CHECK(!Due(idle, 1000 * kS, false, true));
    CHECK(!GiveUp(idle, 1000 * kS));
}

static void centre_waits_for_absorb()
{
    Tracker t;
    OnState(t, 5, 0);
    CHECK(!Due(t, 2 * kS, true, true));
    CHECK(Due(t, 2 * kS, false, true));
}

static void compose_matches_game_recentre()
{
    const float base0[3] = { 0.5f, 1.25f, -0.75f };
    float out[3];
    ComposeBasePos(0.0f, base0, 0.2f, -0.3f, out);
    CHECK(Near(out[0], 0.7f, 1e-5f));
    CHECK(out[1] == 1.25f);
    CHECK(Near(out[2], -1.05f, 1e-5f));
    // Base yaw +90 about +Y: the appSpace +X axis lies along reference -Z (x' = x cos + z sin, z' = -x sin + z cos - the
    // ovr_RecenterTrackingOrigin composition, ovr2xr.cpp:9022-9026).
    const float base1[3] = { 1.0f, 0.4f, 2.0f };
    ComposeBasePos(90.0f, base1, 1.0f, 0.0f, out);
    CHECK(Near(out[0], 1.0f, 1e-4f));
    CHECK(out[1] == 0.4f);
    CHECK(Near(out[2], 1.0f, 1e-4f));
    ComposeBasePos(90.0f, base1, 0.0f, 1.0f, out);
    CHECK(Near(out[0], 2.0f, 1e-4f));
    CHECK(Near(out[2], 2.0f, 1e-4f));
}

int main()
{
    session_start_centres_once();
    refocus_after_dropout();
    no_centre_without_dropout();
    session_start_before_first_centre_survives_dropout();
    centre_settle_and_give_up();
    centre_waits_for_absorb();
    compose_matches_game_recentre();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail;
}
