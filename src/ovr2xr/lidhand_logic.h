// lidhand_logic.h - the hand-open lids' decisions and maths, no game access (plan docs/superpowers/plans/2026-11-18-hand-open-lids-
// phase2.md Task 2; spec docs/superpowers/specs/2026-11-18-hand-open-lids-design.md §4.3-§4.5). Unit test: tests/lidhand_logic_test.cpp.
// Matrices: 3x4 row-major float[12], translation in column 3 (the engine's mtx34, as handcarry / buggywheel). Angles in degrees.
// The lid turns about ITS OWN hinge as the game's clip turns it: local = R(axis, deg) . restRotation, translation unchanged
// (RESEARCH-NATIVE-LOOTING §13.1 - every hinge's translation drift is 0.0 mm, axis spread <= 0.02 deg).
#pragma once

#include <cmath>

namespace dlvr {
namespace lidhand {

constexpr double kPi = 3.14159265358979323846;

inline void RotAxisDeg(const float axis[3], double deg, double R[9])
{
    const double n = std::sqrt(static_cast<double>(axis[0]) * axis[0] + static_cast<double>(axis[1]) * axis[1] +
                               static_cast<double>(axis[2]) * axis[2]);
    const double x = n > 0 ? axis[0] / n : 0, y = n > 0 ? axis[1] / n : 0, z = n > 0 ? axis[2] / n : 0;
    const double a = deg * kPi / 180.0, c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    R[0] = t * x * x + c;     R[1] = t * x * y - s * z; R[2] = t * x * z + s * y;
    R[3] = t * x * y + s * z; R[4] = t * y * y + c;     R[5] = t * y * z - s * x;
    R[6] = t * x * z - s * y; R[7] = t * y * z + s * x; R[8] = t * z * z + c;
}

// The lid's local for an angle: rest rotated by R(axisParent, deg) (parent frame), rest translation kept.
inline void LidLocal(const float rest[12], const float axisParent[3], float deg, float out[12])
{
    double R[9];
    RotAxisDeg(axisParent, deg, R);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = static_cast<float>(R[r * 3 + 0] * rest[0 * 4 + c] + R[r * 3 + 1] * rest[1 * 4 + c] + R[r * 3 + 2] * rest[2 * 4 + c]);
        }
        out[r * 4 + 3] = rest[r * 4 + 3];
    }
}

// The signed angle of `cur` from `rest` about `axisParent` (cur.rest^T read as a rotation about that axis). For a local written by
// LidLocal (or the game's own clip, which turns about the same fixed axis) it returns that angle.
inline float LidAngleFromLocal(const float rest[12], const float cur[12], const float axisParent[3])
{
    double d[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            d[r * 3 + c] = static_cast<double>(cur[r * 4 + 0]) * rest[c * 4 + 0] + static_cast<double>(cur[r * 4 + 1]) * rest[c * 4 + 1] +
                           static_cast<double>(cur[r * 4 + 2]) * rest[c * 4 + 2];
        }
    }
    const double n = std::sqrt(static_cast<double>(axisParent[0]) * axisParent[0] + static_cast<double>(axisParent[1]) * axisParent[1] +
                               static_cast<double>(axisParent[2]) * axisParent[2]);
    if (n <= 0) { return 0.0f; }
    const double sn = 0.5 * ((d[7] - d[5]) * axisParent[0] + (d[2] - d[6]) * axisParent[1] + (d[3] - d[1]) * axisParent[2]) / n;
    const double cs = 0.5 * (d[0] + d[4] + d[8] - 1.0);
    return static_cast<float>(std::atan2(sn, cs) * 180.0 / kPi);
}

