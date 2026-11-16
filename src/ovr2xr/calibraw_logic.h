// Raw-pose calibration samples - build B (docs/superpowers/plans/2026-10-14-calibration-screen-build-b.md task 2).
// The in-level solve builds T = anchor + Ry(bodyYaw)(ctl - headRep) - wristBack (boneblend.cpp:10929-10933) with
// wristBack = Ry(bodyYaw) cq gripRead t_hh (:10647-10658, :10712-10732), and armfit's BodyFrame undoes the same bodyYaw -
// so the body-frame T is M((ctl - headRep) - cq grip t_hh), M: (x, y, z) -> (-z, y, x * sg). Measured: raw == solve within
// 0.066 mm on 8 calibrations (docs/RESEARCH-CALIB-EQUIV-2026-10-14.md). Pure: no OpenXR, no Windows, so
// src/src/tests/calibraw_logic_test.cpp compiles it alone.
#pragma once

#include <cmath>

namespace dlvr {
namespace calibraw {

// The AUTHORED handholder offsets, right / left (INV-161; the capture HDR's handHolderAuthoredLocal) - NOT the live hhL
// (the right one missed the logged wristBack by a constant 21.70 mm).
constexpr float kHhAuthored[2][3] = { { 0.09999989f, 3.8146975e-08f, -0.024999849f },
                                      { 0.096248291f, 0.0f, -0.023768237f } };
// The AUTHORED hand rest local (row-major, axes in the columns), right / left - the [BONEREST] "authored REST LOCAL basis" lines,
// identical in every archived session (77.0 deg = tilt 13.73 + roll +-75.12, INV-150). For the MAIN MENU calibration only, where no
// player model exists and boneblend's latch is empty (HandPostForSide). Logged to 4 decimals: < 0.02 mm at the handholder.
constexpr float kRestAuthored[2][9] = { {  0.9714f,  0.1429f,  0.1895f,
                                           0.1491f,  0.2539f, -0.9557f,
                                          -0.1847f,  0.9566f,  0.2253f },
                                        {  0.9714f, -0.1429f,  0.1895f,
                                          -0.1491f,  0.2539f,  0.9557f,
                                          -0.1847f, -0.9566f,  0.2253f } };
// Build B ruling B1: the stand-in shoulder for the pose CHECK only - Jay's in-game S, mean over 9 calibrations, body frame
// (fwd, up, out) from headRep. The fit never reads it.
constexpr float kStandinS[3] = { -0.1302f, -0.2487f, 0.1909f };
constexpr float kMinReach = 0.45f;   // armfit.cpp kMinReach

inline void QuatMat(const float q[4], float m[9])   // boneblend.cpp:10649-10654, (x, y, z, w)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

inline void Mul3(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }
}

inline void ToBody(const float v[3], int side, float o[3])   // M: tracking -> body (fwd -z, up y, out x * sg)
{
    const float sg = side ? -1.0f : 1.0f;
    o[0] = -v[2];
    o[1] = v[1];
    o[2] = v[0] * sg;
}

// Body-frame T from tracking-space values. side 0 = R, 1 = L.
// post = the hand's DRAWN turn on top of cq . grip (boneblend HandPostForSide: the authored rest x the hand-angle dials) - the solve's
// step-back is turned by the drawn hand (trace docs/traces/20261116_1126_wristback_drawn_basis.md), so this must be too. Identity =
// the previous result.
inline void RawT(const float ctl[3], const float headRep[3], const float q[4], const float grip[9], const float post[9], int side,
                 float outT[3])
{
    float cq[9], hc[9], hb[9];
    QuatMat(q, cq);
    Mul3(cq, grip, hc);
    Mul3(hc, post, hb);
    const float* t = kHhAuthored[side ? 1 : 0];
    const float wb[3] = { hb[0] * t[0] + hb[1] * t[1] + hb[2] * t[2], hb[3] * t[0] + hb[4] * t[1] + hb[5] * t[2],
                          hb[6] * t[0] + hb[7] * t[1] + hb[8] * t[2] };
    const float v[3] = { ctl[0] - headRep[0] - wb[0], ctl[1] - headRep[1] - wb[1], ctl[2] - headRep[2] - wb[2] };
    ToBody(v, side, outT);
}

// The hand basis in the body frame (M . cq . grip), row-major, columns = the hand's axes.
inline void RawHandBasis(const float q[4], const float grip[9], int side, float outB[9])
{
    float cq[9], hb[9];
    QuatMat(q, cq);
    Mul3(cq, grip, hb);
    const float sg = side ? -1.0f : 1.0f;
    for (int c = 0; c < 3; ++c) {
        outB[0 * 3 + c] = -hb[2 * 3 + c];
        outB[1 * 3 + c] = hb[1 * 3 + c];
        outB[2 * 3 + c] = hb[0 * 3 + c] * sg;
    }
}

// armfit.cpp MatchesPose (:118-132) against the stand-in: 1 in the pose, -1 wrong direction, -2 reach under 0.45 m.
inline int RawMatches(const float T[3], int pose)
{
    const float d[3] = { T[0] - kStandinS[0], T[1] - kStandinS[1], T[2] - kStandinS[2] };
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < kMinReach) { return -2; }
    float c = 0.0f;
    switch (pose) {
        case 0: c = d[0] / n; break;    // forward
        case 1: c = d[2] / n; break;    // side
        case 2: c = d[1] / n; break;    // up
        case 3: c = -d[1] / n; break;   // down
        case 4: c = (d[2] - d[1]) * 0.70710678f / n; break;   // Build C, seated: down and OUT at 45 deg (0, -0.7071, +0.7071)
        default: return -1;
    }
    return c >= 0.80f ? 1 : -1;
}

// Forearm-in-hand: handB^T . normalise(T - origin) (armfit.cpp:871-881's form, origin = S in level, the pivot here).
inline bool ForearmInHand(const float hB[9], const float T[3], const float origin[3], float out[3])
{
    float f[3] = { T[0] - origin[0], T[1] - origin[1], T[2] - origin[2] };
    const float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 1e-4f)) { return false; }
    for (int k = 0; k < 3; ++k) { f[k] /= n; }
    for (int c = 0; c < 3; ++c) { out[c] = hB[c] * f[0] + hB[3 + c] * f[1] + hB[6 + c] * f[2]; }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

}  // namespace calibraw
}  // namespace dlvr
