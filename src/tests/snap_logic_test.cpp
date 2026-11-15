// Standalone unit test for src/src/ovr2xr/snap_logic.h - NOT part of the DLL build.
// docs/SPEC-VR-COMFORT-2026-09-30.md §5 (C0, snap turn). Exit code = number of failures.
#include "snap_logic.h"

#include <cstdio>

using namespace dlvr::snapturn;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// A stick already deflected on the very first poll is a flick we never saw start - no snap until it has
// been back to centre (same "a press we did not see start" rule as holster_logic.h's first poll).
static void first_sample_deflected_does_not_fire()
{
    Edge e;
    CHECK(e.Update(0.95f, 0, 0) == 0);
    CHECK(e.Update(0.95f, 10, 0) == 0);
    CHECK(e.Update(0.0f, 20, 0) == 0);
    CHECK(e.Update(0.95f, 30, 0) == 1);
}
static void one_flick_one_snap_right()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    CHECK(e.Update(0.8f, 10, 0) == 1);
    CHECK(e.Update(1.0f, 20, 0) == 0);   // held: no repeat while repeatMs == 0
    CHECK(e.Update(1.0f, 5000, 0) == 0);
}
static void left_is_minus_one()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    CHECK(e.Update(-0.8f, 10, 0) == -1);
}
static void below_fire_threshold_does_not_fire()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    CHECK(e.Update(0.5f, 10, 0) == 0);
    CHECK(e.Update(0.69f, 20, 0) == 0);
    CHECK(e.Update(kFire, 30, 0) == 1);  // the threshold itself fires
}
static void hysteresis_needs_rearm_below_0_3()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    CHECK(e.Update(0.9f, 10, 0) == 1);
    CHECK(e.Update(0.4f, 20, 0) == 0);   // dipped, but not back under kRearm
    CHECK(e.Update(0.9f, 30, 0) == 0);   // so no second snap
    CHECK(e.Update(0.29f, 40, 0) == 0);  // re-armed
    CHECK(e.Update(0.9f, 50, 0) == 1);
}
static void crossing_straight_to_the_other_side_fires_that_side()
{
    // A slow poll can miss the centre on a fast right-to-left flick: the new side is a new flick.
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    CHECK(e.Update(0.9f, 10, 0) == 1);
    CHECK(e.Update(-0.9f, 20, 0) == -1);
    CHECK(e.Update(-0.9f, 30, 0) == 0);
}
static void repeat_while_held()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 300) == 0);
    CHECK(e.Update(0.9f, 1000, 300) == 1);
    CHECK(e.Update(0.9f, 1299, 300) == 0);
    CHECK(e.Update(0.9f, 1300, 300) == 1);
    CHECK(e.Update(0.9f, 1450, 300) == 0);
    CHECK(e.Update(0.9f, 1600, 300) == 1);
    CHECK(e.Update(0.5f, 2000, 300) == 0);   // below fire: no repeat, even past the interval
}
static void reset_requires_centre_again()
{
    Edge e;
    CHECK(e.Update(0.0f, 0, 0) == 0);
    e.Reset();
    CHECK(e.Update(0.9f, 10, 0) == 0);   // first poll after a reset, already deflected
    CHECK(e.Update(0.0f, 20, 0) == 0);
    CHECK(e.Update(0.9f, 30, 0) == 1);
}
static void angle_table()
{
    CHECK(AngleDeg(0) == 15.0f);
    CHECK(AngleDeg(1) == 30.0f);
    CHECK(AngleDeg(2) == 45.0f);
    CHECK(AngleDeg(3) == 60.0f);
    CHECK(AngleDeg(4) == 90.0f);
    CHECK(AngleDeg(-1) == 15.0f);   // clamped, never an out-of-range read
    CHECK(AngleDeg(9) == 90.0f);
    static_assert(kAngleCount == 5, "comfort_snap_angle has five items");
    static_assert(kDefaultAngleIndex == 1, "Jay: 30 degrees is the standard");
}
static void queue_staleness()
{
    CHECK(!QueueStale(1300, 1000));   // exactly kMaxQueueAgeMs old: still applied
    CHECK(QueueStale(1301, 1000));
    CHECK(!QueueStale(1000, 1005));   // queued "after" now (another thread's clock read): not stale
}

int main()
{
    first_sample_deflected_does_not_fire();
    one_flick_one_snap_right();
    left_is_minus_one();
    below_fire_threshold_does_not_fire();
    hysteresis_needs_rearm_below_0_3();
    crossing_straight_to_the_other_side_fires_that_side();
    repeat_while_held();
    reset_requires_centre_again();
    angle_table();
    queue_staleness();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail;
}
