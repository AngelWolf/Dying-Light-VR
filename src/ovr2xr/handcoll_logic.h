// Live-enemy hand collision - the pure part (spec docs/superpowers/specs/2026-10-30-live-enemy-hand-collision-design.md, plan
// Task 2). A live enemy's body = its own ragdoll shapes (ragdoll_ai.phx Bones()), sized by OUR engine's rule and posed on its live
// bones each frame (INV-660; RESEARCH-ENEMY-GRAB §6 (b) / (b2)); the drawn hand stops where the shoulder->hand segment first enters
// one. No game reads here; handcoll.cpp feeds it. Tests: src/tests/handcoll_logic_test.cpp.
#pragma once

#include <cmath>

namespace dlvr {
namespace handcoll {

enum class ShapeKind { Sphere = 1, Capsule = 2 };

// In the bone frame. axis 0/1/2 = the capsule's long side; halfLen = its cylinder length / 2 (0 for a sphere).
struct LocalShape {
    ShapeKind kind;
    float c[3];
    int axis;
    float r;
    float halfLen;
};

// World space. A capsule = the segment a-b swept by r; a sphere has a == b.
struct WorldShape {
    ShapeKind kind;
    float a[3];
    float b[3];
    float r;
};

inline bool Finite(float v) { return std::isfinite(v) != 0; }

// The engine's step before the sizing (+3B0280 with +66F670, RESEARCH §6 (b)): the bone's extents min / max mapped by N^T * M, M =
// the bone's world 3x3 (mtx34 row-major), N = M with unit columns - i.e. each axis scaled by that column's length. False = a column
// of zero length or a non-finite input.
inline bool ScaleToBone(const float m34[12], const float mn[3], const float mx[3], float outMn[3], float outMx[3])
{
    float n[9];
    for (int col = 0; col < 3; ++col) {
        const float x = m34[0 * 4 + col], y = m34[1 * 4 + col], z = m34[2 * 4 + col];
        const float len = std::sqrt(x * x + y * y + z * z);
        if (!(len > 1e-6f) || !Finite(len)) { return false; }
        n[0 * 3 + col] = x / len; n[1 * 3 + col] = y / len; n[2 * 3 + col] = z / len;
    }
    for (int k = 0; k < 3; ++k) {
        if (!Finite(mn[k]) || !Finite(mx[k])) { return false; }
    }
    for (int i = 0; i < 3; ++i) {
        float lo = 0.0f, hi = 0.0f;
        for (int r = 0; r < 3; ++r) {
            const float aLo = m34[r * 4 + 0] * mn[0] + m34[r * 4 + 1] * mn[1] + m34[r * 4 + 2] * mn[2];
            const float aHi = m34[r * 4 + 0] * mx[0] + m34[r * 4 + 1] * mx[1] + m34[r * 4 + 2] * mx[2];
            lo += n[r * 3 + i] * aLo;
            hi += n[r * 3 + i] * aHi;
        }
        outMn[i] = lo;
        outMx[i] = hi;
    }
    return true;
}

// OUR engine's rule (+3B0280, RESEARCH §6 (b2), exact): sides d = (max - min) x scale + 0.01; m = max(d); capsule axis = X if m == dX,
// else Y if m == dY, else Z; r = half the larger of the other two; sphere r = m / 2; capsule L = max(m - r, 0) / scale + 0.08;
// centre = (min + max) / 2. False when scale <= 0 or an input is not finite.
inline bool BuildLocalShape(ShapeKind kind, const float extMin[3], const float extMax[3], float scale, LocalShape* out)
{
    if (!out || !(scale > 0.0f) || !Finite(scale)) { return false; }
    for (int k = 0; k < 3; ++k) {
        if (!Finite(extMin[k]) || !Finite(extMax[k])) { return false; }
    }
    float d[3];
    for (int k = 0; k < 3; ++k) { d[k] = (extMax[k] - extMin[k]) * scale + 0.01f; }
    float m = d[0] > d[1] ? d[0] : d[1];
    if (d[2] > m) { m = d[2]; }
    int axis = 2;
    float r = (d[0] > d[1] ? d[0] : d[1]) * 0.5f;
    if (m == d[0]) {
        axis = 0;
        r = (d[1] > d[2] ? d[1] : d[2]) * 0.5f;
    } else if (m == d[1]) {
        axis = 1;
        r = (d[0] > d[2] ? d[0] : d[2]) * 0.5f;
    }
    LocalShape s{};
    s.kind = kind;
    for (int k = 0; k < 3; ++k) { s.c[k] = (extMin[k] + extMax[k]) * 0.5f; }
    if (kind == ShapeKind::Sphere) {
        s.axis = 0;
        s.r = m * 0.5f;
        s.halfLen = 0.0f;
    } else {
        const float rest = m - r > 0.0f ? m - r : 0.0f;
        s.axis = axis;
        s.r = r;
        s.halfLen = (rest / scale + 0.08f) * 0.5f;
    }
    *out = s;
    return true;
}

// Pose a bone-frame shape with the bone's world matrix (mtx34 row-major: row r = [4r..4r+2], translation [4r+3]); the rotation is
// taken with unit columns (the engine's N), so a scaled bone moves the shape, never stretches it a second time.
inline void PoseShape(const LocalShape& s, const float boneWorld34[12], WorldShape* out)
{
    float n[9];
    for (int col = 0; col < 3; ++col) {
        const float x = boneWorld34[0 * 4 + col], y = boneWorld34[1 * 4 + col], z = boneWorld34[2 * 4 + col];
        const float len = std::sqrt(x * x + y * y + z * z);
        const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
        n[0 * 3 + col] = x * inv; n[1 * 3 + col] = y * inv; n[2 * 3 + col] = z * inv;
    }
    float pa[3] = { s.c[0], s.c[1], s.c[2] }, pb[3] = { s.c[0], s.c[1], s.c[2] };
    if (s.kind == ShapeKind::Capsule) {
        pa[s.axis] -= s.halfLen;
        pb[s.axis] += s.halfLen;
    }
    WorldShape w{};
    w.kind = s.kind;
    w.r = s.r;
    for (int r = 0; r < 3; ++r) {
        w.a[r] = n[r * 3 + 0] * pa[0] + n[r * 3 + 1] * pa[1] + n[r * 3 + 2] * pa[2] + boneWorld34[r * 4 + 3];
        w.b[r] = n[r * 3 + 0] * pb[0] + n[r * 3 + 1] * pb[1] + n[r * 3 + 2] * pb[2] + boneWorld34[r * 4 + 3];
    }
    *out = w;
}

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Distance from p to the segment a-b.
inline float DistToSegment(const float p[3], const float a[3], const float b[3])
{
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float l2 = Dot3(ab, ab);
    float t = l2 > 1e-12f ? Dot3(ap, ab) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float q[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
    return std::sqrt(Dot3(q, q));
}

// The entry distance of the ray o + u t (u unit) into the sphere (c, r); -1 = no entry ahead of o.
inline float RayEnterSphere(const float o[3], const float u[3], const float c[3], float r)
{
    const float oc[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
    const float b = Dot3(u, oc);
    const float cc = Dot3(oc, oc) - r * r;
    const float h = b * b - cc;
    if (h < 0.0f) { return -1.0f; }
    const float t = -b - std::sqrt(h);
    return t >= 0.0f ? t : -1.0f;
}

// The entry distance of the ray o + u t (u unit) into the capsule a-b r: the side cylinder (hits whose axis projection lies inside
// a-b) and both end spheres, the nearest ahead of o. -1 = none.
inline float RayEnterCapsule(const float o[3], const float u[3], const float a[3], const float b[3], float r)
{
    float best = -1.0f;
    const float ba[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float oa[3] = { o[0] - a[0], o[1] - a[1], o[2] - a[2] };
    const float baba = Dot3(ba, ba);
    if (baba > 1e-12f) {
        const float bard = Dot3(ba, u), baoa = Dot3(ba, oa), rdoa = Dot3(u, oa), oaoa = Dot3(oa, oa);
        const float qa = baba - bard * bard;
        const float qb = baba * rdoa - baoa * bard;
        const float qc = baba * oaoa - baoa * baoa - r * r * baba;
        const float h = qb * qb - qa * qc;
        if (qa > 1e-12f && h >= 0.0f) {
            const float t = (-qb - std::sqrt(h)) / qa;
            const float y = baoa + t * bard;
            if (t >= 0.0f && y > 0.0f && y < baba) { best = t; }
        }
    }
    const float ta = RayEnterSphere(o, u, a, r), tb = RayEnterSphere(o, u, b, r);
    if (ta >= 0.0f && (best < 0.0f || ta < best)) { best = ta; }
    if (tb >= 0.0f && (best < 0.0f || tb < best)) { best = tb; }
    return best;
}

// The stop: true + out = the point where the segment S->T first enters a shape, pulled back 1 mm toward S (direction kept, only the
// distance shortened); a shape that already contains S is ignored (a zombie pressed into the player never pulls the hand to the
// shoulder). False + out = T when no shape is entered before T. *hitOut = the shape's index (-1 = none) when non-null.
inline bool StopTarget(const float S[3], const float T[3], const WorldShape* shapes, int n, float out[3], int* hitOut)
{
    out[0] = T[0]; out[1] = T[1]; out[2] = T[2];
    if (hitOut) { *hitOut = -1; }
    if (!shapes || n <= 0) { return false; }
    const float d[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
    const float len = std::sqrt(Dot3(d, d));
    if (!(len > 1e-5f) || !Finite(len)) { return false; }
    const float u[3] = { d[0] / len, d[1] / len, d[2] / len };
    float best = -1.0f;
    int bestI = -1;
    for (int i = 0; i < n; ++i) {
        const WorldShape& s = shapes[i];
        if (!(s.r > 0.0f)) { continue; }
        if (DistToSegment(S, s.a, s.b) <= s.r) { continue; }   // S inside: ignored
        const float t = RayEnterCapsule(S, u, s.a, s.b, s.r);  // a sphere is a capsule with a == b
        if (t < 0.0f || t > len) { continue; }
        if (best < 0.0f || t < best) { best = t; bestI = i; }
    }
    if (bestI < 0) { return false; }
    float k = best - 0.001f;
    if (k < 0.0f) { k = 0.0f; }
    for (int c = 0; c < 3; ++c) { out[c] = S[c] + u[c] * k; }
    if (hitOut) { *hitOut = bestI; }
    return true;
}

// The reach (push_grab_reach_m, 2026-10-31 (6), Jay: "Reach a few cm short"): the shape whose SURFACE is nearest p, among shapes
// that do not contain S (as StopTarget). True when that gap is <= maxGap (maxGap > 0): *gapOut = the gap (0 = p on or inside),
// *hitOut = its index, surface = the surface point nearest p, normal = the outward unit normal there. False = none within maxGap
// (*hitOut = -1). *nearestOut (when non-null) = the smallest gap however far, -1 = no shape admitted - for the miss log.
inline bool NearestSurface(const float S[3], const float p[3], const WorldShape* shapes, int n, float maxGap, float* gapOut,
                           int* hitOut, float surface[3], float normal[3], float* nearestOut)
{
    if (hitOut) { *hitOut = -1; }
    if (nearestOut) { *nearestOut = -1.0f; }
    if (!shapes || n <= 0) { return false; }
    float best = -1.0f;
    int bestI = -1;
    float bestQ[3] = {};
    for (int i = 0; i < n; ++i) {
        const WorldShape& s = shapes[i];
        if (!(s.r > 0.0f)) { continue; }
        if (DistToSegment(S, s.a, s.b) <= s.r) { continue; }   // S inside: ignored
        const float ab[3] = { s.b[0] - s.a[0], s.b[1] - s.a[1], s.b[2] - s.a[2] };
        const float ap[3] = { p[0] - s.a[0], p[1] - s.a[1], p[2] - s.a[2] };
        const float l2 = Dot3(ab, ab);
        float t = l2 > 1e-12f ? Dot3(ap, ab) / l2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float q[3] = { s.a[0] + ab[0] * t, s.a[1] + ab[1] * t, s.a[2] + ab[2] * t };
        const float pq[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
        float gap = std::sqrt(Dot3(pq, pq)) - s.r;
        if (!Finite(gap)) { continue; }
        if (gap < 0.0f) { gap = 0.0f; }
        if (best < 0.0f || gap < best) {
            best = gap;
            bestI = i;
            bestQ[0] = q[0]; bestQ[1] = q[1]; bestQ[2] = q[2];
        }
    }
    if (nearestOut) { *nearestOut = best; }
    if (bestI < 0 || !(maxGap > 0.0f) || best > maxGap) { return false; }
    const WorldShape& s = shapes[bestI];
    const float d[3] = { p[0] - bestQ[0], p[1] - bestQ[1], p[2] - bestQ[2] };
    const float dl = std::sqrt(Dot3(d, d));
    for (int c = 0; c < 3; ++c) {
        normal[c] = dl > 1e-6f ? d[c] / dl : 0.0f;
        surface[c] = bestQ[c] + normal[c] * s.r;
    }
    if (gapOut) { *gapOut = best; }
    if (hitOut) { *hitOut = bestI; }
    return true;
}

// Which enemies are tracked (review F2): the NEAREST. trackedDist = the n tracked enemies' distances from the shoulders (< 0 = not
// readable - treated as the farthest); cap = the slots. True = admit a candidate at candDist: *replaceIdx = -1 for a free slot, else
// the farthest tracked index it replaces (only when the candidate is strictly nearer - an equal distance never churns).
inline bool Admit(const float* trackedDist, int n, int cap, float candDist, int* replaceIdx)
{
    *replaceIdx = -1;
    if (n < cap) { return true; }
    int far = -1;
    float farD = -1.0f;
    for (int i = 0; i < n; ++i) {
        const float d = trackedDist[i] < 0.0f ? 1.0e30f : trackedDist[i];
        if (far < 0 || d > farD) { far = i; farD = d; }
    }
    if (far < 0 || !(candDist < farD)) { return false; }
    *replaceIdx = far;
    return true;
}

// One buzz per touch: true exactly on a rising edge; re-arms only after kRearmS continuously out of contact, so tracking jitter at
// the surface (1-frame dropouts) never buzzes again.
class ContactEdge {
public:
    static constexpr double kRearmS = 0.15;
    bool Update(bool inContact, double nowS)
    {
        if (inContact) {
            lastInS_ = nowS;
            if (armed_) {
                armed_ = false;
                return true;
            }
            return false;
        }
        if (!armed_ && nowS - lastInS_ >= kRearmS) { armed_ = true; }
        return false;
    }
    void Reset() { armed_ = true; lastInS_ = -1.0e9; }

private:
    bool armed_ = true;
    double lastInS_ = -1.0e9;
};

}  // namespace handcoll
}  // namespace dlvr
