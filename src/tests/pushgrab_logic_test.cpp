// Standalone unit test for src/src/ovr2xr/pushgrab_logic.h - NOT part of the DLL build.
// Open-hand push and grab: docs/superpowers/specs/2026-10-31-open-hand-push-and-grab-design.md, plan Task 2. Exit code = failures.
// Compile: cl /nologo /EHsc /std:c++17 /W4 /WX /I src\src\ovr2xr src\src\tests\pushgrab_logic_test.cpp
#include "pushgrab_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using dlvr::pushgrab::ArmSpeedMeter;
using dlvr::pushgrab::CapZombieMeleeRange;
using dlvr::pushgrab::ClassifyPush;
using dlvr::pushgrab::ClassifyPushTracked;
using dlvr::pushgrab::GripLatch;
using dlvr::pushgrab::GrabEvent;
using dlvr::pushgrab::GrabIntent;
using dlvr::pushgrab::HelperGameSize;
using dlvr::pushgrab::HelperPlan;
using dlvr::pushgrab::GrabMachine;
using dlvr::pushgrab::IsThrow;
using dlvr::pushgrab::OriginForDirection;
using dlvr::pushgrab::PushArm;
using dlvr::pushgrab::PushParams;
using dlvr::pushgrab::PushResult;
using dlvr::pushgrab::PushTier;

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static const PushParams kP = { 1.2f, 3.5f, 0.4f };
static const float kNormalTowardPlayer[3] = { 0.0f, 0.0f, -1.0f };  // the body faces -z; pushing in = +z
static const double kDt = 1.0 / 90.0;

static void slow_contact_never_pushes()
{
    const float v[3] = { 0.0f, 0.0f, 0.5f };
    CHECK(ClassifyPush(v, kNormalTowardPlayer, kP).tier == PushTier::None);
}

static void straight_shove_is_shove()
{
    const float v[3] = { 0.0f, 0.0f, 2.0f };
    const PushResult r = ClassifyPush(v, kNormalTowardPlayer, kP);
    CHECK(r.tier == PushTier::Shove);
    CHECK(Near(r.speed, 2.0f));
    CHECK(Near(r.dir[0], 0.0f) && Near(r.dir[1], 0.0f) && Near(r.dir[2], 1.0f));
}

static void sideways_sweep_is_shove()
{
    const float v[3] = { 2.0f, 0.0f, 0.0f };
    const PushResult r = ClassifyPush(v, kNormalTowardPlayer, kP);
    CHECK(r.tier == PushTier::Shove);
    CHECK(Near(r.dir[0], 1.0f) && Near(r.dir[1], 0.0f) && Near(r.dir[2], 0.0f));
}

static void moving_away_never_pushes()
{
    const float v[3] = { 0.0f, 0.0f, -2.0f };
    CHECK(ClassifyPush(v, kNormalTowardPlayer, kP).tier == PushTier::None);
}

static void hard_shove_knocks_down()
{
    const float v[3] = { 0.0f, 0.0f, 4.0f };
    CHECK(ClassifyPush(v, kNormalTowardPlayer, kP).tier == PushTier::KnockDown);
}

static void vertical_component_ignored()
{
    const float v[3] = { 0.0f, 3.0f, 1.5f };
    const PushResult r = ClassifyPush(v, kNormalTowardPlayer, kP);
    CHECK(r.tier == PushTier::Shove);
    CHECK(Near(r.speed, 1.5f));
    CHECK(Near(r.dir[1], 0.0f) && Near(r.dir[2], 1.0f));
}

static void origin_gives_direction()
{
    const float target[3] = { 5.0f, 0.0f, 5.0f }, dir[3] = { 1.0f, 0.0f, 0.0f };
    float o[3] = {};
    OriginForDirection(target, dir, 1.0f, o);
    CHECK(Near(o[0], 4.0f) && Near(o[1], 0.0f) && Near(o[2], 5.0f));
}

// Runs `frames` frames in contact and over threshold from t0; returns how many fired.
static int RunContact(PushArm& arm, double& t, int frames)
{
    int n = 0;
    for (int i = 0; i < frames; ++i) {
        if (arm.Fire(true, true, t, kP.rearmS)) { ++n; }
        t += kDt;
    }
    return n;
}