// The hand's signed angle about the hinge (unit-normalised `axisW`, world) from where it grabbed (`grabW`) to now (`handW`), both
// taken relative to the pivot and projected on the plane normal to the axis. A hand on the hinge line gives 0 (never NaN).
inline float HandAngleDeg(const float pivotW[3], const float axisW[3], const float grabW[3], const float handW[3])
{
    double ax[3] = { axisW[0], axisW[1], axisW[2] };
    const double an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (an <= 0) { return 0.0f; }
    for (double& v : ax) { v /= an; }
    double a[3], b[3];
    for (int k = 0; k < 3; ++k) { a[k] = static_cast<double>(grabW[k]) - pivotW[k]; b[k] = static_cast<double>(handW[k]) - pivotW[k]; }
    const double da = a[0] * ax[0] + a[1] * ax[1] + a[2] * ax[2], db = b[0] * ax[0] + b[1] * ax[1] + b[2] * ax[2];
    for (int k = 0; k < 3; ++k) { a[k] -= da * ax[k]; b[k] -= db * ax[k]; }
    const double la = a[0] * a[0] + a[1] * a[1] + a[2] * a[2], lb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    if (la < 1e-8 || lb < 1e-8) { return 0.0f; }
    const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
    const double sn = cx * ax[0] + cy * ax[1] + cz * ax[2];
    const double cs = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return static_cast<float>(std::atan2(sn, cs) * 180.0 / kPi);
}

inline float ClampLid(float deg, float openDeg) { return deg < 0.0f ? 0.0f : (deg > openDeg ? openDeg : deg); }

// out = m . p (3x4 row-major, translation in column 3).
inline void MulPoint34(const float m[12], const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r) { out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]; }
}

// The lid's far edge in world (trace 20261118_2010): world = parentWorld . local . edge, edge = the row's two end points in the lid
// bone's OWN frame - `local` is the bone's current local (our write or the game's clip), so the edge is where the drawn lid edge is.
inline void EdgeWorld(const float parentW[12], const float local[12], const float edge[6], float a[3], float b[3])
{
    float t[3];
    MulPoint34(local, edge, t);     MulPoint34(parentW, t, a);
    MulPoint34(local, edge + 3, t); MulPoint34(parentW, t, b);
}

