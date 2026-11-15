// Grab bodies by hand - the spring that pulls the grabbed body toward the hand (docs/superpowers/specs/2026-10-23-body-grab-design.md,
// plan Task 2). Pure: no Windows, no game memory, no logging, so src/src/tests/bodygrab_logic_test.cpp compiles it on its own.
// bodygrab.cpp calls it once per player update (HookTail) per grabbing hand and hands the force to IPhBody::AddForceAtPos at the grab
// point (Build constants (a4): the force lands in the NEXT frame's physics step).
//
// F = m * w^2 * (hand - point) - 2 * z * m * w * (pointVel - handVel), w = 2 * pi * hz: a mass-scaled spring-damper, so every body
// follows with the same feel whatever it weighs. Its length is capped at m * maxAccel - the "bit of weight": a hand that moves away
// faster than the cap allows leaves the body behind, and a tracking jump cannot launch it.
#pragma once

#include <cmath>

namespace bodygrab {

struct SpringParams {
    float hz = 4.0f;          // body_grab_hz: the spring's natural frequency
    float damping = 0.7f;     // body_grab_damping: damping ratio (1 = critical)
    float maxAccel = 30.0f;   // body_grab_max_accel: m/s^2, the force cap per kg
};

struct SpringOut {
    float force[3];   // newtons, world frame
    bool capped;      // the cap shortened the force this call
    float errLen;     // |hand - point|, metres
};

inline SpringOut SpringForce(float mass, const SpringParams& p, const float hand[3], const float handVel[3], const float point[3],
                             const float pointVel[3])
{
    const float w = 6.28318530718f * p.hz;
    const float k = mass * w * w;
    const float c = 2.0f * p.damping * mass * w;
    SpringOut o{};
    float e2 = 0.0f, f2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float e = hand[i] - point[i];
        e2 += e * e;
        o.force[i] = k * e - c * (pointVel[i] - handVel[i]);
        f2 += o.force[i] * o.force[i];
    }
    o.errLen = std::sqrt(e2);
    const float cap = mass * p.maxAccel;
    const float fl = std::sqrt(f2);
    o.capped = fl > cap;
    if (o.capped) {
        const float s = cap / fl;
        for (int i = 0; i < 3; ++i) { o.force[i] *= s; }
    }
    return o;
}

// The hand against a ragdoll part's own collision shape - its hitbox (Jay 10-24 (2): "The bone sets should have a radius like an
// actual hitbox"; RESEARCH-BODY-GRAB "READS FOR THE THREE REFINEMENTS" A). The shape's fields as OUR engine's own AABB functions read
// them (ida/bg55): centre c, rotation q stored (x, y, z, w); sphere = radius; capsule = radius + cylinder length (full) along the
// rotation's local Z; box = the three full side lengths. Returns the distance from p to the shape's surface: 0 or below = inside.
enum class Shape { Sphere = 1, Capsule = 2, Box = 3 };

// Rotates v by the unit quaternion q (x, y, z, w); inverse = rotate by the conjugate.
inline void QuatRotate(const float q[4], const float v[3], bool inverse, float out[3])
{
    const float x = inverse ? -q[0] : q[0], y = inverse ? -q[1] : q[1], z = inverse ? -q[2] : q[2], w = q[3];
    // t = 2 * cross(q.xyz, v); out = v + w * t + cross(q.xyz, t)
    const float t0 = 2.0f * (y * v[2] - z * v[1]);
    const float t1 = 2.0f * (z * v[0] - x * v[2]);
    const float t2 = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * t0 + (y * t2 - z * t1);
    out[1] = v[1] + w * t1 + (z * t0 - x * t2);
    out[2] = v[2] + w * t2 + (x * t1 - y * t0);
}

// a * b, quaternions (x, y, z, w) - OUR engine +0EA040 (ida/bg57): the Hamilton product, a applied after b.
inline void QuatMul(const float a[4], const float b[4], float out[4])
{
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2];
    out[2] = a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0];
}

// A shape inside an ode::dxGeomTransform (OUR engine +3744E0, ida/bg56): its own position / rotation are an offset in the
// transform's frame; world centre = transform position + transform rotation * local position, world rotation = transform rotation *
// local rotation.
inline void ComposePose(const float pT[3], const float qT[4], const float lp[3], const float lq[4], float c[3], float q[4])
{
    float r[3] = {};
    QuatRotate(qT, lp, false, r);
    for (int i = 0; i < 3; ++i) { c[i] = pT[i] + r[i]; }
    QuatMul(qT, lq, q);
}

inline void QuatConj(const float q[4], float out[4]) { out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3]; }

// The rotation about world Y by `angle` radians, (x, y, z, w). looting.cpp HandWorld places the hand with exactly this rotation of
// the tracked offset (x' = x cos + z sin, z' = -x sin + z cos), so the hand's world rotation = YawQuat(angle) * grip rotation.
inline void YawQuat(float angle, float out[4])
{
    out[0] = 0.0f; out[1] = std::sin(0.5f * angle); out[2] = 0.0f; out[3] = std::cos(0.5f * angle);
}

// The shortest rotation q as a rotation vector (axis * angle, radians, |angle| <= pi).
inline void QuatToRotVec(const float q[4], float out[3])
{
    const float s = q[3] < 0.0f ? -1.0f : 1.0f;   // q and -q are the same rotation: take the short way
    const float x = s * q[0], y = s * q[1], z = s * q[2], w = s * q[3];
    const float n = std::sqrt(x * x + y * y + z * z);
    if (n < 1.0e-7f) { out[0] = 2.0f * x; out[1] = 2.0f * y; out[2] = 2.0f * z; return; }
    const float a = 2.0f * std::atan2(n, w);
    out[0] = x / n * a; out[1] = y / n * a; out[2] = z / n * a;
}

