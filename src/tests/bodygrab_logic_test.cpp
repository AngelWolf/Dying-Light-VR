// Standalone unit test for src/src/ovr2xr/bodygrab_logic.h - NOT part of the DLL build.
// Grab bodies by hand: docs/superpowers/specs/2026-10-23-body-grab-design.md, plan Task 2. Exit code = failures.
#include "bodygrab_logic.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using bodygrab::SpringForce;
using bodygrab::SpringOut;
using bodygrab::SpringParams;

static float Len(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static const float kZero[3] = { 0.0f, 0.0f, 0.0f };
static const float kTwoPi = 6.28318530718f;

static void spring_pulls_toward_hand()
{
    // The uncapped law: a cap high enough not to bite (at the default 30 m/s^2 the 0.1 m error would saturate it).
    SpringParams p;
    p.maxAccel = 1000.0f;
    const float m = 10.0f;
    const float hand[3] = { 1.0f, 2.0f, 3.0f };
    const float point[3] = { 0.9f, 2.0f, 3.0f };   // 0.1 m behind the hand along x, both still
    const SpringOut o = SpringForce(m, p, hand, kZero, point, kZero);
    const float w = kTwoPi * p.hz;
    CHECK(o.force[0] > 0.0f);
    CHECK(Near(o.force[1], 0.0f, 1e-3f));
    CHECK(Near(o.force[2], 0.0f, 1e-3f));
    CHECK(Near(Len(o.force), m * w * w * 0.1f, 0.01f * m * w * w * 0.1f));
    CHECK(!o.capped);
    CHECK(Near(o.errLen, 0.1f, 1e-4f));
}
static void spring_damps_relative_velocity()
{
    SpringParams p;
    p.maxAccel = 1000.0f;
    const float m = 10.0f;
    const float hand[3] = { 0.0f, 0.0f, 0.0f };
    const float pointVel[3] = { 0.0f, 0.0f, 1.0f };   // at the hand, moving away along +z at 1 m/s
    const SpringOut o = SpringForce(m, p, hand, kZero, hand, pointVel);
    const float w = kTwoPi * p.hz;
    CHECK(o.force[2] < 0.0f);   // opposes the motion
    CHECK(Near(o.force[2], -2.0f * p.damping * m * w * 1.0f, 0.01f * 2.0f * p.damping * m * w));
    CHECK(Near(o.force[0], 0.0f, 1e-3f));
    CHECK(Near(o.force[1], 0.0f, 1e-3f));
    // the same velocity in the hand as well: no relative motion, no damping
    const SpringOut o2 = SpringForce(m, p, hand, pointVel, hand, pointVel);
    CHECK(Len(o2.force) < 1e-4f);
}
static void spring_cap_limits()
{
    SpringParams p;   // default maxAccel 30
    const float m = 10.0f;
    const float hand[3] = { 5.0f, 0.0f, 0.0f };   // 5 m away
    const SpringOut o = SpringForce(m, p, hand, kZero, kZero, kZero);
    CHECK(o.capped);
    CHECK(Near(Len(o.force), m * p.maxAccel, 1e-2f));
    CHECK(o.force[0] > 0.0f);   // still toward the hand
    CHECK(Near(o.errLen, 5.0f, 1e-4f));
}
static void spring_zero_at_rest()
{
    SpringParams p;
    const float m = 10.0f;
    const float at[3] = { 1.0f, -2.0f, 0.5f };
    const float v[3] = { 0.3f, 0.1f, -0.2f };
    const SpringOut o = SpringForce(m, p, at, v, at, v);
    CHECK(Len(o.force) < 1e-6f);
    CHECK(!o.capped);
    CHECK(o.errLen < 1e-6f);
}

// Hitboxes (bodygrab::HitboxDist): the shape's own frame, (x, y, z, w) quaternions.
using bodygrab::HitboxDist;
using bodygrab::Shape;
static const float kIdent[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

static void hitbox_sphere()
{
    const float c[3] = { 1.0f, 0.0f, 0.0f };
    const float prm[3] = { 0.1f, 0.0f, 0.0f };
    const float out[3] = { 1.3f, 0.0f, 0.0f };
    const float in[3] = { 1.05f, 0.0f, 0.0f };
    CHECK(Near(HitboxDist(Shape::Sphere, c, kIdent, prm, out), 0.2f, 1e-5f));
    CHECK(Near(HitboxDist(Shape::Sphere, c, kIdent, prm, in), -0.05f, 1e-5f));
}
static void hitbox_capsule_axis_is_local_z()
{
    // radius 0.05, cylinder 0.4 along local Z: a point 0.1 m off the axis at mid-length is 0.05 m outside; beyond the end cap the
    // distance is to the end point.
    const float prm[3] = { 0.05f, 0.4f, 0.0f };
    const float side[3] = { 0.1f, 0.0f, 0.1f };
    const float past[3] = { 0.0f, 0.0f, 0.5f };
    const float inAxis[3] = { 0.0f, 0.02f, -0.19f };
    CHECK(Near(HitboxDist(Shape::Capsule, kZero, kIdent, prm, side), 0.05f, 1e-5f));
    CHECK(Near(HitboxDist(Shape::Capsule, kZero, kIdent, prm, past), 0.25f, 1e-5f));
    CHECK(HitboxDist(Shape::Capsule, kZero, kIdent, prm, inAxis) < 0.0f);
    // rotated 90 degrees about Y (q = (0, sin45, 0, cos45)): local Z becomes world +X, so the capsule lies along X.
    const float s = std::sqrt(0.5f);
    const float qy[4] = { 0.0f, s, 0.0f, s };
    const float alongX[3] = { 0.15f, 0.0f, 0.0f };
    const float offX[3] = { 0.0f, 0.0f, 0.15f };
    CHECK(HitboxDist(Shape::Capsule, kZero, qy, prm, alongX) < 0.0f);
    CHECK(Near(HitboxDist(Shape::Capsule, kZero, qy, prm, offX), 0.10f, 1e-4f));
}
static void hitbox_box()
{
    const float prm[3] = { 0.2f, 0.4f, 0.6f };   // full sides
    const float out[3] = { 0.3f, 0.0f, 0.0f };   // 0.2 m past the +x face
    const float corner[3] = { 0.13f, 0.24f, 0.0f };
    const float in[3] = { 0.05f, 0.0f, 0.0f };   // 0.05 m inside the +x face
    CHECK(Near(HitboxDist(Shape::Box, kZero, kIdent, prm, out), 0.2f, 1e-5f));
    CHECK(Near(HitboxDist(Shape::Box, kZero, kIdent, prm, corner), 0.05f, 1e-4f));
    CHECK(Near(HitboxDist(Shape::Box, kZero, kIdent, prm, in), -0.05f, 1e-5f));
}

static void compose_wrapped_capsule()
{
    // body at (1, 0, 0) turned 90 degrees about Y (its local Z -> world +X); the capsule sits 0.2 m along the body's local Z and
    // is itself turned 90 degrees about X (its own Z -> the body's -Y). World: centre (1.2, 0, 0), axis along world -Y.
    const float s = std::sqrt(0.5f);
    const float pT[3] = { 1.0f, 0.0f, 0.0f };
    const float qT[4] = { 0.0f, s, 0.0f, s };
    const float lp[3] = { 0.0f, 0.0f, 0.2f };
    const float lq[4] = { s, 0.0f, 0.0f, s };
    float c[3] = {}, q[4] = {};
    bodygrab::ComposePose(pT, qT, lp, lq, c, q);
    CHECK(Near(c[0], 1.2f, 1e-5f));
    CHECK(Near(c[1], 0.0f, 1e-5f));
    CHECK(Near(c[2], 0.0f, 1e-5f));
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float ax[3] = {};
    bodygrab::QuatRotate(q, z, false, ax);
    float viaSteps[3] = {}, local[3] = {};
    bodygrab::QuatRotate(lq, z, false, local);
    bodygrab::QuatRotate(qT, local, false, viaSteps);   // inner first, then the body: the composed rotation must agree
    CHECK(Near(ax[0], viaSteps[0], 1e-5f) && Near(ax[1], viaSteps[1], 1e-5f) && Near(ax[2], viaSteps[2], 1e-5f));
    CHECK(Near(std::fabs(ax[1]), 1.0f, 1e-5f));
    const float prm[3] = { 0.05f, 0.4f, 0.0f };
    const float onAxis[3] = { 1.2f, 0.15f, 0.0f };   // along the world Y axis, inside the 0.4 m cylinder
    const float side[3] = { 1.2f, 0.0f, 0.15f };     // 0.15 m off the axis
    CHECK(HitboxDist(Shape::Capsule, c, q, prm, onAxis) < 0.0f);
    CHECK(Near(HitboxDist(Shape::Capsule, c, q, prm, side), 0.10f, 1e-4f));
}

static void yaw_quat_matches_handworld_mapping()
{
    // looting.cpp HandWorld: x' = x c + z s, z' = -x s + z c (c, s of the yaw) - YawQuat must rotate a vector the same way
    const float rel = 0.7f, c = std::cos(rel), s = std::sin(rel);
    const float v[3] = { 0.3f, -0.2f, 0.5f };
    float q[4] = {}, r[3] = {};
    bodygrab::YawQuat(rel, q);
    bodygrab::QuatRotate(q, v, false, r);
    CHECK(Near(r[0], v[0] * c + v[2] * s, 1e-5f));
    CHECK(Near(r[1], v[1], 1e-6f));
    CHECK(Near(r[2], -v[0] * s + v[2] * c, 1e-5f));
}
static void rotvec_short_way()
{
    const float s = std::sin(0.25f), c = std::cos(0.25f);   // 0.5 rad about Z
    const float q[4] = { 0.0f, 0.0f, s, c };
    const float qn[4] = { 0.0f, 0.0f, -s, -c };            // the same rotation, other sign
    float a[3] = {}, b[3] = {};
    bodygrab::QuatToRotVec(q, a);
    bodygrab::QuatToRotVec(qn, b);
    CHECK(Near(a[2], 0.5f, 1e-5f) && Near(a[0], 0.0f, 1e-6f));
    CHECK(Near(b[2], 0.5f, 1e-5f));
}
static void angular_spring_turns_toward_target()
{
    const float I[9] = { 0.2f, 0, 0, 0, 0.6f, 0, 0, 0, 0.6f };
    const float zero[3] = { 0, 0, 0 };
    const float s = std::sin(0.05f), c = std::cos(0.05f);   // target 0.1 rad about world Y
    const float target[4] = { 0.0f, s, 0.0f, c };
    const bodygrab::AngularOut o = bodygrab::AngularSpring(I, kIdent, zero, target, 4.0f, 0.7f, 1.0e6f);
    const float w = kTwoPi * 4.0f;
    CHECK(Near(o.errAngle, 0.1f, 1e-4f));
    CHECK(Near(o.torque[1], 0.6f * w * w * 0.1f, 0.01f * 0.6f * w * w * 0.1f));   // Y uses I_yy
    CHECK(Near(o.torque[0], 0.0f, 1e-3f) && Near(o.torque[2], 0.0f, 1e-3f));
    CHECK(!o.capped);
    const bodygrab::AngularOut z = bodygrab::AngularSpring(I, kIdent, zero, kIdent, 4.0f, 0.7f, 1.0e6f);
    CHECK(Near(z.torque[0], 0.0f, 1e-6f) && Near(z.torque[1], 0.0f, 1e-6f) && Near(z.torque[2], 0.0f, 1e-6f));
}
static void angular_spring_damps_and_caps()
{
    const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float spin[3] = { 0.0f, 0.0f, 10.0f };            // spinning about Z at 10 rad/s, at the target
    const bodygrab::AngularOut o = bodygrab::AngularSpring(I, kIdent, spin, kIdent, 4.0f, 0.7f, 1.0e6f);
    CHECK(o.torque[2] < 0.0f);                               // resists the spin
    // the damping part is never capped: a spin at the target is resisted in full whatever the cap
    const bodygrab::AngularOut cpd = bodygrab::AngularSpring(I, kIdent, spin, kIdent, 4.0f, 0.7f, 50.0f);
    CHECK(!cpd.capped);
    CHECK(Near(cpd.torque[2], -2.0f * 0.7f * kTwoPi * 4.0f * 10.0f, 1e-2f));
    // the spring part is: 1 rad off about Z with a 50 rad/s^2 cap -> 50 N m on a unit inertia, plus the (zero) damping
    const float s1 = std::sin(0.5f), c1 = std::cos(0.5f);
    const float far[4] = { 0.0f, 0.0f, s1, c1 };
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    const bodygrab::AngularOut sp = bodygrab::AngularSpring(I, kIdent, still, far, 4.0f, 0.7f, 50.0f);
    CHECK(sp.capped);
    CHECK(Near(sp.torque[2], 50.0f, 1e-3f));
    // a body turned 90 degrees about X: its local Y inertia must land on world Z
    const float Iy[9] = { 1, 0, 0, 0, 5, 0, 0, 0, 1 };
    const float r = std::sqrt(0.5f);
    const float qx[4] = { r, 0.0f, 0.0f, r };
    const float spinZ[3] = { 0.0f, 0.0f, -1.0f };
    const bodygrab::AngularOut t = bodygrab::AngularSpring(Iy, qx, spinZ, qx, 4.0f, 0.7f, 1.0e6f);
    CHECK(Near(t.torque[2], 5.0f * 2.0f * 0.7f * kTwoPi * 4.0f, 1e-2f));
}

static void closest_on_shapes()
{
    float o[3] = {};
    const float sph[3] = { 0.1f, 0.0f, 0.0f };
    const float pOut[3] = { 0.0f, 0.3f, 0.0f };
    bodygrab::ClosestOnShape(Shape::Sphere, kZero, kIdent, sph, pOut, o);
    CHECK(Near(o[0], 0.0f, 1e-6f) && Near(o[1], 0.1f, 1e-6f) && Near(o[2], 0.0f, 1e-6f));
    const float pIn[3] = { 0.02f, 0.03f, 0.0f };   // inside: the point itself
    bodygrab::ClosestOnShape(Shape::Sphere, kZero, kIdent, sph, pIn, o);
    CHECK(Near(o[0], 0.02f, 1e-6f) && Near(o[1], 0.03f, 1e-6f));
    // capsule along X (turned 90 deg about Y): a point beside the middle lands on its side, one past the end on the end cap
    const float s = std::sqrt(0.5f);
    const float qy[4] = { 0.0f, s, 0.0f, s };
    const float cap[3] = { 0.05f, 0.4f, 0.0f };
    const float side[3] = { 0.1f, 0.0f, 0.2f };
    bodygrab::ClosestOnShape(Shape::Capsule, kZero, qy, cap, side, o);
    CHECK(Near(o[0], 0.1f, 1e-5f) && Near(o[1], 0.0f, 1e-5f) && Near(o[2], 0.05f, 1e-5f));
    CHECK(Near(HitboxDist(Shape::Capsule, kZero, qy, cap, o), 0.0f, 1e-5f));
    const float past[3] = { 0.5f, 0.0f, 0.0f };
    bodygrab::ClosestOnShape(Shape::Capsule, kZero, qy, cap, past, o);
    CHECK(Near(o[0], 0.25f, 1e-5f));
    // box: clamps to the faces
    const float box[3] = { 0.2f, 0.4f, 0.6f };
    const float corner[3] = { 0.5f, 0.5f, 0.1f };
    bodygrab::ClosestOnShape(Shape::Box, kZero, kIdent, box, corner, o);
    CHECK(Near(o[0], 0.1f, 1e-6f) && Near(o[1], 0.2f, 1e-6f) && Near(o[2], 0.1f, 1e-6f));
}

// The held live enemy's weight (2026-10-31 (6), Jay: "The "no gravity" also causes the entire zombie to float"): gravity stays off,
// each part is pulled down by this fraction of its own weight. Clamped to 0..1; a bad mass gives nothing.
static void held_weight_force()
{
    CHECK(std::fabs(bodygrab::HeldWeightForce(10.0f, 0.3f) - (-29.43f)) < 0.01f);
    CHECK(bodygrab::HeldWeightForce(10.0f, 0.0f) == 0.0f);
    CHECK(std::fabs(bodygrab::HeldWeightForce(10.0f, 2.0f) - (-98.1f)) < 0.01f);   // clamped to 1
    CHECK(bodygrab::HeldWeightForce(10.0f, -1.0f) == 0.0f);                      // clamped to 0
    CHECK(bodygrab::HeldWeightForce(-5.0f, 0.5f) == 0.0f);
    CHECK(bodygrab::HeldWeightForce(NAN, 0.5f) == 0.0f);
}

int main()
{
    held_weight_force();
    closest_on_shapes();
    yaw_quat_matches_handworld_mapping();
    rotvec_short_way();
    angular_spring_turns_toward_target();
    angular_spring_damps_and_caps();
    compose_wrapped_capsule();
    spring_pulls_toward_hand();
    spring_damps_relative_velocity();
    spring_cap_limits();
    spring_zero_at_rest();
    hitbox_sphere();
    hitbox_capsule_axis_is_local_z();
    hitbox_box();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
