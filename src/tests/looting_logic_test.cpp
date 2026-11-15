// Standalone unit test for src/src/ovr2xr/looting_logic.h - NOT part of the DLL build.
// Loot by hand: docs/superpowers/specs/2026-10-23-loot-by-hand-design.md, plan Task 2. Exit code = failures.
#include "looting_logic.h"

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

using looting::HandMachine;
using looting::Input;
using looting::Output;
using looting::Params;
using looting::State;

static Input In(bool grip, float dist, float speed, double t)
{
    Input in{};
    in.gripDown = grip;
    in.bodyDist = dist;
    in.handSpeed = speed;
    in.nowS = t;
    in.gripOwnedElsewhere = false;
    in.otherHandBusy = false;
    return in;
}

static bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static void arms_on_press_edge_near_body()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    const Output o = m.Step(In(true, 0.10f, 0.05f, 0.01), p);
    CHECK(o.state == State::Settling);
    CHECK(Is(o.event, "ARM"));
    CHECK(o.withholdGrip);
    CHECK(!o.wantSquare);
}
static void no_arm_when_grip_already_held()
{
    HandMachine m;
    Params p;
    const Output o = m.Step(In(true, 0.10f, 0.05f, 0.0), p);   // first sample: grip already down
    CHECK(o.state == State::Idle);
    CHECK(!o.withholdGrip);
    CHECK(o.event == nullptr);
    const Output o2 = m.Step(In(true, 0.10f, 0.05f, 0.5), p);  // still held - no edge
    CHECK(o2.state == State::Idle);
}
static void no_arm_out_of_reach()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.40f, 0.0f, 0.0), p);
    CHECK(m.Step(In(true, 0.40f, 0.0f, 0.01), p).state == State::Idle);
}
static void no_arm_without_body()
{
    HandMachine m;
    Params p;
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    CHECK(m.Step(In(true, -1.0f, 0.0f, 0.01), p).state == State::Idle);
}
static void no_arm_when_grip_owned_elsewhere()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    Input in = In(true, 0.10f, 0.0f, 0.01);
    in.gripOwnedElsewhere = true;
    const Output o = m.Step(in, p);
    CHECK(o.state == State::Idle);
    CHECK(!o.withholdGrip);
}
static void no_arm_when_other_hand_busy()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    Input in = In(true, 0.10f, 0.0f, 0.01);
    in.otherHandBusy = true;
    CHECK(m.Step(in, p).state == State::Idle);
}
static void settles_into_search()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.05f, 1.0), p);
    const Output early = m.Step(In(true, 0.10f, 0.05f, 1.20), p);
    CHECK(early.state == State::Settling);
    CHECK(!early.wantSquare);
    const Output o = m.Step(In(true, 0.10f, 0.05f, 1.31), p);
    CHECK(o.state == State::Searching);
    CHECK(Is(o.event, "SEARCH"));
    CHECK(o.wantSquare);
    CHECK(o.withholdGrip);
}
static void moving_during_settle_spends_the_press()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.0f, 0.01), p);
    const Output o = m.Step(In(true, 0.10f, 0.50f, 0.05), p);
    CHECK(o.state == State::Spent);
    CHECK(Is(o.event, "END moved"));
    CHECK(!o.withholdGrip);
    const Output o2 = m.Step(In(true, 0.10f, 0.0f, 1.0), p);   // still and near again, same press
    CHECK(o2.state == State::Spent);
    CHECK(!o2.wantSquare);
    const Output o3 = m.Step(In(false, 0.10f, 0.0f, 1.1), p);  // grip opens
    CHECK(o3.state == State::Idle);
    CHECK(o3.event == nullptr);
}
static void body_lost_during_settle()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.0f, 0.01), p);
    const Output o = m.Step(In(true, -1.0f, 0.0f, 0.05), p);
    CHECK(o.state == State::Spent);
    CHECK(Is(o.event, "END body gone"));
}
static void search_ends_on_grip_open()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.0f, 0.01), p);
    m.Step(In(true, 0.10f, 0.0f, 0.40), p);
    const Output o = m.Step(In(false, 0.10f, 0.0f, 0.60), p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.event, "END grip opened"));
    CHECK(!o.wantSquare);
    CHECK(!o.withholdGrip);
}
static void grip_open_during_settle_ends_without_search()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.0f, 0.01), p);
    const Output o = m.Step(In(false, 0.10f, 0.0f, 0.10), p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.event, "END grip opened"));
}
static void search_keeps_square_while_held_even_if_hand_moves()
{
    HandMachine m;
    Params p;
    m.Step(In(false, 0.10f, 0.0f, 0.0), p);
    m.Step(In(true, 0.10f, 0.0f, 0.01), p);
    m.Step(In(true, 0.10f, 0.0f, 0.40), p);
    const Output o = m.Step(In(true, -1.0f, 1.0f, 0.50), p);
    CHECK(o.state == State::Searching);
    CHECK(o.wantSquare);
    CHECK(o.event == nullptr);
}
static void withhold_only_while_settling_or_searching()
{
    HandMachine m;
    Params p;
    CHECK(!m.Step(In(false, 0.10f, 0.0f, 0.0), p).withholdGrip);
    CHECK(m.Step(In(true, 0.10f, 0.0f, 0.01), p).withholdGrip);
    CHECK(m.Step(In(true, 0.10f, 0.0f, 0.40), p).withholdGrip);
    CHECK(!m.Step(In(false, 0.10f, 0.0f, 0.50), p).withholdGrip);
}
static void params_are_read_every_step()
{
    HandMachine m;
    Params p;
    p.reachM = 0.50f;
    m.Step(In(false, 0.40f, 0.0f, 0.0), p);
    CHECK(m.Step(In(true, 0.40f, 0.0f, 0.01), p).state == State::Settling);   // in reach at 0.50
    HandMachine m2;
    Params q;
    q.settleS = 0.0f;
    m2.Step(In(false, 0.10f, 0.0f, 0.0), q);
    m2.Step(In(true, 0.10f, 0.0f, 0.01), q);
    CHECK(m2.Step(In(true, 0.10f, 0.0f, 0.02), q).state == State::Searching);  // settle 0 -> next step searches
}