static void one_push_per_contact()
{
    PushArm arm;
    double t = 10.0;
    CHECK(RunContact(arm, t, 27) == 1);  // 0.3 s
}

static void rearm_time_fires_again()
{
    PushArm arm;
    double t = 10.0;
    CHECK(RunContact(arm, t, 45) == 2);  // 0.5 s: the first at 0, the second at >= 0.4 s
}

static void flicker_shorter_than_rearm_fires_once()
{
    PushArm arm;
    double t = 10.0;
    int n = 0;
    for (int i = 0; i < 9; ++i) {  // 0.1 s with one frame out of contact in the middle
        const bool in = (i != 4);
        if (arm.Fire(in, true, t, kP.rearmS)) { ++n; }
        t += kDt;
    }
    CHECK(n == 1);
}

static void leaving_contact_rearms()
{
    PushArm arm;
    double t = 10.0;
    int n = RunContact(arm, t, 5);
    for (int i = 0; i < 18; ++i) {  // 0.2 s out
        if (arm.Fire(false, false, t, kP.rearmS)) { ++n; }
        t += kDt;
    }
    n += RunContact(arm, t, 5);
    CHECK(n == 2);
}

static void under_threshold_does_not_spend_the_arm()
{
    PushArm arm;
    double t = 10.0;
    int n = 0;
    for (int i = 0; i < 5; ++i) {  // touching slowly, then speeding up inside the same contact
        if (arm.Fire(true, i >= 3, t, kP.rearmS)) { ++n; }
        t += kDt;
    }
    CHECK(n == 1);
}

static void grip_held_before_contact_never_grabs()
{
    GrabMachine m;
    double t = 10.0;
    CHECK(m.Update(true, false, t, 2.5f) == GrabEvent::None);
    t += kDt;
    CHECK(m.Update(true, true, t, 2.5f) == GrabEvent::None);
    CHECK(!m.Holding());
}

static void press_in_contact_starts()
{
    GrabMachine m;
    double t = 10.0;
    CHECK(m.Update(false, true, t, 2.5f) == GrabEvent::None);
    t += kDt;
    CHECK(m.Update(true, true, t, 2.5f) == GrabEvent::Start);
    CHECK(m.Holding());
}

static void release_ends_once()
{
    GrabMachine m;
    double t = 10.0;
    m.Update(false, true, t, 2.5f);
    t += kDt;
    m.Update(true, true, t, 2.5f);
    t += kDt;
    CHECK(m.Update(true, false, t, 2.5f) == GrabEvent::None);  // held, the hand has left the stop - still holding
    t += kDt;
    CHECK(m.Update(false, false, t, 2.5f) == GrabEvent::Release);
    t += kDt;
    CHECK(m.Update(false, false, t, 2.5f) == GrabEvent::None);
    CHECK(!m.Holding());
}

static void break_free_after_time()
{
    GrabMachine m;
    double t = 10.0;
    m.Update(false, true, t, 2.5f);
    t += kDt;
    m.Update(true, true, t, 2.5f);
    int bf = 0;
    for (int i = 0; i < 270; ++i) {  // 3 s held
        t += kDt;
        if (m.Update(true, true, t, 2.5f) == GrabEvent::BreakFree) { ++bf; }
    }
    CHECK(bf == 1);
    CHECK(!m.Holding());
}

static void break_free_then_release_ends_once()
{
    GrabMachine m;
    double t = 10.0;
    m.Update(false, true, t, 2.5f);
    t += kDt;
    m.Update(true, true, t, 2.5f);
    int ends = 0;
    for (int i = 0; i < 230; ++i) {  // past 2.5 s, then let go
        t += kDt;
        const GrabEvent e = m.Update(true, true, t, 2.5f);
        if (e == GrabEvent::BreakFree || e == GrabEvent::Release) { ++ends; }
    }
    t += kDt;
    const GrabEvent e = m.Update(false, true, t, 2.5f);
    if (e == GrabEvent::BreakFree || e == GrabEvent::Release) { ++ends; }
    CHECK(ends == 1);
}

