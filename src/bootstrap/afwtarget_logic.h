#pragma once
// AFW REBUILD TARGET on an off-centre frustum (Jay 2026-11-24: "Start on AFW at 0"; trace docs/traces/20261124_1700_afw_offcentre_stage1.md;
// plan docs/PLAN-OFFCENTRE-ALL-RUNTIMES-2026-11-24.md rows 3, 4, 6).
//
// A view-projection matrix here is row-major, clip_i = row_i . (p, 1); row 2 = (0, 0, 0, near) (reversed-Z infinite), row 3 = the view's
// forward axis f (w = depth along it). Rows 0 / 1 split into a part perpendicular to f (the frustum's SCALE times view-right / view-up)
// and a part along f (the frustum's OFF-CENTRE, plus the jitter when it is not stripped):  row0 = ax + cx f,  row1 = ay + cy f.
//
// The rebuild draws the HELD eye at the FRESH camera moved `sep` along view-right. Until now it used the FRESH eye's frustum for that -
// right only when both eyes share one frustum (symmetric_fov 2). At symmetric_fov 0 the eyes' frustums are mirror images, so the held
// eye was drawn ~15 deg off its own image. HeldTarget keeps the fresh camera (rows' directions and position) and puts the HELD eye's own
// scale and off-centre (read off its own matrix mH, rotation-free scalars) on it.
//
// Pure maths, no globals: tests/afwtarget_logic_test.cpp compiles this header with cl.

#include <cmath>