// ---- NO-ARM reasons (STATE 10-23 (2) cause 2: a press that does not arm must say why) ----
static void no_arm_reason_names_each_refusal()
{
    Params p;
    {
        HandMachine m;
        m.Step(In(false, 0.10f, 0.0f, 0.0), p);
        Input in = In(true, 0.10f, 0.0f, 0.01);
        in.gripOwnedElsewhere = true;
        CHECK(Is(m.Step(in, p).noArm, "grip owned elsewhere"));
    }
    {
        HandMachine m;
        m.Step(In(false, 0.10f, 0.0f, 0.0), p);
        Input in = In(true, 0.10f, 0.0f, 0.01);
        in.otherHandBusy = true;
        CHECK(Is(m.Step(in, p).noArm, "other hand looting"));
    }
    {
        HandMachine m;
        m.Step(In(false, -1.0f, 0.0f, 0.0), p);
        CHECK(Is(m.Step(In(true, -1.0f, 0.0f, 0.01), p).noArm, "no body"));
    }
    {
        HandMachine m;
        m.Step(In(false, 0.40f, 0.0f, 0.0), p);
        CHECK(Is(m.Step(In(true, 0.40f, 0.0f, 0.01), p).noArm, "body out of reach"));
    }
}
static void no_arm_reason_only_on_a_press_edge()
{
    Params p;
    HandMachine m;
    CHECK(m.Step(In(false, 0.40f, 0.0f, 0.0), p).noArm == nullptr);   // no press
    CHECK(m.Step(In(true, 0.40f, 0.0f, 0.01), p).noArm != nullptr);   // the edge
    CHECK(m.Step(In(true, 0.40f, 0.0f, 0.02), p).noArm == nullptr);   // still held - not an edge
    HandMachine a;
    a.Step(In(false, 0.10f, 0.0f, 0.0), p);
    CHECK(a.Step(In(true, 0.10f, 0.0f, 0.01), p).noArm == nullptr);   // armed - no reason
    HandMachine f;
    CHECK(f.Step(In(true, 0.40f, 0.0f, 0.0), p).noArm == nullptr);    // first sample is never an edge
}