// Severed-limb rotation hold (Jay 10-24 (2): "Follow my hand's rotation"): the torque that turns a body toward `target`, a
// mass-free spring-damper per axis scaled by the body's own inertia - tau = I_world * (w^2 * err - 2 z w * angVel), w = 2 pi hz.
// Only the SPRING part is capped at maxAngAccel (rad/s^2); the damping part never is (limb_hold_run1: the cap bit 8-52% of
// updates, mostly on the damping, while the limb shook at 5.5-8.4 rad/s with a 6-21 deg error - a capped damper cannot stop a
// shake). `inertiaLocal` = the 3 x 3 inertia in the body frame (row-major), `q` the body's rotation, `angVel` its world angular
// velocity.
struct AngularOut {
    float torque[3];   // N m, world frame
    bool capped;       // the cap shortened the angular acceleration
    float errAngle;    // radians from the target
};
inline AngularOut AngularSpring(const float inertiaLocal[9], const float q[4], const float angVel[3], const float target[4], float hz,
                                float damping, float maxAngAccel)
{
    AngularOut o{};
    float qc[4] = {}, e[4] = {}, err[3] = {};
    QuatConj(q, qc);
    QuatMul(target, qc, e);   // the world-frame rotation from the body to the target
    QuatToRotVec(e, err);
    o.errAngle = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    const float w = 6.28318530718f * hz;
    float spring[3] = {}, acc[3] = {};
    float s2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        spring[i] = w * w * err[i];
        s2 += spring[i] * spring[i];
    }
    const float sl = std::sqrt(s2);
    const float k = (sl > maxAngAccel && sl > 0.0f) ? maxAngAccel / sl : 1.0f;
    o.capped = k < 1.0f;
    for (int i = 0; i < 3; ++i) { acc[i] = k * spring[i] - 2.0f * damping * w * angVel[i]; }
    // tau = R * I * R^T * acc
    float local[3] = {}, il[3] = {};
    QuatRotate(q, acc, true, local);
    for (int r = 0; r < 3; ++r) {
        il[r] = inertiaLocal[3 * r] * local[0] + inertiaLocal[3 * r + 1] * local[1] + inertiaLocal[3 * r + 2] * local[2];
    }
    QuatRotate(q, il, false, o.torque);
    return o;
}

inline float HitboxDist(Shape s, const float c[3], const float q[4], const float prm[3], const float p[3])
{
    const float d[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
    if (s == Shape::Sphere) { return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - prm[0]; }
    float l[3] = {};
    QuatRotate(q, d, true, l);   // the point in the shape's own frame
    if (s == Shape::Capsule) {
        const float h = 0.5f * prm[1];
        const float z = l[2] < -h ? -h : (l[2] > h ? h : l[2]);
        const float e[3] = { l[0], l[1], l[2] - z };
        return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) - prm[0];
    }
    // box: outside = distance to the box; inside = minus the distance to the nearest face
    float out2 = 0.0f, inside = 1.0e30f;
    bool in = true;
    for (int k = 0; k < 3; ++k) {
        const float h = 0.5f * prm[k];
        const float a = std::fabs(l[k]);
        if (a > h) { in = false; out2 += (a - h) * (a - h); }
        else if (h - a < inside) { inside = h - a; }
    }
    return in ? -inside : std::sqrt(out2);
}

// The point of a hitbox's surface nearest p (Jay 10-24 (2): grabbed parts "latch to the palm" - this point is pulled into the
// hand). p inside the shape = p itself (the hand is already in it). Same shape fields as HitboxDist.
// The held live enemy's weight (2026-10-31 (6), Jay: "The "no gravity" also causes the entire zombie to float"): with the part's own
// gravity off, the downward force (N, world Y) that gives it `fraction` (0..1, clamped) of its weight. g = 9.81 - the engine's world
// gravity is NOT read; the fraction is a feel value Jay tunes. A bad mass or fraction gives 0.
inline float HeldWeightForce(float mass, float fraction)
{
    if (!std::isfinite(mass) || mass <= 0.0f || !std::isfinite(fraction)) { return 0.0f; }
    const float f = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
    return -9.81f * f * mass;
}

inline void ClosestOnShape(Shape s, const float c[3], const float q[4], const float prm[3], const float p[3], float out[3])
{
    if (HitboxDist(s, c, q, prm, p) <= 0.0f) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    const float d[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
    if (s == Shape::Sphere) {
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int i = 0; i < 3; ++i) { out[i] = c[i] + d[i] / n * prm[0]; }
        return;
    }
    float l[3] = {}, r[3] = {};
    QuatRotate(q, d, true, l);
    if (s == Shape::Capsule) {
        const float h = 0.5f * prm[1];
        const float z = l[2] < -h ? -h : (l[2] > h ? h : l[2]);
        const float e[3] = { l[0], l[1], l[2] - z };
        const float n = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        for (int i = 0; i < 3; ++i) { r[i] = e[i] / n * prm[0]; }
        r[2] += z;
    } else {
        for (int i = 0; i < 3; ++i) {
            const float h = 0.5f * prm[i];
            r[i] = l[i] < -h ? -h : (l[i] > h ? h : l[i]);
        }
    }
    float w[3] = {};
    QuatRotate(q, r, false, w);
    for (int i = 0; i < 3; ++i) { out[i] = c[i] + w[i]; }
}

}  // namespace bodygrab
