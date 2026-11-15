// Standalone unit test for src/src/ovr2xr/calibscreen_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-a.md task 2. Exit code = number of failures.
#include "calibscreen_logic.h"

#include <cstdio>

using namespace dlvr::calib;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static Input In(unsigned long long t)
{
    Input i;
    i.nowMs = t;
    return i;
}
static Input Grips(unsigned long long t, float v)
{
    Input i = In(t);
    i.gripL = v; i.gripR = v; i.gripValidL = true; i.gripValidR = true;
    return i;
}

static void opens_on_posture_with_saved_choice()
{
    Machine m;
    m.Open(1);
    CHECK(m.screen == Screen::Posture);
    CHECK(m.posture == 1);
}
static void grips_held_at_open_do_not_confirm_until_released()
{
    Machine m;
    m.Open(0);
    Cmd c = m.Tick(Grips(0, 1.0f));
    CHECK(c.savePosture == -1);
    c = m.Tick(Grips(10, 1.0f));
    CHECK(c.savePosture == -1);
    m.Tick(Grips(20, 0.0f));
    c = m.Tick(Grips(30, 1.0f));
    CHECK(c.savePosture == 0);
    CHECK(c.startRun);
    CHECK(c.beginPose == 0);
    CHECK(m.screen == Screen::Pose);
}
static void stick_moves_the_choice_with_hysteresis()
{
    Machine m;
    m.Open(0);
    Input i = In(0); i.stickX = 0.9f; m.Tick(i);
    CHECK(m.posture == 1);
    i = In(10); i.stickX = -0.9f; m.Tick(i);   // never back under the re-arm line: no second move
    CHECK(m.posture == 1);
    i = In(20); i.stickX = 0.0f; m.Tick(i);
    i = In(30); i.stickX = -0.9f; m.Tick(i);
    CHECK(m.posture == 0);
}
// Build C: seated starts its own run at once - no notice screen; the choice stays readable (the glue passes it to armfit).
static void seated_runs_at_once()
{
    Machine m;
    m.Open(1);
    m.Tick(Grips(0, 0.0f));
    Cmd c = m.Tick(Grips(10, 1.0f));
    CHECK(c.savePosture == 1);
    CHECK(c.startRun);
    CHECK(c.beginPose == 0);
    CHECK(m.screen == Screen::Pose);
    CHECK(m.posture == 1);
}
static Machine AtPose0()
{
    Machine m;
    m.Open(0);
    m.Tick(Grips(0, 0.0f));
    m.Tick(Grips(10, 1.0f));
    m.Tick(Grips(20, 0.0f));   // released after the confirm (A4)
    return m;
}
static void read_needs_both_arms_ok_and_a_fresh_grip()
{
    Machine m = AtPose0();
    Input i = Grips(100, 1.0f); i.vR = 1; i.vL = 3;
    Cmd c = m.Tick(i);
    CHECK(c.collect == kNoChange);
    CHECK(!m.reading);
    i = Grips(110, 1.0f); i.vR = 1; i.vL = 1;
    c = m.Tick(i);
    CHECK(c.collect == kStart);
    CHECK(m.reading);
}
static void releasing_the_grips_discards_the_read()
{
    Machine m = AtPose0();
    Input i = Grips(100, 1.0f); i.vR = 1; i.vL = 1; m.Tick(i);
    i = Grips(110, 0.0f); i.vR = 1; i.vL = 1; i.nR = 40; i.nL = 40;
    Cmd c = m.Tick(i);
    CHECK(c.collect == kDiscard);
    CHECK(!m.reading);
    CHECK(m.screen == Screen::Pose);
    CHECK(m.pose == 0);
}
static void an_arm_out_of_pose_pauses_and_is_named()
{
    Machine m = AtPose0();
    Input i = Grips(100, 1.0f); i.vR = 1; i.vL = 1; m.Tick(i);
    i = Grips(110, 1.0f); i.vR = 1; i.vL = 2; i.nR = 60; i.nL = 60;
    Cmd c = m.Tick(i);
    CHECK(c.collect == kNoChange);   // still reading: armfit pauses storage itself (A3)
    CHECK(m.reading);
    CHECK(m.pausedArm == 1);         // 1 = left
}
static void full_ring_keeps_advances_and_needs_a_release()
{
    Machine m = AtPose0();
    Input i = Grips(100, 1.0f); i.vR = 1; i.vL = 1; m.Tick(i);
    i = Grips(110, 1.0f); i.vR = 1; i.vL = 1; i.nR = kTarget; i.nL = kTarget;
    Cmd c = m.Tick(i);
    CHECK(c.collect == kKeep);
    CHECK(c.beginPose == 1);
    CHECK(m.pose == 1);
    i = Grips(120, 1.0f); i.vR = 1; i.vL = 1; i.nR = 0; i.nL = 0;
    c = m.Tick(i);
    CHECK(c.collect == kNoChange);   // still squeezed from pose 0: no read until released (A4)
    i = Grips(130, 0.0f); m.Tick(i);
    i = Grips(140, 1.0f); i.vR = 1; i.vL = 1;
    c = m.Tick(i);
    CHECK(c.collect == kStart);
}
static Machine AtComputing()
{
    Machine m = AtPose0();
    unsigned long long t = 100;
    for (int p = 0; p < kPoses; ++p) {
        Input i = Grips(t, 0.0f); m.Tick(i); t += 10;
        i = Grips(t, 1.0f); i.vR = 1; i.vL = 1; m.Tick(i); t += 10;
        i = Grips(t, 1.0f); i.vR = 1; i.vL = 1; i.nR = kTarget; i.nL = kTarget; m.Tick(i); t += 10;
    }
    return m;
}
static void last_pose_asks_for_the_fit()
{
    Machine m = AtComputing();
    CHECK(m.screen == Screen::Computing);
}
static void pass_closes_after_the_show_time()
{
    Machine m = AtComputing();
    Input i = In(1000); i.finish = 1;
    m.Tick(i);
    CHECK(m.screen == Screen::Passed);
    Cmd c = m.Tick(In(1000 + kPassShowMs - 1));
    CHECK(!c.closed);
    c = m.Tick(In(1000 + kPassShowMs));
    CHECK(c.closed);
    CHECK(m.screen == Screen::Closed);
}
static void fail_then_grips_redoes_all_poses()
{
    Machine m = AtComputing();
    Input i = In(1000); i.finish = -1;
    m.Tick(i);
    CHECK(m.screen == Screen::Failed);
    m.Tick(Grips(1010, 0.0f));
    Cmd c = m.Tick(Grips(1020, 1.0f));
    CHECK(c.startRun);
    CHECK(c.beginPose == 0);
    CHECK(m.pose == 0);
    CHECK(m.screen == Screen::Pose);
}
static void b_tap_does_not_skip_b_hold_does()
{
    Machine m = AtPose0();
    Input i = In(100); i.bDown = true; m.Tick(i);
    i = In(500); i.bDown = false;
    Cmd c = m.Tick(i);
    CHECK(!c.closed);
    i = In(600); i.bDown = true; m.Tick(i);
    i = In(600 + kSkipHoldMs - 1); i.bDown = true;
    c = m.Tick(i);
    CHECK(!c.closed);
    CHECK(m.skipFrac > 0.9f);
    i = In(600 + kSkipHoldMs); i.bDown = true;
    c = m.Tick(i);
    CHECK(c.closed);
    CHECK(c.cancel);                 // a run had started: armfit must cancel it
    CHECK(m.screen == Screen::Closed);
}
static void skip_before_any_run_does_not_cancel_armfit()
{
    Machine m;
    m.Open(0);
    Input i = In(0); i.bDown = true; m.Tick(i);
    i = In(kSkipHoldMs); i.bDown = true;
    Cmd c = m.Tick(i);
    CHECK(c.closed);
    CHECK(!c.cancel);
}
static void inactive_grip_reads_not_held()
{
    Machine m;
    m.Open(0);
    m.Tick(Grips(0, 0.0f));
    Input i = Grips(10, 1.0f); i.gripValidL = false;   // left squeeze action inactive, value stale
    Cmd c = m.Tick(i);
    CHECK(c.savePosture == -1);
}
static void grip_hysteresis_holds_between_lines()
{
    Machine m = AtPose0();
    Input i = Grips(100, 0.6f); i.vR = 1; i.vL = 1; m.Tick(i);
    CHECK(m.reading);
    i = Grips(110, 0.4f); i.vR = 1; i.vL = 1;   // under 0.5, over 0.3: still held
    Cmd c = m.Tick(i);
    CHECK(c.collect == kNoChange);
    CHECK(m.reading);
    i = Grips(120, 0.2f); i.vR = 1; i.vL = 1;
    c = m.Tick(i);
    CHECK(c.collect == kDiscard);
}
// The spec's values as LITERALS (the other tests use the constants symbolically, so they cannot see a changed constant -
// the build A plan's deliberate-miss step proved that, 2026-10-14): B held 1 s skips (spec section 2), 150 samples per arm
// (kFitPerPose), the grips at 0.5 on / 0.3 off, the pass shown 3 s.
static void spec_values_are_pinned_by_literal()
{
    Machine m;
    m.Open(0);
    Input i = In(0); i.bDown = true; m.Tick(i);
    i = In(999); i.bDown = true;
    Cmd c = m.Tick(i);
    CHECK(!c.closed);
    i = In(1000); i.bDown = true;
    c = m.Tick(i);
    CHECK(c.closed);
}
static_assert(kTarget == 150, "spec: 150 accepted samples per arm per pose");
static_assert(kGripOn == 0.5f && kGripOff == 0.3f, "spec: grips 0.5 on / 0.3 off");
static_assert(kPassShowMs == 3000, "pass screen shown 3 s");
static_assert(kPoses == 4, "standing set: forward, side, up, down");
static void closed_machine_does_nothing()
{
    Machine m;
    Cmd c = m.Tick(Grips(0, 1.0f));
    CHECK(m.screen == Screen::Closed);
    CHECK(!c.startRun && !c.closed && c.collect == kNoChange);
}

int main()
{
    opens_on_posture_with_saved_choice();
    grips_held_at_open_do_not_confirm_until_released();
    stick_moves_the_choice_with_hysteresis();
    seated_runs_at_once();
    read_needs_both_arms_ok_and_a_fresh_grip();
    releasing_the_grips_discards_the_read();
    an_arm_out_of_pose_pauses_and_is_named();
    full_ring_keeps_advances_and_needs_a_release();
    last_pose_asks_for_the_fit();
    pass_closes_after_the_show_time();
    fail_then_grips_redoes_all_poses();
    b_tap_does_not_skip_b_hold_does();
    skip_before_any_run_does_not_cancel_armfit();
    inactive_grip_reads_not_held();
    grip_hysteresis_holds_between_lines();
    spec_values_are_pinned_by_literal();
    closed_machine_does_nothing();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
