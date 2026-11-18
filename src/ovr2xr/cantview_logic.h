#pragma once
// CANTED VIEWS - parallel conversion (trace docs/traces/20261118_1150_canted_views.md; audit S06 6a-1; STATUS-BOARD B1;
// Jay 2026-11-18: a Pimax Crystal Light player saw "doubled vision" -> "Yes, go ahead").
//
// The game is a LibOVR 1.10 client: ovrEyeRenderDesc carries a translation only (ovr2xr.cpp:24-27), so it renders BOTH
// eyes with ONE orientation - parallel - whatever the headset. A runtime whose two views are turned outward (canted
// displays: Pimax Crystal / Crystal Light 10 deg per eye by review figures; Index 5 deg per Valve) then got the LEFT eye's
// orientation as the head and the right eye DECLARED turned by the full cant it was never rendered with - the double image.
//
// The fix is what Pimax's "Parallel Projection" does inside the runtime, done here instead: replace both located view
// orientations with their bisector (the head), and re-express each eye's FOV in that frame - the smallest frustum, in the
// head frame, that contains the eye's own frustum. The game renders at the bisector, it is declared at the bisector, and
// the FOV it renders contains the display: declared == rendered, no doubled image, no missing edge. The compositor
// reprojects onto the turned displays (Valve's FOV deep-dive: the same mechanism lets parallel apps "just work" on mild cant).
//
// Below kMinCantDeg between the two views nothing is converted - the caller leaves the views exactly as located, which is
// the path every logged headset (Quest 3 VDXR, Index / Quest on SteamVR) already runs.
//
// Pure maths, no globals, no OpenXR types: cantview_test.cpp compiles this same header with cl.
// Conventions = OpenXR: right-handed, +Y up, -Z forward; Fov angles in radians, left / down negative (XrFovf).

#include <cmath>

namespace dlvr::cantview {

struct Quat { float x, y, z, w; };
struct Fov { float left, right, up, down; };   // XrFovf layout

constexpr float kMinCantDeg = 1.0f;            // views closer than this are treated as parallel (untouched)
constexpr float kMaxTan = 5.67128182f;         // tan(80 deg): the widest tangent a converted frustum may reach
constexpr float kMaxFakeDeg = 25.0f;           // diag_fake_view_cant_deg range
constexpr float kRadPerDeg = 0.017453292519943295f;
constexpr float kDegPerRad = 57.29577951308232f;

inline Quat Mul(const Quat& a, const Quat& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

inline Quat Conj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

inline Quat Normalize(const Quat& q)
{
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-12f)) { return { 0.0f, 0.0f, 0.0f, 1.0f }; }
    return { q.x / n, q.y / n, q.z / n, q.w / n };
}

// out = q * v * conj(q)
inline void Rotate(const Quat& q, const float v[3], float out[3])
{
    const float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    const float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    const float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

// The angle between two orientations, degrees (the relative rotation; atan2 form, not acos(dot), so it does not go blind
// near 0).
inline float AngleDeg(const Quat& a, const Quat& b)
{
    const Quat r = Mul(Conj(a), b);
    const float s = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return 2.0f * std::atan2(s, std::fabs(r.w)) * kDegPerRad;
}

// The orientation halfway between a and b (sign-aligned mean, normalised) - exact for two rotations about a common axis.
inline Quat Bisector(const Quat& a, const Quat& b)
{
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float k = (d < 0.0f) ? -1.0f : 1.0f;
    return Normalize({ a.x + k * b.x, a.y + k * b.y, a.z + k * b.z, a.w + k * b.w });
}

// A frustum given in frame E, re-expressed in frame H: rel = conj(qH) * qE (it takes E-frame vectors into H). Returns the
// smallest H-frame frustum that contains it. The four corner rays carry it: a rectangle on E's image plane maps to a
// convex quadrilateral on H's (a projective map keeps lines straight while every point stays in front), so the corner
// tangents bound it exactly. A corner turned to 90 deg or past, or a tangent past kMaxTan, is clamped to kMaxTan in its
// direction and *clamped is set - that part of the display cannot be covered by a parallel frustum and shows black.
inline Fov Reexpress(const Fov& f, const Quat& rel, bool* clamped)
{
    const float tx[2] = { std::tan(f.left), std::tan(f.right) };
    const float ty[2] = { std::tan(f.down), std::tan(f.up) };
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    bool cl = false;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float v[3] = { tx[i], ty[j], -1.0f };
            float o[3];
            Rotate(rel, v, o);
            float dn = -o[2];
            if (dn < 1e-3f) { dn = 1e-3f; cl = true; }
            const float px = o[0] / dn;
            const float py = o[1] / dn;
            if (px < minX) { minX = px; }
            if (px > maxX) { maxX = px; }
            if (py < minY) { minY = py; }
            if (py > maxY) { maxY = py; }
        }
    }
    auto clampTan = [&cl](float t) {
        if (t > kMaxTan) { cl = true; return kMaxTan; }
        if (t < -kMaxTan) { cl = true; return -kMaxTan; }
        return t;
    };
    minX = clampTan(minX); maxX = clampTan(maxX); minY = clampTan(minY); maxY = clampTan(maxY);
    if (clamped) { *clamped = *clamped || cl; }
    return { std::atan(minX), std::atan(maxX), std::atan(maxY), std::atan(minY) };
}

// diag_fake_view_cant_deg: turn view e outward by deg about its own +Y (view 0 = LEFT eye: +deg turns -Z toward -X, i.e.
// left = outward; view 1: -deg) and re-express its FOV in the turned frame - what a runtime reporting a canted display of
// the same physical coverage would hand over. Position untouched.
inline void FakeCant(int e, float deg, Quat& q, Fov& f, bool* clamped)
{
    const float half = ((e == 0) ? 1.0f : -1.0f) * deg * kRadPerDeg * 0.5f;
    const Quat turn{ 0.0f, std::sin(half), 0.0f, std::cos(half) };
    q = Normalize(Mul(q, turn));
    f = Reexpress(f, Conj(turn), clamped);   // old frame -> turned frame = conj(turn)
}

struct Result {
    bool converted = false;   // both orientations replaced by the bisector, both FOVs re-expressed
    bool clamped = false;     // a converted FOV hit kMaxTan (part of the display not covered)
    float angleDeg = 0.0f;    // the angle between the two views as handed in
    Quat rel[2]{ { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };   // conj(head) * view_e, valid when converted
};

// The conversion. q / f are both views (0 = left, 1 = right), changed in place only when the views are >= kMinCantDeg
// apart.
inline Result ToParallel(Quat q[2], Fov f[2])
{
    Result r;
    r.angleDeg = AngleDeg(q[0], q[1]);
    if (!(r.angleDeg >= kMinCantDeg)) { return r; }
    const Quat h = Bisector(q[0], q[1]);
    for (int e = 0; e < 2; ++e) {
        r.rel[e] = Mul(Conj(h), q[e]);
        f[e] = Reexpress(f[e], r.rel[e], &r.clamped);
        q[e] = h;
    }
    r.converted = true;
    return r;
}

}  // namespace dlvr::cantview
