// Standalone unit test for src/src/ovr2xr/uipanel_logic.h - NOT part of the DLL build.
// docs/SPEC-UI-MENU-PANEL-2026-10-11.md §7.2. Exit code = number of failures.
#include "uipanel_logic.h"

#include <cstdio>

using namespace dlvr::uipanel;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

// a head turned `yaw` (left positive) then pitched `pitch` (up positive), radians
static void HeadQuat(float yaw, float pitch, float q[4])
{
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    // q = qYaw * qPitch, qYaw = (0, sy, 0, cy), qPitch = (sp, 0, 0, cp)
    q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;
}

// The two headsets measured so far (INV-735): Jay's Quest 3 (2688 x 2880 per eye, rendered +-1.3764 x +-1.4747) and a
// tester's (1728 x 1824, +-1.1504 x +-1.2143) - the game's own frustum, from each run's "HmdDesc ... fov tans" line.
static const EyeTan kQuest3 = { 1.3764f, 1.3764f, 1.4747f, 1.4747f };
static const EyeTan kSmallUi = { 1.1504f, 1.1504f, 1.2143f, 1.2143f };

// Selection wheels (plan 2026-10-27 Task 6, D13): the game's item wheel WITH its name / stats / cash / repair parts in Jay's eye
// image (2688 x 2880) - canvas box (140, 100)-(1140, 620) on the 1280 x 720 canvas (run Q1: the 338 x 338 box cut off the names and
// cash / parts), which spans UiCrop = (589, 1036) 1510 x 808. Centred on the image.
static void wheel_rect_is_the_canvas_wheel()
{
    Rect w{};
    CHECK(WheelRect(2688, 2880, kQuest3, &w));
    CHECK(w.w == 1180 && w.h == 584);                     // 1000 / 1280 * 1510 = 1179.7, 520 / 720 * 808 = 583.6, made even
    CHECK(w.x + w.w / 2 == 1344 && w.y + w.h / 2 == 1440);   // the canvas centre = the image centre
    CHECK(w.x == 754 && w.y == 1148);
    Rect small{};
    CHECK(!WheelRect(10, 10, kQuest3, &small));           // below the floor: refused, never a guessed box
    const EyeTan none = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(!WheelRect(2688, 2880, none, &small));          // tangents not known yet: refused
}

// INV-735: the box comes from the headset's field of view, not from constants. Jay's Quest 3 reproduces the measured box exactly;
// the tester's headset gives its own (the 970 x 512 the old constants gave it mapped every HUD draw 1.21x too far from the centre).
static void crop_is_the_measured_menu_rectangle()
{
    Rect r{};
    CHECK(UiCrop(2688, 2880, kQuest3, &r));
    CHECK(r.w == 1510 && r.h == 808);                     // 56.18 % x 28.09 %, made even
    CHECK(r.x == (2688 - 1510) / 2 && r.y == (2880 - 808) / 2);
    CHECK(r.x + r.w <= 2688 && r.y + r.h <= 2880);
    Rect y{};
    CHECK(UiCrop(1728, 1824, kSmallUi, &y));
    CHECK(y.w == 1178 && y.h == 622);                     // 0.7848 / 1.1504 x 1728, 0.4142 / 1.2143 x 1824, made even
    CHECK(y.x == 275 && y.y == 601);
    CHECK(!(y.w == 970 && y.h == 512));                   // NOT the Quest 3 fractions - the test can miss
    // an asymmetric frustum (wider to the left): the box stays on the projection axis, so it moves right of the image centre
    const EyeTan asym = { 1.6f, 1.1528f, 1.4747f, 1.4747f };
    Rect a{};
    CHECK(UiCrop(2688, 2880, asym, &a));
    CHECK(a.x + a.w / 2 > 1344 + 100);                    // the axis sits at 2688 x 1.6 / 2.7528 = 1562
    const Rect all = Crop(2688, 2880);
    CHECK(all.x == 0 && all.y == 0 && all.w == 2688 && all.h == 2880);
    Rect tiny{};
    CHECK(!UiCrop(32, 32, kQuest3, &tiny));               // nonsense size: refused, never a zero rectangle
}