static void regrip_after_break_free_needs_new_press()
{
    GrabMachine m;
    double t = 10.0;
    m.Update(false, true, t, 2.5f);
    t += kDt;
    m.Update(true, true, t, 2.5f);
    for (int i = 0; i < 240; ++i) { t += kDt; m.Update(true, true, t, 2.5f); }
    t += kDt;
    CHECK(m.Update(true, true, t, 2.5f) == GrabEvent::None);  // still squeezing after the break-free: no new grab
    t += kDt;
    m.Update(false, true, t, 2.5f);
    t += kDt;
    CHECK(m.Update(true, true, t, 2.5f) == GrabEvent::Start);
}

static void throw_vs_drop()
{
    const float slow[3] = { 0.8f, 0.0f, 0.0f }, fast[3] = { 0.0f, 0.0f, 1.2f };
    CHECK(!IsThrow(slow, 1.0f));
    CHECK(IsThrow(fast, 1.0f));
}

// Final review I2: the tier comes from the CONTROLLER's own (tracking-space) speed, so a snap / smooth turn or walking with the hand
// still - large game-world velocity, no hand motion - never pushes. The direction is still the world velocity's.
static void snap_turn_never_pushes()
{
    const float world[3] = { 20.0f, 0.0f, 5.0f };
    CHECK(ClassifyPushTracked(world, 0.05f, kNormalTowardPlayer, kP).tier == PushTier::None);
}

static void tracked_speed_sets_the_tier()
{
    const float world[3] = { 0.0f, 0.0f, 2.0f };
    const PushResult shove = ClassifyPushTracked(world, 2.0f, kNormalTowardPlayer, kP);
    CHECK(shove.tier == PushTier::Shove);
    CHECK(Near(shove.speed, 2.0f));
    CHECK(Near(shove.dir[2], 1.0f));
    CHECK(ClassifyPushTracked(world, 4.0f, kNormalTowardPlayer, kP).tier == PushTier::KnockDown);
}

static void tracked_push_moving_away_never_pushes()
{
    const float world[3] = { 0.0f, 0.0f, -3.0f };
    CHECK(ClassifyPushTracked(world, 3.0f, kNormalTowardPlayer, kP).tier == PushTier::None);
}

// Final review I1: the grip is withheld from the game from the grab's START until the grip OPENS - through the break-free and after
// a game-side refusal - never only while the machine holds.
static void withhold_survives_break_free_until_grip_opens()
{
    GripLatch l;
    CHECK(!l.Update(false, true));     // squeezing, no grab yet: the game gets the grip
    CHECK(l.Update(true, true));       // the grab's START: withheld
    CHECK(l.Update(false, true));      // break-free fired, still squeezing: still withheld
    CHECK(l.Update(false, true));
    CHECK(!l.Update(false, false));    // the grip opens: released
    CHECK(!l.Update(false, true));     // a new squeeze with no START reaches the game again
}

static void withhold_ends_when_grip_opens_on_start_frame()
{
    GripLatch l;
    CHECK(!l.Update(true, false));     // a START reported on a frame the grip is already open: nothing to withhold
}

// Zombie close approach (spec 2026-10-31-zombie-close-approach-and-grab-intent-design.md §2): the cap applies to an ordinary zombie
// targeting the player only, and never raises a range.
static void cap_applies() { CHECK(CapZombieMeleeRange(1.8f, true, true, 0.9f) == 0.9f); }
static void cap_needs_zombie() { CHECK(CapZombieMeleeRange(1.8f, false, true, 0.9f) == 1.8f); }
static void cap_needs_player() { CHECK(CapZombieMeleeRange(1.8f, true, false, 0.9f) == 1.8f); }
static void cap_never_raises() { CHECK(CapZombieMeleeRange(0.6f, true, true, 0.9f) == 0.6f); }
static void cap_nan_returns_original() { CHECK(CapZombieMeleeRange(1.8f, true, true, NAN) == 1.8f); }