// ---- SpeedWindow (D-269: polls ~570/s, the pose moves once per display frame) ----
using looting::SpeedWindow;
static void feed(SpeedWindow& w, double t0, double t1, float mps, double frameS, double pollS)
{
    for (double t = t0; t < t1; t += pollS) {
        const double frameT = static_cast<double>(static_cast<long long>(t / frameS)) * frameS;   // the pose of the last frame
        const float p[3] = { static_cast<float>(mps * frameT), 0.0f, 0.0f };
        w.Push(t, p);
    }
}
static void slow_frame_stepped_hand_reads_slow()
{
    SpeedWindow w;
    feed(w, 0.0, 0.30, 0.10f, 0.012, 0.00175);   // 0.10 m/s, 12 ms frames, 1.75 ms polls
    const float s = w.Speed();
    CHECK(s > 0.05f && s < 0.15f);   // a per-poll delta here reads ~0.69 m/s on a step
}
static void fast_hand_reads_fast()
{
    SpeedWindow w;
    feed(w, 0.0, 0.30, 1.00f, 0.012, 0.00175);
    const float s = w.Speed();
    CHECK(s > 0.85f && s < 1.15f);
}
static void still_hand_reads_zero()
{
    SpeedWindow w;
    feed(w, 0.0, 0.30, 0.0f, 0.012, 0.00175);
    CHECK(w.Speed() == 0.0f);
}
static void unfilled_window_reads_minus_one()
{
    SpeedWindow w;
    CHECK(w.Speed() < 0.0f);   // empty
    feed(w, 0.0, 0.040, 1.00f, 0.012, 0.00175);   // 40 ms < the 50 ms minimum
    CHECK(w.Speed() < 0.0f);
    feed(w, 0.040, 0.060, 1.00f, 0.012, 0.00175);   // now >= 50 ms held
    CHECK(w.Speed() > 0.5f);
}
static void reset_drops_history()
{
    SpeedWindow w;
    feed(w, 0.0, 0.30, 2.00f, 0.012, 0.00175);   // fast...
    w.Reset();
    CHECK(w.Speed() < 0.0f);
    for (double t = 0.30; t < 0.50; t += 0.00175) {   // ...then still at one point
        const float p[3] = { 0.6f, 0.0f, 0.0f };
        w.Push(t, p);
    }
    CHECK(w.Speed() == 0.0f);
}
static void window_is_about_100_ms()
{
    SpeedWindow w;
    for (double t = 0.0; t < 0.30; t += 0.00175) {   // still, then a 5 cm step at 0.25 s
        const float p[3] = { t >= 0.25 ? 0.05f : 0.0f, 0.0f, 0.0f };
        w.Push(t, p);
    }
    const float s = w.Speed();
    CHECK(s > 0.40f && s < 0.60f);   // 0.05 m over ~0.10 s - not over the whole 0.30 s held (0.17), nor one poll
}
static void ring_overflow_keeps_the_newest()
{
    SpeedWindow w;
    for (double t = 0.0; t < 1.0; t += 0.0005) {   // 2,000 polls/s: the 128-sample ring holds ~64 ms < 100 ms
        const float p[3] = { static_cast<float>(0.5 * t), 0.0f, 0.0f };
        w.Push(t, p);
    }
    const float s = w.Speed();
    CHECK(s > 0.45f && s < 0.55f);   // still measured, over the oldest held sample (>= 50 ms)
}