// With the eye image declared +-1.3764 x +-1.4747 the menu rectangle must come out as the UI plane's own angle:
// 2 x 0.7732 x distance wide, 2 x 0.4142 x distance high (3.09 m x 1.66 m at 2 m).
static void size_keeps_the_angle_the_menu_has_today()
{
    Rect r{};
    CHECK(UiCrop(2688, 2880, kQuest3, &r));
    float w = 0.0f, h = 0.0f;
    SizeM(r, 2688, 2880, -1.3764f, 1.3764f, -1.4747f, 1.4747f, 2.0f, 1.0f, &w, &h);
    CHECK(Near(w, 2.0f * 2.0f * 0.7732f, 0.01f));
    CHECK(Near(h, 2.0f * 2.0f * 0.4142f, 0.01f));
    SizeM(r, 2688, 2880, -1.3764f, 1.3764f, -1.4747f, 1.4747f, 2.0f, 0.5f, &w, &h);
    CHECK(Near(w, 2.0f * 0.7732f, 0.01f));
    // an asymmetric eye (wider outward) changes nothing about how the span is taken
    SizeM(r, 2688, 2880, -1.6f, 1.1528f, -1.4747f, 1.4747f, 2.0f, 1.0f, &w, &h);
    CHECK(Near(w, 2.0f * 2.0f * 0.7732f, 0.01f));
}

static void straight_ahead_puts_it_in_front_and_below()
{
    const float pos[3] = { 0.3f, 1.6f, -0.2f };
    float q[4];
    HeadQuat(0.0f, 0.0f, q);
    float p[3], pq[4];
    CHECK(Latch(pos, q, 2.0f, -0.25f, p, pq));
    CHECK(Near(p[0], 0.3f, 1e-4f) && Near(p[1], 1.35f, 1e-4f) && Near(p[2], -2.2f, 1e-4f));
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float face[3];
    Rotate(pq, z, face);                                  // the panel's face looks back at the head: +z
    CHECK(Near(face[0], 0.0f, 1e-4f) && Near(face[2], 1.0f, 1e-4f));
}

// Head turned 90 degrees LEFT looks along -x: the panel is 2 m along -x and its face looks along +x.
static void turned_left_puts_it_to_the_left()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float q[4];
    HeadQuat(1.5707963f, 0.0f, q);
    float p[3], pq[4];
    CHECK(Latch(pos, q, 2.0f, 0.0f, p, pq));
    CHECK(Near(p[0], -2.0f, 1e-3f) && Near(p[2], 0.0f, 1e-3f) && Near(p[1], 1.6f, 1e-4f));
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float face[3];
    Rotate(pq, z, face);
    CHECK(Near(face[0], 1.0f, 1e-3f) && Near(face[2], 0.0f, 1e-3f));
    // the control: a head turned RIGHT must give the opposite side
    HeadQuat(-1.5707963f, 0.0f, q);
    CHECK(Latch(pos, q, 2.0f, 0.0f, p, pq));
    CHECK(Near(p[0], 2.0f, 1e-3f));
}

// Looking 40 degrees DOWN when the menu opens must not tilt the panel or pull it closer: same heading, upright, 2 m.
static void pitch_does_not_tilt_or_pull_the_panel()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float q[4];
    HeadQuat(0.6f, -0.698f, q);
    float p[3], pq[4];
    CHECK(Latch(pos, q, 2.0f, -0.25f, p, pq));
    CHECK(Near(std::sqrt(p[0] * p[0] + p[2] * p[2]), 2.0f, 1e-3f));
    CHECK(Near(p[1], 1.35f, 1e-4f));
    CHECK(Near(std::atan2(-p[0], -p[2]), 0.6f, 1e-3f));  // the heading is the yaw alone
    CHECK(Near(pq[0], 0.0f, 1e-6f) && Near(pq[2], 0.0f, 1e-6f));   // a turn about the vertical only
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float u[3];
    Rotate(pq, up, u);
    CHECK(Near(u[1], 1.0f, 1e-5f));
}