// Grab intent (spec §3): a hand squeezed at least the threshold when it would shove is reaching to grab.
static void intent_closing()
{
    CHECK(GrabIntent(true, true, 0.20f, 0.20f));
    CHECK(GrabIntent(true, true, 1.0f, 0.20f));
}
static void intent_open() { CHECK(!GrabIntent(true, true, 0.19f, 0.20f)); }
static void intent_no_read_is_open_hand() { CHECK(!GrabIntent(true, false, 1.0f, 0.20f)); }
// Final review I3: a hand whose grip another feature owns (holster, two-hand, loot) or with vr_grip off cannot start a grab - it
// shoves as before.
static void intent_cannot_grab_is_open_hand() { CHECK(!GrabIntent(false, true, 1.0f, 0.20f)); }

// The arm-only shove speed (2026-10-31 (6), Jay: "it was also factoring in the player running towards a zombie as arm acceleration";
// PG3-PG5: 13 of 18 shoves fired with the arm under 1.2 m/s): hand minus head, 3D, over 50-150 ms, new poses only.
static void arm_speed_steady_motion()
{
    ArmSpeedMeter m;
    for (int i = 0; i <= 10; ++i) {   // 2 m/s along x, a new pose every 11 ms
        const float rel[3] = { 0.022f * i, 0.0f, 0.0f };
        m.Add(0.011 * i, rel);
    }
    CHECK(Near(m.Speed(0.110), 2.0f, 0.05f));
}
static void arm_speed_counts_vertical()
{
    ArmSpeedMeter m;
    for (int i = 0; i <= 10; ++i) {
        const float rel[3] = { 0.0f, -0.015f * i, 0.0f };   // a slap straight down, 1.5 m/s
        m.Add(0.010 * i, rel);
    }
    CHECK(Near(m.Speed(0.100), 1.5f, 0.05f));
}
static void arm_speed_repeated_pose_is_not_a_spike()
{
    // the same pose seen on two game updates, then the next one: the old per-update difference read this as a spike
    ArmSpeedMeter m;
    double t = 0.0;
    for (int i = 0; i <= 12; ++i) {
        const float rel[3] = { 0.005f * i, 0.0f, 0.0f };   // 0.5 m/s at 10 ms per new pose
        m.Add(t, rel);
        t += 0.002;   // seen again 2 ms later, unchanged - ignored
        m.Add(t, rel);
        t += 0.008;
    }
    CHECK(m.Speed(t) < 0.7f);
}
static void arm_speed_too_short_span_is_zero()
{
    ArmSpeedMeter m;
    const float a[3] = { 0.0f, 0.0f, 0.0f }, b[3] = { 0.1f, 0.0f, 0.0f };
    m.Add(0.000, a);
    m.Add(0.020, b);   // only 20 ms of history: no reading yet
    CHECK(m.Speed(0.020) == 0.0f);
}
static void arm_speed_stale_is_zero()
{
    ArmSpeedMeter m;
    for (int i = 0; i <= 10; ++i) {
        const float rel[3] = { 0.02f * i, 0.0f, 0.0f };
        m.Add(0.010 * i, rel);
    }
    CHECK(m.Speed(0.500) == 0.0f);   // no new pose for 0.4 s: not moving as far as we know
}

