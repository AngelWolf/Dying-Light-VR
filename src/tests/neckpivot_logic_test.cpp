// Standalone unit test for src/src/ovr2xr/neckpivot_logic.h - NOT part of the DLL build.
// The neck pivot's arc under the head-yaw decouple (2026-10-20, arms short after a 180 + recentre). Exit code = failures.
#include "neckpivot_logic.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace dlvr::neckpivot;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

static const float kN[3] = { 0.008f, 0.112f, -0.084f };   // Jay's n (INV-321), metres, head frame
static const float kI[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// Turned yawDeg about +Y (left positive), then pitched pitchDeg about the head's own X (up positive).
static void Head(float yawDeg, float pitchDeg, float q[4])
{
    const float y = yawDeg * 0.017453292f, p = pitchDeg * 0.017453292f;
    const float cy = std::cos(y * 0.5f), sy = std::sin(y * 0.5f), cp = std::cos(p * 0.5f), sp = std::sin(p * 0.5f);
    q[0] = cy * sp; q[1] = sy * cp; q[2] = -sy * sp; q[3] = cy * cp;
}
static void Yaw(float yawDeg, float q[4]) { Head(yawDeg, 0.0f, q); }

static float Len(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void pure_yaw_is_zero_against_its_own_yaw()
{
    for (float deg : { 30.0f, 90.0f, 179.4f, -150.0f }) {
        float q[4], yq[4], d[3];
        Head(deg, 0.0f, q);
        Yaw(deg, yq);
        ArcOffset(q, yq, kN, d);
        CHECK(Len(d) < 1e-5f);
    }
}

static void pure_yaw_against_identity_is_todays_arc()
{
    // Today's (R - I) n: 2 |n_xz| sin(yaw/2) - 168.8 mm at 180 deg, 119.4 mm at 90 (the reading in STATE).
    float q[4], d[3];
    Head(180.0f, 0.0f, q);
    ArcOffset(q, kI, kN, d);
    CHECK(Near(Len(d), 2.0f * std::sqrt(0.008f * 0.008f + 0.084f * 0.084f), 1e-4f));
    Head(90.0f, 0.0f, q);
    ArcOffset(q, kI, kN, d);
    CHECK(Near(Len(d), 2.0f * std::sqrt(0.008f * 0.008f + 0.084f * 0.084f) * 0.70710678f, 1e-4f));
}

static void pitch_only_unchanged()
{
    float q[4], a[3], b[3], yq[4];
    Head(0.0f, -35.0f, q);
    Yaw(0.0f, yq);
    ArcOffset(q, kI, kN, a);     // today
    ArcOffset(q, yq, kN, b);     // yaw-free with zero yaw
    for (int k = 0; k < 3; ++k) { CHECK(Near(a[k], b[k], 1e-6f)); }
    CHECK(Len(a) > 0.03f);       // a 35 deg nod really moves the eye
}

static void pitch_arc_turns_with_the_yaw()
{
    // The same nod, facing 180: the arc has today's pitch-only length, turned by the yaw (x, z negated).
    float q0[4], q[4], yq[4], a[3], b[3];
    Head(0.0f, -35.0f, q0);
    ArcOffset(q0, kI, kN, a);
    Head(180.0f, -35.0f, q);
    Yaw(180.0f, yq);
    ArcOffset(q, yq, kN, b);
    CHECK(Near(b[0], -a[0], 1e-5f));
    CHECK(Near(b[1],  a[1], 1e-5f));
    CHECK(Near(b[2], -a[2], 1e-5f));
}

// Independent reference: rotate v by pitch about X, then by yaw about Y, written out by hand.
static void RotYP(float yawDeg, float pitchDeg, const float v[3], float o[3])
{
    const float y = yawDeg * 0.017453292f, p = pitchDeg * 0.017453292f;
    const float x1 = v[0];
    const float y1 = v[1] * std::cos(p) - v[2] * std::sin(p);
    const float z1 = v[1] * std::sin(p) + v[2] * std::cos(p);
    o[0] = x1 * std::cos(y) + z1 * std::sin(y);
    o[1] = y1;
    o[2] = -x1 * std::sin(y) + z1 * std::cos(y);
}

static void matches_hand_rotation_at_90_with_a_nod()
{
    float q[4], yq[4], d[3], rn[3], yn[3];
    Head(90.0f, -35.0f, q);
    Yaw(90.0f, yq);
    ArcOffset(q, yq, kN, d);
    RotYP(90.0f, -35.0f, kN, rn);
    RotYP(90.0f, 0.0f, kN, yn);
    for (int k = 0; k < 3; ++k) { CHECK(Near(d[k], rn[k] - yn[k], 1e-5f)); }
}

// INV-740: FullArcPoint is roomscale's input. The eyes of a head pivoting on a FIXED neck (eye = neck + R n): the point must not
// move for any turn / nod - and at rest (R = I) it is the eye itself, so roomscale's origin (centred on the at-rest eye) sees 0.
static void full_arc_point_still_on_a_pure_head_motion()
{
    const float neck[3] = { 0.31f, -0.02f, 0.47f };
    float rest[3], q0[4];
    Head(0.0f, 0.0f, q0);
    const float eye0[3] = { neck[0] + kN[0], neck[1] + kN[1], neck[2] + kN[2] };
    FullArcPoint(q0, eye0, kN, rest);
    for (int k = 0; k < 3; ++k) { CHECK(Near(rest[k], eye0[k], 1e-6f)); }
    for (float yaw : { 15.0f, 60.0f, 90.0f, 179.4f, -45.0f, -120.0f }) {
        for (float pitch : { 0.0f, -25.0f, 30.0f }) {
            float q[4], rn[3], eye[3], p[3];
            Head(yaw, pitch, q);
            RotYP(yaw, pitch, kN, rn);
            for (int k = 0; k < 3; ++k) { eye[k] = neck[k] + rn[k]; }
            FullArcPoint(q, eye, kN, p);
            for (int k = 0; k < 3; ++k) { CHECK(Near(p[k], rest[k], 1e-5f)); }
        }
    }
}

// CONTROL (the defect it replaces): today's roomscale input under the decouple - base = the head's own yaw - moves by the yaw arc on
// the same pure turn (> 50 mm at 60 deg), so the test above is not passing because nothing ever moves.
static void yaw_base_point_moves_on_a_pure_turn()
{
    const float neck[3] = { 0.0f, 0.0f, 0.0f };
    float q[4], yq[4], rn[3], eye[3], d[3];
    Head(60.0f, 0.0f, q);
    Yaw(60.0f, yq);
    RotYP(60.0f, 0.0f, kN, rn);
    for (int k = 0; k < 3; ++k) { eye[k] = neck[k] + rn[k]; }
    ArcOffset(q, yq, kN, d);
    const float p[3] = { eye[0] - d[0], eye[1] - d[1], eye[2] - d[2] };
    const float moved[3] = { p[0] - kN[0], p[1] - kN[1], p[2] - kN[2] };   // vs the at-rest eye (neck + n)
    CHECK(Len(moved) > 0.05f);
}

int main()
{
    full_arc_point_still_on_a_pure_head_motion();
    yaw_base_point_moves_on_a_pure_turn();
    matches_hand_rotation_at_90_with_a_nod();
    pure_yaw_is_zero_against_its_own_yaw();
    pure_yaw_against_identity_is_todays_arc();
    pitch_only_unchanged();
    pitch_arc_turns_with_the_yaw();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