static void looking_straight_up_gives_no_heading()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float q[4];
    HeadQuat(0.0f, 1.5707963f, q);
    float p[3], pq[4];
    CHECK(!Latch(pos, q, 2.0f, 0.0f, p, pq));
}

// The declared pitch about the eye's own x axis (ovr2xr.cpp PitchPose).
static void Pitched(const float q[4], float deg, float out[4])
{
    const float h = deg * 3.14159265f / 360.0f;
    const float p[4] = { std::sin(h), 0.0f, 0.0f, std::cos(h) };
    QuatMul(q, p, out);
}

// Head turned 30 degrees, image declared 5.5 degrees pitched: the panel as the DECLARED camera sees it must be the latched
// panel as the RENDERED camera sees it - position and orientation. Without the fix they differ by a roll of ~2.7 degrees.
static void redeclared_panel_is_rigid_against_the_game_image()
{
    const float head[3] = { 0.1f, 1.6f, 0.2f };
    float q0[4];
    HeadQuat(0.0f, 0.0f, q0);
    float pPos[3], pQuat[4];
    CHECK(Latch(head, q0, 2.0f, -0.25f, pPos, pQuat));
    for (int k = -1; k <= 1; k += 2) {
        float qR[4], qD[4];
        HeadQuat(0.5235988f * k, 0.1f, qR);            // now: 30 degrees left / right, a little up
        Pitched(qR, -5.5f, qD);
        float oPos[3], oQuat[4];
        Redeclare(pPos, pQuat, head, qR, qD, oPos, oQuat);
        const float invR[4] = { -qR[0], -qR[1], -qR[2], qR[3] }, invD[4] = { -qD[0], -qD[1], -qD[2], qD[3] };
        const float dR[3] = { pPos[0] - head[0], pPos[1] - head[1], pPos[2] - head[2] };
        const float dD[3] = { oPos[0] - head[0], oPos[1] - head[1], oPos[2] - head[2] };
        float a[3], b[3], qa[4], qb[4];
        Rotate(invR, dR, a);
        Rotate(invD, dD, b);
        CHECK(Near(a[0], b[0], 1e-4f) && Near(a[1], b[1], 1e-4f) && Near(a[2], b[2], 1e-4f));
        QuatMul(invR, pQuat, qa);
        QuatMul(invD, oQuat, qb);
        CHECK(Near(std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]), 1.0f, 1e-5f));
        // the control: WITHOUT the fix the panel's "up" as the declared camera sees it is rolled by about
        // 5.5 x sin(30) = 2.75 degrees, and the sign flips with the side - the defect Jay described
        // (measured as the tilt, in the picture, of the panel's vertical centre line: its top and bottom points projected)
        auto tilt = [&](const float inv[4], const float pos[3], const float quat[4]) {
            const float up[3] = { 0.0f, 0.5f, 0.0f };
            float u[3];
            Rotate(quat, up, u);
            const float top[3] = { pos[0] + u[0] - head[0], pos[1] + u[1] - head[1], pos[2] + u[2] - head[2] };
            const float bot[3] = { pos[0] - u[0] - head[0], pos[1] - u[1] - head[1], pos[2] - u[2] - head[2] };
            float t[3], b2[3];
            Rotate(inv, top, t);
            Rotate(inv, bot, b2);
            const float tx = t[0] / -t[2], ty = t[1] / -t[2], bx = b2[0] / -b2[2], by = b2[1] / -b2[2];
            return std::atan2(tx - bx, ty - by) * 57.29578f;
        };
        const float want = tilt(invR, pPos, pQuat);                 // how the game's own picture would show it
        const float unfixed = tilt(invD, pPos, pQuat) - want;       // the latched pose submitted as it is
        const float fixed = tilt(invD, oPos, oQuat) - want;         // the re-declared pose
        CHECK(std::fabs(unfixed) > 2.0f && std::fabs(unfixed) < 4.0f);
        static float first = 0.0f;
        if (k == -1) { first = unfixed; } else { CHECK(first * unfixed < 0.0f); }   // the sign flips with the side
        CHECK(std::fabs(fixed) < 0.01f);
    }
    // no pitch declared: nothing changes
    float qR[4], oPos[3], oQuat[4];
    HeadQuat(0.4f, 0.0f, qR);
    Redeclare(pPos, pQuat, head, qR, qR, oPos, oQuat);
    CHECK(Near(oPos[0], pPos[0], 1e-5f) && Near(oPos[1], pPos[1], 1e-5f) && Near(oPos[2], pPos[2], 1e-5f));
}

