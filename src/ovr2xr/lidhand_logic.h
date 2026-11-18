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

enum class ItemRelease { Stow, GiveBack };
// Stow only in the shoulder zone AND with the item's take action 1 (23 = the inventory refuses it, RESEARCH §13.5).
inline ItemRelease OnItemRelease(bool inShoulderZone, int takeAction)
{
    return (inShoulderZone && takeAction == 1) ? ItemRelease::Stow : ItemRelease::GiveBack;
}

}  // namespace lidhand
}  // namespace dlvr
