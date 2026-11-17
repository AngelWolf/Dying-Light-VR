// Walk root + spine-top yaw hold - the pure math (trace docs/traces/20261117_1730_root_yaw_hold.md, spec
// docs/superpowers/specs/2026-11-17-walk-root-yaw-hold-design.md). Matrices are the engine's mtx34 as the rig reads them: 12 floats by
// ROWS, rotation R[r][c] = m[4r + c], translation m[3] / m[7] / m[11]; parent x local (the rig's Mul34). World / object up = +Y.
// A HEADING is atan2(x, z) of one rotation column projected on XZ - the same column for a bone every frame (the most horizontal one,
// chosen at the latch), so pitch / roll of a few degrees do not change it.
#pragma once
#include <cmath>

namespace dlvr {
namespace rootyaw {

constexpr float kPi = 3.14159265f;

inline float WrapPi(float a)
{
    while (a > kPi)  { a -= 2.0f * kPi; }
    while (a < -kPi) { a += 2.0f * kPi; }
    return a;
}

// The rotation column (0..2) closest to the horizontal plane (smallest |y| over its length); -1 if every column is degenerate.
inline int MostHorizontalCol(const float* m12)
{
    int best = -1;
    float bestY = 2.0f;
    for (int c = 0; c < 3; ++c) {
        const float x = m12[c], y = m12[4 + c], z = m12[8 + c];
        const float len = std::sqrt(x * x + y * y + z * z);
        if (!(len > 1e-4f)) { continue; }
        const float ay = std::fabs(y) / len;
        if (ay < bestY) { bestY = ay; best = c; }
    }
    return best;
}

// Heading (radians) of rotation column `col` on XZ; false when the column has no horizontal length.
inline bool HeadingOfCol(const float* m12, int col, float* out)
{
    if (col < 0 || col > 2) { return false; }
    const float x = m12[col], z = m12[8 + col];
    if (!(x * x + z * z > 1e-8f)) { return false; }
    *out = std::atan2(x, z);
    return true;
}

// Pre-multiplies the ROTATION part by Ry(a) (about the parent frame's +Y; translation untouched). For any column v:
// heading(Ry(a) v) = heading(v) + a, its y unchanged - so pitch / roll are kept and only the yaw moves.
inline void PreRotY(float a, float* m12)
{
    const float c = std::cos(a), s = std::sin(a);
    for (int col = 0; col < 3; ++col) {
        const float r0 = m12[col], r2 = m12[8 + col];
        m12[col]     = c * r0 + s * r2;
        m12[8 + col] = -s * r0 + c * r2;
    }
}

// 3x3 inverse of the rotation part of m12 (general - the parent may carry scale, INV-11). False if singular.
inline bool Inverse3(const float* m12, float* inv9)
{
    const float a = m12[0], b = m12[1], c = m12[2], d = m12[4], e = m12[5], f = m12[6], g = m12[8], h = m12[9], i = m12[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-8f)) { return false; }
    const float k = 1.0f / det;
    inv9[0] = A * k; inv9[1] = -(b * i - c * h) * k; inv9[2] = (b * f - c * e) * k;
    inv9[3] = B * k; inv9[4] = (a * i - c * g) * k;  inv9[5] = -(a * f - c * d) * k;
    inv9[6] = C * k; inv9[7] = -(a * h - b * g) * k; inv9[8] = (a * e - b * d) * k;
    return true;
}

// local rotation = inverse(parent world rotation) x child world rotation, written into local12's rotation part only (its translation
// is left as the caller had it). False if the parent rotation is singular.
inline bool LocalRotFromWorld(const float* parentW12, const float* childW12, float* local12)
{
    float inv[9];
    if (!Inverse3(parentW12, inv)) { return false; }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            local12[4 * r + c] = inv[3 * r] * childW12[c] + inv[3 * r + 1] * childW12[4 + c] + inv[3 * r + 2] * childW12[8 + c];
        }
    }
    return true;
}

// o = a x b (3x4 affine by rows - the rig's Mul34; parent x local). o must not alias a or b.
inline void ComposeRT(const float* a, const float* b, float* o)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            o[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + (c == 3 ? a[4 * r + 3] : 0.0f);
        }
    }
}

}  // namespace rootyaw
}  // namespace dlvr