// HUD PANEL (SPEC-UI-HUD-PANEL-2026-10-11 §4.4). Sideways is the HEAD's right along the floor, whatever the pitch.
static void hud_side_offset_is_to_the_heads_right()
{
    const float pos[3] = { 0.3f, 1.6f, -0.2f };
    float q[4], p[3], pq[4], p0[3], pq0[4];
    HeadQuat(0.0f, 0.0f, q);                              // looking along -z: right = +x
    CHECK(LatchSide(pos, q, 2.0f, -0.25f, 0.4f, p, pq));
    CHECK(Near(p[0], 0.7f, 1e-4f) && Near(p[1], 1.35f, 1e-4f) && Near(p[2], -2.2f, 1e-4f));
    CHECK(LatchSide(pos, q, 2.0f, -0.25f, -0.4f, p, pq)); // the control: negative = LEFT
    CHECK(Near(p[0], -0.1f, 1e-4f));
    HeadQuat(1.5707963f, -0.5f, q);                       // turned 90 degrees LEFT (looks along -x), looking down: right = -z
    CHECK(LatchSide(pos, q, 2.0f, 0.0f, 0.4f, p, pq));
    CHECK(Near(p[0], 0.3f - 2.0f, 1e-3f) && Near(p[2], -0.2f - 0.4f, 1e-3f) && Near(p[1], 1.6f, 1e-4f));
    // no offset = the menu panel's own latch, position and facing
    CHECK(LatchSide(pos, q, 2.0f, -0.25f, 0.0f, p, pq));
    CHECK(Latch(pos, q, 2.0f, -0.25f, p0, pq0));
    CHECK(Near(p[0], p0[0], 1e-6f) && Near(p[2], p0[2], 1e-6f) && Near(pq[1], pq0[1], 1e-6f) && Near(pq[3], pq0[3], 1e-6f));
    // the offset slides the panel, it does not turn it
    CHECK(LatchSide(pos, q, 2.0f, -0.25f, 0.9f, p, pq));
    CHECK(Near(pq[1], pq0[1], 1e-6f) && Near(pq[3], pq0[3], 1e-6f));
    HeadQuat(0.0f, 1.5707963f, q);                        // straight up: no heading, no latch
    CHECK(!LatchSide(pos, q, 2.0f, 0.0f, 0.4f, p, pq));
}

static void hud_relatch_rules()
{
    const float a[3] = { 1.8f, -0.25f, 0.0f };
    float b[3] = { 1.8f, -0.25f, 0.0f };
    CHECK(HudRelatch(false, false, 0, false, a, b));      // never latched
    CHECK(!HudRelatch(true, false, 0, false, a, b));      // latched, nothing changed: KEPT
    CHECK(HudRelatch(true, true, 0, false, a, b));        // the re-centre button
    CHECK(HudRelatch(true, false, 1500, false, a, b));    // back from a load
    CHECK(!HudRelatch(true, false, 400, false, a, b));    // a blink is not a load
    CHECK(!HudRelatch(true, false, 60000, true, a, b));   // a menu held open for a minute: KEPT (the spec's rule)
    b[0] = 2.0f;
    CHECK(HudRelatch(true, false, 0, false, a, b));       // the distance slider moved
    b[0] = 1.8f; b[1] = -0.30f;
    CHECK(HudRelatch(true, false, 0, false, a, b));       // height
    b[1] = -0.25f; b[2] = 0.1f;
    CHECK(HudRelatch(true, false, 0, true, a, b));        // sideways, even with a menu just closed
}