namespace dlvr {
namespace afwtarget {

// The frustum part of a matrix's rows 0 / 1, relative to its own row 3.
struct Split {
    double f[3];      // row 3's unit direction (view-forward)
    double wl;        // |row 3 xyz|
    double ax[3], ay[3];
    double a, b;      // |ax|, |ay| - the frustum's horizontal / vertical scale
    double cx, cy;    // the off-centre along f
};

inline bool SplitRows(const float m[16], Split* s)
{
    const double r3[3] = { m[12], m[13], m[14] };
    s->wl = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
    if (!(s->wl > 1e-9)) { return false; }
    for (int i = 0; i < 3; ++i) { s->f[i] = r3[i] / s->wl; }
    const double r0[3] = { m[0], m[1], m[2] }, r1[3] = { m[4], m[5], m[6] };
    s->cx = r0[0] * s->f[0] + r0[1] * s->f[1] + r0[2] * s->f[2];
    s->cy = r1[0] * s->f[0] + r1[1] * s->f[1] + r1[2] * s->f[2];
    for (int i = 0; i < 3; ++i) { s->ax[i] = r0[i] - s->cx * s->f[i]; s->ay[i] = r1[i] - s->cy * s->f[i]; }
    s->a = std::sqrt(s->ax[0] * s->ax[0] + s->ax[1] * s->ax[1] + s->ax[2] * s->ax[2]);
    s->b = std::sqrt(s->ay[0] * s->ay[0] + s->ay[1] * s->ay[1] + s->ay[2] * s->ay[2]);
    return s->a > 1e-9 && s->b > 1e-9;
}

// The two frustums are the same centred one (to float noise): the old target is right, and is used unchanged.
constexpr double kSameTol = 1e-5;

// The target the held eye is drawn with: rows 0, 1, 3 (xyz) in RT, their w in tT (Carry's form). heldIsRight 1 = the held eye is to the
// RIGHT of the fresh one (the camera moves +sep along view-right). *general = false: both frustums centred and equal - RT / tT are the
// OLD arithmetic (the fresh matrix, row 0's w moved by -sgn*sep*a), bit for bit. false return = a degenerate matrix (no target).
inline bool HeldTarget(const float mF[16], const float mH[16], int heldIsRight, double sep, double RT[9], double tT[3], bool* general)
{
    *general = false;
    // ---- the old target, verbatim (AfwRebuildRun before 2026-11-24) ----
    RT[0] = mF[0]; RT[1] = mF[1]; RT[2] = mF[2]; RT[3] = mF[4]; RT[4] = mF[5]; RT[5] = mF[6]; RT[6] = mF[12]; RT[7] = mF[13]; RT[8] = mF[14];
    tT[0] = mF[3]; tT[1] = mF[7]; tT[2] = mF[15];
    const double wl = std::sqrt(RT[6] * RT[6] + RT[7] * RT[7] + RT[8] * RT[8]);
    if (!(wl > 1e-9)) { return false; }
    const double cx = (RT[0] * RT[6] + RT[1] * RT[7] + RT[2] * RT[8]) / wl;
    const double ax[3] = { RT[0] - cx * RT[6] / wl, RT[1] - cx * RT[7] / wl, RT[2] - cx * RT[8] / wl };
    const double a = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    const double sgn = heldIsRight ? 1.0 : -1.0;
    Split F{}, H{};
    if (!mH || !SplitRows(mF, &F) || !SplitRows(mH, &H) ||
        (std::fabs(F.cx) < kSameTol && std::fabs(H.cx) < kSameTol && std::fabs(F.cy) < kSameTol && std::fabs(H.cy) < kSameTol &&
         std::fabs(H.a - F.a) <= kSameTol * F.a && std::fabs(H.b - F.b) <= kSameTol * F.b)) {
        tT[0] -= sgn * sep * a;
        return true;
    }
    // ---- the held eye's own frustum at the fresh camera moved sgn * sep along r = ax / a ----
    *general = true;
    const double kx = H.a / F.a, ky = H.b / F.b;
    const double r[3] = { F.ax[0] / F.a, F.ax[1] / F.a, F.ax[2] / F.a };
    const double fdotC = -static_cast<double>(mF[15]) / F.wl;   // f . C_F   (row 3's w = -|row 3| f . C_F)
    const double axC = -static_cast<double>(mF[3]) - F.cx * fdotC;   // ax . C_F (row 0's w = -(ax + cx f) . C_F)
    const double ayC = -static_cast<double>(mF[7]) - F.cy * fdotC;   // ay . C_F
    const double ayr = F.ay[0] * r[0] + F.ay[1] * r[1] + F.ay[2] * r[2];   // 0 for an unskewed projection; kept, it is free
    for (int i = 0; i < 3; ++i) {
        RT[i] = kx * F.ax[i] + H.cx * F.f[i];
        RT[3 + i] = ky * F.ay[i] + H.cy * F.f[i];
    }
    // w_i = -row_i . C_T, C_T = C_F + sgn * sep * r (r is perpendicular to f, so row 3's w does not change)
    tT[0] = -(kx * axC + H.cx * fdotC + kx * sgn * sep * F.a);
    tT[1] = -(ky * ayC + H.cy * fdotC + ky * sgn * sep * ayr);
    return true;
}

// The inverse of a 3x4 carry [G | g] (target = G src + g) -> [Gi | -Gi g]. false = singular.
inline bool InvertCarry(const float g[3][4], float out[3][4])
{
    const double a[9] = { g[0][0], g[0][1], g[0][2], g[1][0], g[1][1], g[1][2], g[2][0], g[2][1], g[2][2] };
    const double c0 = a[4] * a[8] - a[5] * a[7], c1 = a[5] * a[6] - a[3] * a[8], c2 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * c0 + a[1] * c1 + a[2] * c2;
    if (!(std::fabs(det) > 1e-12)) { return false; }
    const double o[9] = { c0 / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
                          c1 / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                          c2 / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out[r][c] = static_cast<float>(o[r * 3 + c]); }
        out[r][3] = static_cast<float>(-(o[r * 3] * g[0][3] + o[r * 3 + 1] * g[1][3] + o[r * 3 + 2] * g[2][3]));
    }
    return true;
}

// The OLD stale-test carry ("identity minus the offset"), as rows: exact for every point when the target and the fresh view share a frustum.
inline void IdentityMinusOffset(const float g[3][4], float out[3][4])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { out[r][c] = r == c ? 1.0f : 0.0f; }
        out[r][3] = -g[r][3];
    }
}

}  // namespace afwtarget
}  // namespace dlvr