// ---- THE FREE LIFT (trace 20261122_1830, Jay: basket B "1" -> "Yes") ----
// out = a . b (3x4 rigid, row-major, translation in column 3).
inline void Compose34(const float a[12], const float b[12], float out[12])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + (c == 3 ? a[r * 4 + 3] : 0.0f);
        }
    }
}
// The inverse of a rigid 3x4 (rotation transposed, translation -R^T t). A bone / holder world is rigid (no scale).
inline void InvRigid34(const float m[12], float out[12])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out[r * 4 + c] = m[c * 4 + r]; }
        out[r * 4 + 3] = -(m[0 * 4 + r] * m[3] + m[1 * 4 + r] * m[7] + m[2 * 4 + r] * m[11]);
    }
}
// Distance from q to the circle (centre c, unit normal n, radius r): sqrt((in-plane distance from c - r)^2 + height^2). The free lift's
// grab zone is "within reach of anywhere on the round rim".
inline float RingDist(const float c[3], const float n[3], float r, const float q[3])
{
    const double d[3] = { static_cast<double>(q[0]) - c[0], static_cast<double>(q[1]) - c[1], static_cast<double>(q[2]) - c[2] };
    const double h = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    const double rho = std::sqrt(std::fmax(0.0, d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - h * h));
    return static_cast<float>(std::sqrt((rho - r) * (rho - r) + h * h));
}
// The let-go: the game's own open fires when the cover's rim centre is more than this far from where it rests (my call, Jay approved
// the design); closer = it goes back on, still closed.
constexpr float kFreeLiftOpenM = 0.20f;
inline bool FreeLiftOpens(const float restC[3], const float nowC[3])
{
    const double d[3] = { static_cast<double>(nowC[0]) - restC[0], static_cast<double>(nowC[1]) - restC[1], static_cast<double>(nowC[2]) - restC[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > kFreeLiftOpenM;
}

// Distance from q to the segment a-b (a == b -> the distance to a).
inline float SegDist(const float a[3], const float b[3], const float q[3])
{
    const double d[3] = { static_cast<double>(b[0]) - a[0], static_cast<double>(b[1]) - a[1], static_cast<double>(b[2]) - a[2] };
    const double w[3] = { static_cast<double>(q[0]) - a[0], static_cast<double>(q[1]) - a[1], static_cast<double>(q[2]) - a[2] };
    const double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    double t = dd > 1e-12 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd : 0.0;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double e[3] = { w[0] - t * d[0], w[1] - t * d[1], w[2] - t * d[2] };
    return static_cast<float>(std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
}

// Distance from q to the infinite line through p along axis (axis need not be unit; a zero axis -> the distance to p).
inline float LineDist(const float p[3], const float axis[3], const float q[3])
{
    const double w[3] = { static_cast<double>(q[0]) - p[0], static_cast<double>(q[1]) - p[1], static_cast<double>(q[2]) - p[2] };
    const double an = static_cast<double>(axis[0]) * axis[0] + static_cast<double>(axis[1]) * axis[1] + static_cast<double>(axis[2]) * axis[2];
    if (an <= 1e-12) { return SegDist(p, p, q); }
    const double t = (w[0] * axis[0] + w[1] * axis[1] + w[2] * axis[2]) / an;
    const double e[3] = { w[0] - t * axis[0], w[1] - t * axis[1], w[2] - t * axis[2] };
    return static_cast<float>(std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
}

// The door tie-break (trace 20261121_1549): how far (metres) the hand is PAST this door's own free edge, measured along the door's
// hinge -> edge direction (perpendicular to the hinge axis); 0 = the hand is over the leaf (between the hinge line and the edge) or
// behind the hinge. Two doors whose edges meet at a seam: the hand over door A reads 0 for A and > 0 for B.
inline float LeafOvershoot(const float pivotW[3], const float axisW[3], const float edgeMidW[3], const float handW[3])
{
    double ax[3] = { axisW[0], axisW[1], axisW[2] };
    const double an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (an <= 1e-12) { return 0.0f; }
    for (double& v : ax) { v /= an; }
    double d[3], h[3];
    for (int k = 0; k < 3; ++k) { d[k] = static_cast<double>(edgeMidW[k]) - pivotW[k]; h[k] = static_cast<double>(handW[k]) - pivotW[k]; }
    const double dd = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
    for (int k = 0; k < 3; ++k) { d[k] -= dd * ax[k]; }
    const double R = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (R <= 1e-9) { return 0.0f; }
    const double along = (h[0] * d[0] + h[1] * d[1] + h[2] * d[2]) / R;   // metres from the hinge line toward the edge
    return along > R ? static_cast<float>(along - R) : 0.0f;
}

// True only on the frame the lid goes from below frac x open to at / above it.
inline bool CrossesOpenPoint(float prevDeg, float nowDeg, float openDeg, float frac)
{
    const float p = frac * openDeg;
    return prevDeg < p && nowDeg >= p;
}

// Index of the smallest distance within `reach` (a distance < 0 = no reading, skipped), else -1.
inline int NearestWithin(const float dist[], int n, float reach)
{
    int best = -1;
    for (int i = 0; i < n; ++i) {
        if (dist[i] < 0.0f || dist[i] > reach) { continue; }
        if (best < 0 || dist[i] < dist[best]) { best = i; }
    }
    return best;
}

// The per-item pose row (trace 20261120_2057): `mesh` as GetMeshName reports it (folder / case / ".msh" vary) -> its lower-case base name
// -> the index of the EXACT key in names[0..n), else -1 (no row = today's placement). Keys are lower case without folder or extension.
inline int PoseFind(const char* const* names, int n, const char* mesh)
{
    if (!names || !mesh || !*mesh) { return -1; }
    const char* s = mesh;
    for (const char* q = mesh; *q; ++q) { if (*q == '/' || *q == '\\') { s = q + 1; } }
    char base[96] = {};
    int len = 0;
    for (; s[len] && len < 95; ++len) { const char c = s[len]; base[len] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
    base[len] = 0;
    for (int i = len - 1; i >= 0; --i) {
        if (base[i] == '.') { base[i] = 0; break; }
    }
    if (!base[0]) { return -1; }
    for (int i = 0; i < n; ++i) {
        const char* a = names[i];
        const char* b = base;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) { return i; }
    }
    return -1;
}

// ---- CLIP-FOLLOW rows (plan 2026-11-21-clip-follow-containers Task 2; trace 20261121_2145): a container whose open is NOT one fixed
// hinge (the van's rear doors, basket B's lift-off cover) is written at the game's OWN clip frame nearest the hand. `frames` = the bone's
// local (parent frame, 3x4) per table frame; t = a FRAME POSITION (float frames, 0 .. n-1).

// Rotation 3x4 -> unit quaternion (w, x, y, z), Shepperd's method.
inline void QuatFrom34(const float m[12], double q[4])
{
    const double m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[4], m11 = m[5], m12 = m[6], m20 = m[8], m21 = m[9], m22 = m[10];
    const double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        q[0] = 0.25 * s; q[1] = (m21 - m12) / s; q[2] = (m02 - m20) / s; q[3] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q[0] = (m21 - m12) / s; q[1] = 0.25 * s; q[2] = (m01 + m10) / s; q[3] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q[0] = (m02 - m20) / s; q[1] = (m01 + m10) / s; q[2] = 0.25 * s; q[3] = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q[0] = (m10 - m01) / s; q[1] = (m02 + m20) / s; q[2] = (m12 + m21) / s; q[3] = 0.25 * s;
    }
}

// L(t): the two frames around t, rotation slerped (shortest arc), translation lerped; t clamped to [0, n-1]. n < 1 -> identity.
inline void ClipLocalAt(const float (*frames)[12], int n, float t, float out[12])
{
    if (!frames || n < 1) {
        const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        for (int i = 0; i < 12; ++i) { out[i] = id[i]; }
        return;
    }
    if (t <= 0.0f || n == 1) { for (int i = 0; i < 12; ++i) { out[i] = frames[0][i]; } return; }
    if (t >= static_cast<float>(n - 1)) { for (int i = 0; i < 12; ++i) { out[i] = frames[n - 1][i]; } return; }
    const int f = static_cast<int>(t);
    const double u = static_cast<double>(t) - f;
    const float* a = frames[f];
    const float* b = frames[f + 1];
    double qa[4], qb[4];
    QuatFrom34(a, qa);
    QuatFrom34(b, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0) { for (double& x : qb) { x = -x; } d = -d; }
    double wa, wb;
    if (d > 0.9995) {
        wa = 1.0 - u; wb = u;
    } else {
        const double th = std::acos(d), s = std::sin(th);
        wa = std::sin((1.0 - u) * th) / s; wb = std::sin(u * th) / s;
    }
    double q[4];
    double n2 = 0.0;
    for (int i = 0; i < 4; ++i) { q[i] = wa * qa[i] + wb * qb[i]; n2 += q[i] * q[i]; }
    const double nn = std::sqrt(n2);
    for (double& x : q) { x /= nn; }
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                          2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                          2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out[r * 4 + c] = static_cast<float>(R[r * 3 + c]); }
        out[r * 4 + 3] = static_cast<float>(a[r * 4 + 3] * (1.0 - u) + b[r * 4 + 3] * u);
    }
}

// The t minimizing |parentW . L(t) . g - hand| + stickMPerFrame . |t - tNow| over [tNow - window, tNow + window] within [0, n-1],
// sampled every 0.25 frame. The window keeps the cover on ITS stretch of a turning path and limits a hand that tears away to `window`
// frames per call; the stickiness (trace 20261121_2225, the basket cover: 0 = the plain nearest frame) makes a far frame win only when
// it is clearly nearer - a near-tie between two stretches of a looping path no longer flips per tick.
inline float ClipNearestT(const float (*frames)[12], int n, const float parentW[12], const float g[3], const float hand[3], float tNow,
                          float window, float stickMPerFrame = 0.0f)
{
    if (!frames || n < 1) { return 0.0f; }
    const float hi = static_cast<float>(n - 1);
    float lo = tNow - window, up = tNow + window;
    if (lo < 0.0f) { lo = 0.0f; }
    if (up > hi) { up = hi; }
    float best = tNow < 0.0f ? 0.0f : (tNow > hi ? hi : tNow), bestD = -1.0f;
    for (float t = lo; t <= up + 1e-4f; t += 0.25f) {
        const float tt = t > hi ? hi : t;
        float L[12], W[12], p[3];
        ClipLocalAt(frames, n, tt, L);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                W[r * 4 + c] = parentW[r * 4 + 0] * L[0 * 4 + c] + parentW[r * 4 + 1] * L[1 * 4 + c] + parentW[r * 4 + 2] * L[2 * 4 + c] +
                               (c == 3 ? parentW[r * 4 + 3] : 0.0f);
            }
        }
        MulPoint34(W, g, p);
        const float dx = p[0] - hand[0], dy = p[1] - hand[1], dz = p[2] - hand[2];
        // a true distance (metres) when a stickiness is given, so the per-frame cost adds in metres; squared otherwise (as before)
        const float d = stickMPerFrame > 0.0f ? std::sqrt(dx * dx + dy * dy + dz * dz) + stickMPerFrame * std::fabs(tt - tNow)
                                              : dx * dx + dy * dy + dz * dz;
        if (bestD < 0.0f || d < bestD) { bestD = d; best = tt; }
    }
    return best;
}