// HUD PANEL LAZY FOLLOW (docs/superpowers/specs/2026-10-20-hud-panel-lazy-follow-design.md §6). Defaults: 45 deg, 500 ms,
// 300 ms glide, 0.5 m drift; the panel latched 0.5 m ahead, 0.05 m below, no sideways (Jay's disk).
static FollowCfg Cfg() { return FollowCfg{ 45.0f, 500, 300, 0.5f, 0.5f, -0.05f, 0.0f }; }

static float PanelYawDeg(const float q[4]) { return 2.0f * std::atan2(q[1], q[3]) * 57.29578f; }

// A panel latched straight ahead of a head at `pos`, and the follow state reset at that latch.
static void LatchedAhead(const float pos[3], float p[3], float pq[4], FollowState& s)
{
    float q[4];
    HeadQuat(0.0f, 0.0f, q);
    const FollowCfg c = Cfg();
    LatchSide(pos, q, c.distance, c.height, c.side, p, pq);
    HudFollowReset(s, pos);
}

// Runs the head at `yawDeg` from `t0` to `t1` ms in 10 ms steps; returns the last result.
static FollowResult Hold(FollowState& s, const float pos[3], float yawDeg, unsigned long long t0, unsigned long long t1,
                         float p[3], float pq[4])
{
    float q[4];
    HeadQuat(yawDeg / 57.29578f, 0.0f, q);
    FollowResult r = kFollowIdle;
    for (unsigned long long t = t0; t <= t1; t += 10) { r = HudFollowStep(s, pos, q, t, Cfg(), p, pq); }
    return r;
}

static void follow_a_small_turn_never_moves_the_panel()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4], p0[3], pq0[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    for (int i = 0; i < 3; ++i) { p0[i] = p[i]; }
    for (int i = 0; i < 4; ++i) { pq0[i] = pq[i]; }
    CHECK(Hold(s, pos, 30.0f, 0, 5000, p, pq) == kFollowIdle);       // 30 deg for 5 s: under 45
    CHECK(Hold(s, pos, -44.0f, 5010, 9000, p, pq) == kFollowIdle);   // the other side, just under
    CHECK(p[0] == p0[0] && p[1] == p0[1] && p[2] == p0[2]);           // bit-identical: Idle never writes
    CHECK(pq[0] == pq0[0] && pq[1] == pq0[1] && pq[2] == pq0[2] && pq[3] == pq0[3]);
    CHECK(s.follows == 0);
}

static void follow_a_glance_is_cancelled()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    CHECK(Hold(s, pos, 60.0f, 0, 400, p, pq) == kFollowArmed);      // 400 ms past the line: armed, not yet moving
    CHECK(Hold(s, pos, 0.0f, 410, 3000, p, pq) == kFollowIdle);     // back in front: cancelled
    CHECK(s.cancels == 1 && s.follows == 0);
    CHECK(Near(PanelYawDeg(pq), 0.0f, 1e-3f));
}