// The player's AwayFromAIHelper size (INV-669; Jay 2026-10-31 (6): "Yes, build it"): 1 = write `out`, 0 = already right, -1 = the
// value is neither the game's nor ours (refuse, never write).
static void helper_shrinks_from_the_games_value()
{
    const float orig[3] = { 0.6f, 0.4f, 0.6f }, cur[3] = { 0.6f, 0.4f, 0.6f };
    float out[3] = {};
    CHECK(HelperPlan(true, 0.3f, cur, orig, nullptr, out) == 1);
    CHECK(out[0] == 0.3f && out[1] == 0.4f && out[2] == 0.3f);
}
static void helper_already_right_is_left_alone()
{
    const float orig[3] = { 0.6f, 0.4f, 0.6f }, cur[3] = { 0.3f, 0.4f, 0.3f };
    float out[3] = {};
    CHECK(HelperPlan(true, 0.3f, cur, orig, cur, out) == 0);
}
static void helper_restored_when_off()
{
    const float orig[3] = { 0.6f, 0.4f, 0.6f }, ours[3] = { 0.3f, 0.4f, 0.3f };
    float out[3] = {};
    CHECK(HelperPlan(false, 0.3f, ours, orig, ours, out) == 1);
    CHECK(out[0] == 0.6f && out[1] == 0.4f && out[2] == 0.6f);
    CHECK(HelperPlan(false, 0.3f, orig, orig, ours, out) == 0);
}
static void helper_unknown_value_refuses()
{
    const float orig[3] = { 0.6f, 0.4f, 0.6f }, cur[3] = { 0.5f, 0.4f, 0.5f };
    float out[3] = {};
    CHECK(HelperPlan(true, 0.3f, cur, orig, nullptr, out) == -1);
    const float ours[3] = { 0.3f, 0.4f, 0.3f };
    CHECK(HelperPlan(true, 0.25f, cur, orig, ours, out) == -1);
}
// RUN PG7: the game's file bytes are 99 99 19 3F / CC CC CC 3E (0.59999996 / 0.39999998), NOT 0.6f / 0.4f - the exact compare
// against 0.6f refused the game's own value. The game's size must be built from those bits and accepted.
static void helper_accepts_the_games_real_bits()
{
    const unsigned char file[12] = { 0x99, 0x99, 0x19, 0x3F, 0xCC, 0xCC, 0xCC, 0x3E, 0x99, 0x99, 0x19, 0x3F };
    float cur[3] = {}, orig[3] = {}, out[3] = {};
    std::memcpy(cur, file, sizeof(cur));
    HelperGameSize(orig);
    CHECK(HelperPlan(true, 0.3f, cur, orig, nullptr, out) == 1);
    CHECK(out[0] == 0.3f && out[2] == 0.3f && std::memcmp(&out[1], file + 4, 4) == 0);   // Y kept bit for bit
}
static void helper_radius_clamped()
{
    const float orig[3] = { 0.6f, 0.4f, 0.6f };
    float out[3] = {};
    CHECK(HelperPlan(true, 5.0f, orig, orig, nullptr, out) == 0);    // clamped to the game's 0.6 = already right
    CHECK(HelperPlan(true, 0.0f, orig, orig, nullptr, out) == 1 && out[0] == 0.1f);
    CHECK(HelperPlan(true, NAN, orig, orig, nullptr, out) == 0);     // a bad value = the game's
}

int main()
{
    helper_shrinks_from_the_games_value();
    helper_already_right_is_left_alone();
    helper_restored_when_off();
    helper_unknown_value_refuses();
    helper_radius_clamped();
    helper_accepts_the_games_real_bits();
    arm_speed_steady_motion();
    arm_speed_counts_vertical();
    arm_speed_repeated_pose_is_not_a_spike();
    arm_speed_too_short_span_is_zero();
    arm_speed_stale_is_zero();
    cap_applies();
    cap_needs_zombie();
    cap_needs_player();
    cap_never_raises();
    cap_nan_returns_original();
    intent_closing();
    intent_open();
    intent_no_read_is_open_hand();
    intent_cannot_grab_is_open_hand();
    snap_turn_never_pushes();
    tracked_speed_sets_the_tier();
    tracked_push_moving_away_never_pushes();
    withhold_survives_break_free_until_grip_opens();
    withhold_ends_when_grip_opens_on_start_frame();
    slow_contact_never_pushes();
    straight_shove_is_shove();
    sideways_sweep_is_shove();
    moving_away_never_pushes();
    hard_shove_knocks_down();
    vertical_component_ignored();
    origin_gives_direction();
    one_push_per_contact();
    rearm_time_fires_again();
    flicker_shorter_than_rearm_fires_once();
    leaving_contact_rearms();
    under_threshold_does_not_spend_the_arm();
    grip_held_before_contact_never_grabs();
    press_in_contact_starts();
    release_ends_once();
    break_free_after_time();
    break_free_then_release_ends_once();
    regrip_after_break_free_needs_new_press();
    throw_vs_drop();
    if (g_fail == 0) { std::printf("pushgrab_logic_test: ALL PASS\n"); }
    else { std::printf("pushgrab_logic_test: %d FAILURE(S)\n", g_fail); }
    return g_fail;
}