// The written t eased toward the chosen t (trace 20261121_2225): t + (target - t) . (1 - e^(-dt / tau)); tau <= 0 = no easing.
inline float ClipEase(float tNow, float target, float dtS, float tauS)
{
    if (!(tauS > 0.0f)) { return target; }
    if (!(dtS > 0.0f)) { return tNow; }
    return tNow + (target - tNow) * (1.0f - std::exp(-dtS / tauS));
}

// The point of segment a-b nearest q (the cover's rim point under the hand, trace 20261121_2225).
inline void SegClosest(const float a[3], const float b[3], const float q[3], float out[3])
{
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float u = l2 > 1e-12f ? ((q[0] - a[0]) * ab[0] + (q[1] - a[1]) * ab[1] + (q[2] - a[2]) * ab[2]) / l2 : 0.0f;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    for (int k = 0; k < 3; ++k) { out[k] = a[k] + u * ab[k]; }
}

// Distance between two locals: rotation angle (radians) + translation (metres x 100 - a centimetre ~ a 0.6 deg turn).
inline double LocalDist(const float a[12], const float b[12])
{
    double tr = 0.0;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) { tr += static_cast<double>(a[r * 4 + k]) * b[r * 4 + k]; }
    }
    double c = (tr - 1.0) / 2.0;
    c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
    const double dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    return std::acos(c) + 100.0 * std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The t whose local is nearest `cur`: where the game's clip / our last write left the bone. COST (final review, trace 20261121_2145 - it