static void follow_a_held_turn_brings_it_in_front()
{
    const float pos[3] = { 0.2f, 1.6f, -0.1f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    // 90 deg LEFT held: armed at 0, glide from 500 ms, settled by 500 + 300 ms
    const FollowResult mid = Hold(s, pos, 90.0f, 0, 650, p, pq);
    CHECK(mid == kFollowGliding);
    const float midYaw = PanelYawDeg(pq);
    CHECK(midYaw > 5.0f && midYaw < 85.0f);                          // part-way, never past
    CHECK(Hold(s, pos, 90.0f, 660, 900, p, pq) == kFollowIdle);     // settled at ~800 ms, idle after
    CHECK(s.follows == 1);
    CHECK(Near(PanelYawDeg(pq), 90.0f, 2.0f));
    float q[4], want[3], wq[4];
    HeadQuat(1.5707963f, 0.0f, q);
    LatchSide(pos, q, Cfg().distance, Cfg().height, Cfg().side, want, wq);   // where a latch now would put it
    CHECK(Near(p[0], want[0], 0.02f) && Near(p[1], want[1], 1e-4f) && Near(p[2], want[2], 0.02f));
    // the control: turned RIGHT the same amount, it goes the other way
    FollowState s2{};
    float p2[3], pq2[4];
    LatchedAhead(pos, p2, pq2, s2);
    Hold(s2, pos, -90.0f, 0, 1000, p2, pq2);
    CHECK(Near(PanelYawDeg(pq2), -90.0f, 2.0f));
}

static void follow_chases_a_head_still_turning()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    CHECK(Hold(s, pos, 60.0f, 0, 520, p, pq) == kFollowGliding);    // the glide has started toward 60
    // the head keeps turning to 150 during the glide: the panel ends facing 150, not 60
    float q[4];
    FollowResult r = kFollowIdle;
    for (unsigned long long t = 530; t <= 2000; t += 10) {
        const float yaw = t < 800 ? 60.0f + (static_cast<float>(t) - 520.0f) * (90.0f / 280.0f) : 150.0f;
        HeadQuat(yaw / 57.29578f, 0.0f, q);
        r = HudFollowStep(s, pos, q, t, Cfg(), p, pq);
    }
    CHECK(r == kFollowIdle && s.follows == 1);
    CHECK(Near(PanelYawDeg(pq), 150.0f, 2.0f));
    // across +-180: a head at 170 then -170 is a 20 deg change, not 340
    FollowState s3{};
    float p3[3], pq3[4];
    LatchedAhead(pos, p3, pq3, s3);
    Hold(s3, pos, 170.0f, 0, 1000, p3, pq3);
    CHECK(Near(PanelYawDeg(pq3), 170.0f, 2.0f));
    CHECK(Hold(s3, pos, -170.0f, 1010, 3000, p3, pq3) == kFollowIdle);   // 20 deg away: no follow
    CHECK(s3.follows == 1);
}

static void follow_walking_away_brings_it_along()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    const float near[3] = { 0.3f, 1.6f, 0.2f };                      // 0.36 m: under 0.5
    CHECK(Hold(s, near, 0.0f, 0, 2000, p, pq) == kFollowIdle);
    const float far[3] = { 0.5f, 1.6f, 0.3f };                       // 0.58 m: over
    Hold(s, far, 0.0f, 2010, 3500, p, pq);
    CHECK(s.follows == 1);
    float q[4], want[3], wq[4];
    HeadQuat(0.0f, 0.0f, q);
    LatchSide(far, q, Cfg().distance, Cfg().height, Cfg().side, want, wq);
    CHECK(Near(p[0], want[0], 1e-3f) && Near(p[2], want[2], 1e-3f));
    // the drift is measured from the NEW latch now: standing still there is no further follow
    CHECK(Hold(s, far, 0.0f, 3510, 6000, p, pq) == kFollowIdle && s.follows == 1);
    // height alone (standing up) is not drift
    const float up[3] = { 0.5f, 2.4f, 0.3f };
    CHECK(Hold(s, up, 0.0f, 6010, 8000, p, pq) == kFollowIdle && s.follows == 1);
}

static void follow_no_heading_holds_without_nan()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    CHECK(Hold(s, pos, 60.0f, 0, 300, p, pq) == kFollowArmed);
    float q[4];
    HeadQuat(1.0f, -1.5707963f, q);                                  // straight down: no heading
    CHECK(HudFollowStep(s, pos, q, 310, Cfg(), p, pq) == kFollowNoHeading);
    CHECK(s.phase == 1);                                             // still armed, not cancelled
    CHECK(s.cancels == 0);
    for (float v : p) { CHECK(v == v); }                             // no NaN
    for (float v : pq) { CHECK(v == v); }
}