// ---- box distances (INV-616: a fresh corpse = the dead AI's box OR its attached container's box, the nearer one) ----
using looting::BoxDist;
using looting::NearerBoxDist;
static void box_dist_inside_is_zero()
{
    const float b[6] = { 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    const float p[3] = { 0.5f, 0.5f, 0.5f };
    CHECK(BoxDist(b, p) == 0.0f);
}
static void box_dist_outside_one_axis_and_corner()
{
    const float b[6] = { 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    const float p1[3] = { 1.3f, 0.5f, 0.5f };
    CHECK(BoxDist(b, p1) > 0.299f && BoxDist(b, p1) < 0.301f);
    const float p2[3] = { -0.3f, -0.4f, 0.5f };   // 0.3 / 0.4 off two faces -> 0.5
    CHECK(BoxDist(b, p2) > 0.499f && BoxDist(b, p2) < 0.501f);
}
static void nearer_of_two_boxes_and_which()
{
    const float a[6] = { 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };          // the AI's box
    const float c[6] = { 5.f, 0.f, 0.f, 6.f, 1.f, 1.f };          // the container's box
    const float p[3] = { 4.8f, 0.5f, 0.5f };
    float da = -1.0f, dc = -1.0f;
    const float d = NearerBoxDist(a, true, c, true, p, &da, &dc);
    CHECK(d > 0.199f && d < 0.201f);   // the container's
    CHECK(da > 3.79f && da < 3.81f);
    CHECK(dc > 0.199f && dc < 0.201f);
}
static void nearer_ignores_a_missing_box()
{
    const float a[6] = { 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    const float c[6] = {};
    const float p[3] = { 1.5f, 0.5f, 0.5f };
    float da = -1.0f, dc = -1.0f;
    CHECK(NearerBoxDist(a, true, c, false, p, &da, &dc) > 0.49f);
    CHECK(dc < 0.0f);   // -1 = no such box
    CHECK(NearerBoxDist(a, false, c, false, p, &da, &dc) < 0.0f);   // neither -> -1
}

// ---- PressLatch (right grip camouflage block): decided at the press edge, held until release ----
using looting::PressLatch;
static void latch_blocks_a_press_made_on_a_target()
{
    PressLatch l;
    CHECK(!l.Step(false, true));   // no press
    CHECK(l.Step(true, true));     // press on a target -> withheld
    CHECK(l.Step(true, false));    // the target leaves mid-press -> still withheld (no R1 burst mid-hold)
    CHECK(!l.Step(false, false));  // release
}
static void latch_passes_a_press_made_off_target()
{
    PressLatch l;
    CHECK(!l.Step(false, false));
    CHECK(!l.Step(true, false));   // press with no target -> R1 passes
    CHECK(!l.Step(true, true));    // a target appears mid-press -> still passes (no R1 drop mid-hold)
    CHECK(!l.Step(false, true));
    CHECK(l.Step(true, true));     // the next press on it is withheld
}
static void latch_first_sample_down_is_not_a_press()
{
    PressLatch l;
    CHECK(!l.Step(true, true));    // grip already down at the first sample: not a press edge
}

// ---- body grab (docs/superpowers/specs/2026-10-23-body-grab-design.md, plan Task 2): the DRAG state ----
static Params DragOn()
{
    Params p;
    p.dragEnabled = true;
    return p;
}
static Input Torso(Input in)
{
    in.onTorso = true;
    return in;
}
static void drag_on_non_torso_press()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    const Output o = m.Step(In(true, 0.10f, 0.05f, 0.01), p);   // a limb, not the torso
    CHECK(o.state == State::Dragging);
    CHECK(Is(o.event, "DRAG"));
    CHECK(o.dragging);
    CHECK(o.withholdGrip);
    CHECK(!o.wantSquare);
}
static void torso_press_still_loots()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(Torso(In(false, 0.10f, 0.05f, 0.0)), p);
    const Output a = m.Step(Torso(In(true, 0.10f, 0.05f, 0.01)), p);
    CHECK(a.state == State::Settling);
    CHECK(Is(a.event, "ARM"));
    CHECK(!a.dragging);
    const Output b = m.Step(Torso(In(true, 0.10f, 0.05f, 0.40)), p);
    CHECK(b.state == State::Searching);
    CHECK(Is(b.event, "SEARCH"));
    CHECK(b.wantSquare);
    CHECK(!b.dragging);
}
static void torso_press_moving_drags()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(Torso(In(false, 0.10f, 0.05f, 0.0)), p);
    m.Step(Torso(In(true, 0.10f, 0.05f, 0.01)), p);   // ARM
    const Output o = m.Step(Torso(In(true, 0.10f, 0.80f, 0.10)), p);   // moves before the search
    CHECK(o.state == State::Dragging);
    CHECK(Is(o.event, "DRAG"));
    CHECK(o.dragging);
    CHECK(!o.wantSquare);
}
static void drag_disabled_keeps_spent()
{
    HandMachine m;
    Params p;   // dragEnabled defaults false: loot by hand exactly as before
    CHECK(!p.dragEnabled);
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    const Output a = m.Step(In(true, 0.10f, 0.05f, 0.01), p);   // not on the torso - still arms as today
    CHECK(a.state == State::Settling);
    CHECK(Is(a.event, "ARM"));
    const Output b = m.Step(In(true, 0.10f, 0.80f, 0.10), p);
    CHECK(b.state == State::Spent);
    CHECK(Is(b.event, "END moved"));
    CHECK(!b.dragging);
}
static void drag_ends_on_grip_open()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    m.Step(In(true, 0.10f, 0.05f, 0.01), p);   // DRAG
    const Output o = m.Step(In(false, 0.10f, 0.05f, 0.50), p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.event, "END drag released"));
    CHECK(!o.dragging);
    CHECK(!o.withholdGrip);
}
static void drag_withholds_grip()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    m.Step(In(true, 0.10f, 0.05f, 0.01), p);   // DRAG
    // a fast hand, a body the hand has outrun (beyond reach), long past the settle time: still dragging, grip still withheld
    const Output a = m.Step(In(true, 0.60f, 2.50f, 0.50), p);
    CHECK(a.state == State::Dragging);
    CHECK(a.dragging);
    CHECK(a.withholdGrip);
    CHECK(!a.wantSquare);
    CHECK(a.event == nullptr);
    const Output b = m.Step(In(true, 0.10f, 0.00f, 3.00), p);
    CHECK(b.state == State::Dragging);
    CHECK(b.withholdGrip);
}
static void drag_body_lost_ends()
{
    HandMachine m;
    const Params p = DragOn();
    m.Step(In(false, 0.10f, 0.05f, 0.0), p);
    m.Step(In(true, 0.10f, 0.05f, 0.01), p);   // DRAG
    const Output o = m.Step(In(true, -1.0f, 0.05f, 0.20), p);   // the body is gone
    CHECK(o.state == State::Idle);
    CHECK(Is(o.event, "END body gone"));
    CHECK(!o.dragging);
    // the grip is still held: no new press, so nothing re-arms
    const Output o2 = m.Step(In(true, 0.10f, 0.05f, 0.30), p);
    CHECK(o2.state == State::Idle);
    CHECK(o2.event == nullptr);
}
static void second_hand_on_looted_torso_drags()
{
    // spec 6b.4: one search per body; the other hand's torso press is a drag, not "other hand looting"
    HandMachine m;
    const Params p = DragOn();
    Input in0 = Torso(In(false, 0.10f, 0.05f, 0.0));
    in0.otherHandBusy = true;
    m.Step(in0, p);
    Input in1 = Torso(In(true, 0.10f, 0.05f, 0.01));
    in1.otherHandBusy = true;
    const Output o = m.Step(in1, p);
    CHECK(o.state == State::Dragging);
    CHECK(Is(o.event, "DRAG"));
    CHECK(o.noArm == nullptr);
    // with dragging off the old refusal stands
    HandMachine m2;
    Params off;
    m2.Step(in0, off);
    const Output o2 = m2.Step(in1, off);
    CHECK(o2.state == State::Idle);
    CHECK(Is(o2.noArm, "other hand looting"));
}
static void no_start_while_blocked()
{
    HandMachine m;
    const Params p = DragOn();
    Input b0 = In(false, 0.10f, 0.05f, 0.0);
    b0.blocked = true;
    m.Step(b0, p);
    Input b1 = In(true, 0.10f, 0.05f, 0.01);
    b1.blocked = true;
    const Output a = m.Step(b1, p);   // menu / calibration / loading: no grab, no loot
    CHECK(a.state == State::Idle);
    CHECK(Is(a.noArm, "blocked"));
    CHECK(!a.withholdGrip);
    // a held drag ENDS when the block comes up
    HandMachine m2;
    m2.Step(In(false, 0.10f, 0.05f, 0.0), p);
    m2.Step(In(true, 0.10f, 0.05f, 0.01), p);   // DRAG
    Input b2 = In(true, 0.10f, 0.05f, 0.20);
    b2.blocked = true;
    const Output c = m2.Step(b2, p);
    CHECK(c.state == State::Idle);
    CHECK(Is(c.event, "END blocked"));
    CHECK(!c.dragging);
}