// runs per tick in the item-reach check): one compare per table frame, then 0.25-frame interpolations within +-1 frame of the nearest
// (<= 9). evals (optional) = interpolations done.
inline float ClipTFromLocal(const float (*frames)[12], int n, const float cur[12], int* evals = nullptr)
{
    if (evals) { *evals = 0; }
    if (!frames || n < 1) { return 0.0f; }
    int bf = 0;
    double bd = LocalDist(frames[0], cur);
    for (int f = 1; f < n; ++f) {
        const double d = LocalDist(frames[f], cur);
        if (d < bd) { bd = d; bf = f; }
    }
    float best = static_cast<float>(bf);
    const float lo = bf > 0 ? static_cast<float>(bf - 1) : 0.0f;
    const float hi = bf < n - 1 ? static_cast<float>(bf + 1) : static_cast<float>(n - 1);
    for (float t = lo; t <= hi + 1e-4f; t += 0.25f) {
        if (t == static_cast<float>(static_cast<int>(t))) { continue; }   // whole frames were compared above
        float L[12];
        ClipLocalAt(frames, n, t, L);
        if (evals) { ++*evals; }
        const double d = LocalDist(L, cur);
        if (d < bd) { bd = d; best = t; }
    }
    return best;
}

enum class ItemRelease { Stow, GiveBack, Drop };