static void follow_reset_at_a_latch_starts_clean()
{
    const float pos[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(pos, p, pq, s);
    CHECK(Hold(s, pos, 60.0f, 0, 600, p, pq) == kFollowGliding);
    // a real latch mid-glide (the re-centre button, a slider): the glide is dropped, the new latch stands
    float q[4];
    HeadQuat(60.0f / 57.29578f, 0.0f, q);
    LatchSide(pos, q, Cfg().distance, Cfg().height, Cfg().side, p, pq);
    HudFollowReset(s, pos);
    CHECK(s.phase == 0);
    CHECK(Hold(s, pos, 60.0f, 610, 3000, p, pq) == kFollowIdle);
    CHECK(Near(PanelYawDeg(pq), 60.0f, 1e-3f));
}

// A physical crouch / stand edge (s.pending, uipanel.cpp from PhysCrouchEdges): the panel follows the head DOWN with no turn
// and no floor drift, after the usual wait, once. The control is follow_walking_away_brings_it_along's "height alone".
static void follow_a_physical_crouch_brings_it_down()
{
    const float stand[3] = { 0.0f, 1.6f, 0.0f };
    float p[3], pq[4];
    FollowState s{};
    LatchedAhead(stand, p, pq, s);
    const float standY = p[1];
    const float crouch[3] = { 0.02f, 1.1f, 0.01f };                  // 0.5 m down, 2 cm along the floor, no turn
    CHECK(Hold(s, crouch, 0.0f, 0, 1000, p, pq) == kFollowIdle);    // no edge: it stays up
    CHECK(Near(p[1], standY, 1e-6f) && s.follows == 0);
    s.pending = true;                                                // the crouch edge
    CHECK(Hold(s, crouch, 0.0f, 1010, 1400, p, pq) == kFollowArmed);   // the wait still applies
    Hold(s, crouch, 0.0f, 1410, 2500, p, pq);
    CHECK(s.follows == 1 && !s.pending);
    float q[4], want[3], wq[4];
    HeadQuat(0.0f, 0.0f, q);
    LatchSide(crouch, q, Cfg().distance, Cfg().height, Cfg().side, want, wq);
    CHECK(Near(p[0], want[0], 1e-3f) && Near(p[1], want[1], 1e-4f) && Near(p[2], want[2], 1e-3f));
    CHECK(Near(p[1], standY - 0.5f, 1e-4f));
    CHECK(Hold(s, crouch, 0.0f, 2510, 5000, p, pq) == kFollowIdle && s.follows == 1);   // once per edge
    // the stand edge brings it back up
    s.pending = true;
    Hold(s, stand, 0.0f, 5010, 7000, p, pq);
    CHECK(s.follows == 2 && Near(p[1], standY, 1e-4f));
    // a real latch drops a pending edge (it has just placed the panel at the head anyway)
    s.pending = true;
    HudFollowReset(s, stand);
    CHECK(!s.pending && Hold(s, stand, 0.0f, 7010, 9000, p, pq) == kFollowIdle && s.follows == 2);
}

// ---- the UI headset sweep (trace 20261113_2125): every wrist / router / wheel number follows the running headset ----

// (1) A wrist piece that is the same SHARE of the HUD comes out the same size on every headset: Jay's minimap 780 of 2880 UI px and
// the tester's 494 of 1824 (the same layout, a smaller UI screen) are both ~9 cm. The Quest 3 value is unchanged.
static void wrist_piece_metres_follow_the_ui_screen()
{
    CHECK(Near(static_cast<float>(MetresPerUiPx(2880.0)), 0.000115f, 1e-9f));
    const float jay = static_cast<float>(780.0 * MetresPerUiPx(2880.0));
    const float smallUi = static_cast<float>(494.0 * MetresPerUiPx(1824.0));
    CHECK(Near(jay, 0.0897f, 1e-3f));
    CHECK(Near(smallUi, jay, 1e-3f));
    CHECK(!Near(static_cast<float>(494.0 * 0.000115), jay, 0.01f));   // the old constant gave it 5.7 cm - the test can miss
    CHECK(MetresPerUiPx(0.0) == 0.0);                                 // unknown -> 0 (the caller refuses), never a guess
}

// (2) The router's tolerances scale with the UI screen: 2 / 24 px on Jay's 2880-high UI, the same share elsewhere.
static void router_tolerances_follow_the_ui_screen()
{
    CHECK(Near(UiPxTol(2.0f, 2880.0f), 2.0f, 1e-5f));
    CHECK(Near(UiPxTol(24.0f, 2880.0f), 24.0f, 1e-5f));
    CHECK(Near(UiPxTol(24.0f, 1824.0f), 15.2f, 1e-3f));
    CHECK(Near(UiPxTol(2.0f, 5760.0f), 4.0f, 1e-5f));
    CHECK(UiPxTol(2.0f, 0.0f) == 0.0f);
}

// (3) A piece bigger than its slot is stepped down to fit (an integer step - nearest texel), never cut.
static void slot_fit_steps_down_instead_of_clipping()
{
    int w = 0, h = 0, s = 0;
    CHECK(FitStep(297, 314, 1024, 512, &w, &h, &s));                 // Jay's quests piece: fits as is
    CHECK(s == 1 && w == 297 && h == 314);
    CHECK(FitStep(594, 628, 1024, 512, &w, &h, &s));                 // the same piece at 2x resolution: 628 > 512 -> step 2
    CHECK(s == 2 && w == 297 && h == 314);
    CHECK(FitStep(3000, 100, 1024, 512, &w, &h, &s));                // wide: step 3 (3000 / 3 = 1000 <= 1024)
    CHECK(s == 3 && w == 1000 && h <= 512);
    CHECK(w <= 1024 && h <= 512);
    CHECK(!FitStep(0, 10, 1024, 512, &w, &h, &s));                   // nothing to fit
}

// (4) The wheel sits on the UI canvas centre = the projection axis, which is the image centre only for a symmetric frustum.
static void wheel_centre_is_the_canvas_centre()
{
    const EyeTan sym = { 1.3764f, 1.3764f, 1.4747f, 1.4747f };
    float cx = 0.0f, cy = 0.0f;
    CHECK(CanvasCentre(2688, 2880, sym, &cx, &cy));
    CHECK(Near(cx, 1344.0f, 1.0f) && Near(cy, 1440.0f, 1.0f));
    const EyeTan asym = { 1.6f, 1.1528f, 1.4747f, 1.4747f };
    CHECK(CanvasCentre(2688, 2880, asym, &cx, &cy));
    CHECK(Near(cx, 2688.0f * 1.6f / 2.7528f, 2.0f));                // ~1562, not 1344
    CHECK(!Near(cx, 1344.0f, 50.0f));
    const EyeTan none = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(!CanvasCentre(2688, 2880, none, &cx, &cy));
}

int main()
{
    wrist_piece_metres_follow_the_ui_screen();
    router_tolerances_follow_the_ui_screen();
    slot_fit_steps_down_instead_of_clipping();
    wheel_centre_is_the_canvas_centre();
    follow_a_physical_crouch_brings_it_down();
    follow_a_small_turn_never_moves_the_panel();
    follow_a_glance_is_cancelled();
    follow_a_held_turn_brings_it_in_front();
    follow_chases_a_head_still_turning();
    follow_walking_away_brings_it_along();
    follow_no_heading_holds_without_nan();
    follow_reset_at_a_latch_starts_clean();
    hud_side_offset_is_to_the_heads_right();
    hud_relatch_rules();
    redeclared_panel_is_rigid_against_the_game_image();
    crop_is_the_measured_menu_rectangle();
    size_keeps_the_angle_the_menu_has_today();
    straight_ahead_puts_it_in_front_and_below();
    turned_left_puts_it_to_the_left();
    pitch_does_not_tilt_or_pull_the_panel();
    looking_straight_up_gives_no_heading();
    wheel_rect_is_the_canvas_wheel();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