// Late severed-limb reading (Params.lateArmS, Input.limbPending): a press with no body waits while the reading is due.
static Params LateParams()
{
    Params p;
    p.dragEnabled = true;
    p.lateArmS = 0.15;
    return p;
}
static void late_reading_arms_a_drag()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    const Output o1 = m.Step(e, p);
    CHECK(o1.state == State::Waiting);
    CHECK(o1.noArm == nullptr);
    CHECK(o1.withholdGrip);
    Input r = In(true, 0.02f, 0.0f, 0.03);   // the limb arrived, in reach, not a torso
    r.limbPending = false;
    const Output o2 = m.Step(r, p);
    CHECK(o2.state == State::Dragging);
    CHECK(Is(o2.event, "DRAG"));
    CHECK(o2.waitedS > 0.019 && o2.waitedS < 0.021);
}
static void late_reading_none_is_a_no_arm()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    m.Step(e, p);
    const Output o = m.Step(In(true, -1.0f, 0.0f, 0.02), p);   // reported: nothing there
    CHECK(o.state == State::Idle);
    CHECK(Is(o.noArm, "no body"));
    CHECK(!o.withholdGrip);
    CHECK(o.waitedS >= 0.0);
    const Output o2 = m.Step(In(true, 0.02f, 0.0f, 0.03), p);   // still held: no new edge, as before
    CHECK(o2.state == State::Idle);
}
static void late_reading_times_out()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    m.Step(e, p);
    Input w = In(true, -1.0f, 0.0f, 0.10);
    w.limbPending = true;
    CHECK(m.Step(w, p).state == State::Waiting);
    w.nowS = 0.17;
    const Output o = m.Step(w, p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.noArm, "no body"));
}
static void late_wait_off_when_not_pending_or_disabled()
{
    HandMachine m;
    Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    const Output o = m.Step(In(true, -1.0f, 0.0f, 0.01), p);   // reading already in: no wait
    CHECK(o.state == State::Idle);
    CHECK(Is(o.noArm, "no body"));
    CHECK(o.waitedS < 0.0);
    HandMachine m2;
    p.lateArmS = 0.0;
    m2.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    CHECK(Is(m2.Step(e, p).noArm, "no body"));   // lateArmS 0 = exactly as before
}
static void late_wait_never_holds_a_blocked_press()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    e.gripOwnedElsewhere = true;
    const Output o = m.Step(e, p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.noArm, "grip owned elsewhere"));
}
static void late_wait_grip_opened()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, -1.0f, 0.0f, 0.0), p);
    Input e = In(true, -1.0f, 0.0f, 0.01);
    e.limbPending = true;
    m.Step(e, p);
    const Output o = m.Step(In(false, -1.0f, 0.0f, 0.05), p);
    CHECK(o.state == State::Idle);
    CHECK(Is(o.noArm, "grip opened while waiting for the limb reading"));
    CHECK(!o.withholdGrip);
}
static void late_wait_holds_a_press_with_a_body_in_reach()
{
    // hitbox_dump_run1: a corpse's box covers the hand and a severed limb lies by it - the press must wait for the limb reading
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, 0.0f, 0.0f, 0.0), p);
    Input e = In(true, 0.0f, 0.0f, 0.01);   // the corpse in reach at once
    e.limbPending = true;
    const Output o1 = m.Step(e, p);
    CHECK(o1.state == State::Waiting);
    CHECK(o1.event == nullptr);
    CHECK(o1.withholdGrip);
    Input r = In(true, 0.01f, 0.0f, 0.02);   // the reading is in (looting.cpp now hands the limb as the body)
    const Output o2 = m.Step(r, p);
    CHECK(o2.state == State::Dragging);
    CHECK(Is(o2.event, "DRAG"));
    CHECK(o2.waitedS > 0.009 && o2.waitedS < 0.011);
}
static void late_wait_then_a_torso_press_still_arms()
{
    HandMachine m;
    const Params p = LateParams();
    m.Step(In(false, 0.0f, 0.0f, 0.0), p);
    Input e = In(true, 0.0f, 0.0f, 0.01);
    e.limbPending = true;
    e.onTorso = true;
    m.Step(e, p);
    Input r = In(true, 0.0f, 0.0f, 0.03);
    r.onTorso = true;
    const Output o = m.Step(r, p);
    CHECK(o.state == State::Settling);
    CHECK(Is(o.event, "ARM"));
}