// What a carried item is (INV-758): a lid container's ContainerItem, a placed ItemSpawner (it IS the item), or a DroppedItem.
enum class ItemKind { Chest, Spawner, Dropped };
// Plan 2026-11-20-world-items-by-hand (Jay: "Just do the physics then instead", "chest items drop with physics too"): shoulder + action 1
// = Stow; the inventory refusing (23) or an unread action (-1) = GiveBack for an item with a home (Chest / Spawner), Drop for a
// DroppedItem (no home - it falls where it is); any other release = Drop (the game's take + the game's drop at the hand).
inline ItemRelease OnItemRelease(bool inShoulderZone, int takeAction, ItemKind kind)
{
    if (takeAction == 1) { return inShoulderZone ? ItemRelease::Stow : ItemRelease::Drop; }
    return kind == ItemKind::Dropped ? ItemRelease::Drop : ItemRelease::GiveBack;
}

// Which inventory entry a take added or raised, from before / after snapshots (entry pointers + stack counts at entry +64, INV-755):
// an entry new in `after` = its whole stack (a count of 0 - a non-stacking item - = 1); the same entry with a higher count = the rise.
// A count that fell is not a take. Returns the index into `after`, -1 = none (ammo / cash / collectables are taken with no entry).
inline int InvDelta(const void* const* bp, const int* bc, int bn, const void* const* ap, const int* ac, int an, int* count)
{
    for (int i = 0; i < an; ++i) {
        int was = -1;
        for (int j = 0; j < bn && was < 0; ++j) { if (bp[j] == ap[i]) { was = bc[j]; } }
        if (was < 0) { *count = ac[i] > 0 ? ac[i] : 1; return i; }
        if (ac[i] > was) { *count = ac[i] - was; return i; }
    }
    return -1;
}

// The let-go drop's entry BY ITEM IDENTITY (trace 20261121_1627; RUN CL1b dropped the grappling hook - InvDelta's first changed entry):
// as InvDelta, but only an `after` entry whose ItemID (`aid`, the game's own stack-match key: *vfunc+432 of the desc object, +CD4D60)
// equals `want`. kNoId (an id that did not read) never matches. -1 = no entry of this item was added or raised.
constexpr int kNoId = static_cast<int>(0x80000000u);
inline int InvDeltaMatch(const void* const* bp, const int* bc, int bn, const void* const* ap, const int* ac, const int* aid, int an,
                         int want, int* count)
{
    if (want == kNoId) { return -1; }
    for (int i = 0; i < an; ++i) {
        if (aid[i] != want) { continue; }
        int was = -1;
        for (int j = 0; j < bn && was < 0; ++j) { if (bp[j] == ap[i]) { was = bc[j]; } }
        if (was < 0) { *count = ac[i] > 0 ? ac[i] : 1; return i; }
        if (ac[i] > was) { *count = ac[i] - was; return i; }
    }
    return -1;
}

}  // namespace lidhand
}  // namespace dlvr