int main()
{
    late_reading_arms_a_drag();
    late_reading_none_is_a_no_arm();
    late_reading_times_out();
    late_wait_off_when_not_pending_or_disabled();
    late_wait_never_holds_a_blocked_press();
    late_wait_grip_opened();
    late_wait_holds_a_press_with_a_body_in_reach();
    late_wait_then_a_torso_press_still_arms();
    drag_on_non_torso_press();
    torso_press_still_loots();
    torso_press_moving_drags();
    drag_disabled_keeps_spent();
    drag_ends_on_grip_open();
    drag_withholds_grip();
    drag_body_lost_ends();
    second_hand_on_looted_torso_drags();
    no_start_while_blocked();
    latch_blocks_a_press_made_on_a_target();
    latch_passes_a_press_made_off_target();
    latch_first_sample_down_is_not_a_press();
    box_dist_inside_is_zero();
    box_dist_outside_one_axis_and_corner();
    nearer_of_two_boxes_and_which();
    nearer_ignores_a_missing_box();
    no_arm_reason_names_each_refusal();
    no_arm_reason_only_on_a_press_edge();
    slow_frame_stepped_hand_reads_slow();
    fast_hand_reads_fast();
    still_hand_reads_zero();
    unfilled_window_reads_minus_one();
    reset_drops_history();
    window_is_about_100_ms();
    ring_overflow_keeps_the_newest();
    arms_on_press_edge_near_body();
    no_arm_when_grip_already_held();
    no_arm_out_of_reach();
    no_arm_without_body();
    no_arm_when_grip_owned_elsewhere();
    no_arm_when_other_hand_busy();
    settles_into_search();
    moving_during_settle_spends_the_press();
    body_lost_during_settle();
    search_ends_on_grip_open();
    grip_open_during_settle_ends_without_search();
    search_keeps_square_while_held_even_if_hand_moves();
    withhold_only_while_settling_or_searching();
    params_are_read_every_step();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
