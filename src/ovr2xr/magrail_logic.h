// magrail_logic.h - THE HELD MAG ON THE WELL'S LINE (pure, unit-tested: src/src/tests/magrail_logic_test.cpp). No game includes.
// Jay 2026-11-02 (4), after the hand-on-gun collision was dropped ("Hands themselves, forget about it"): "Magazine is what we'll keep,
// so I want that back on & fixed". Behaviour agreed in chat (Jay: "Nothing" missing):
//   - PULLING: the grabbed mag stays seated and slides ONLY along the well (the hand's travel along the well's out axis); sideways /
//     up does nothing. Once its top clears the well's open end it eases into the fingers and follows the hand. A sideways pull keeps
//     it held (Jay: "Stay in fingers, out of the magwell" -> "Down the well first, then follows my hand").
//   - INSERTING: the fresh mag in the fingers locks onto the well's line when it comes to the open end lined up (within the align
//     limit, its centre within the capture radius of the line); pushing up slides it in. Outside the angle it stops at the lip.
//     Pulled back down past the ease it is free again.
//   - The ARM is never pushed (replaces gunshape.cpp GunShapeMagStop's push, D-309 / RUN SG2-SG7: 47 / 63 ESCAPE in SG4 / SG5).
// The decision has state (once on the rail it stays until the mag is clear - a sideways hand never pulls it out through the side).
// Matrices: row-major 3x4, translation at [3] [7] [11] (reload.cpp's convention).
#pragma once

#include "twohand_logic.h"   // SlerpM3

#include <cmath>

namespace dlvr {
namespace reload {
namespace rail {

enum class Kind { Free, Rail, Ease, Lip };

struct Params {
    float clearM = 0.0f;     // the seated mag's travel along the out axis until its TOP clears the well's open end (per gun)
    float easeM = 0.03f;     // past clearM: blend from the well's line to the hand over this travel
    float captureM = 0.03f;  // the held mag's centre within this of the well's line to lock on / stop at the lip
    float alignDeg = 45.0f;  // reload_align_deg: lined up enough to go in
};

struct State {
    bool onRail = false;
};

// s = how far the mag is OUT of its seat along the out axis (m, + = out); lateral = its centre's distance from the well's line;
// alignDeg = its rotation vs the seat's. pulling = the Pulling step (the grabbed mag starts on the rail: the driver sets
// st.onRail at the Grab). Returns what to draw.
inline Kind Decide(State& st, float s, float lateral, float alignDeg, const Params& p)
{
    const float freeAt = p.clearM + p.easeM;
    if (!std::isfinite(s) || !std::isfinite(lateral) || !std::isfinite(alignDeg)) { st.onRail = false; return Kind::Free; }
    if (!st.onRail) {
        if (lateral <= p.captureM && s < freeAt) {
            if (alignDeg <= p.alignDeg) {
                st.onRail = true;   // lock on - continuous: at s = freeAt the Ease weight is 1 (the hand's pose)
            } else if (s < p.clearM) {
                return Kind::Lip;
            } else {
                return Kind::Free;
            }
        } else {
            return Kind::Free;
        }
    }
    if (s >= freeAt) { st.onRail = false; return Kind::Free; }
    return s <= p.clearM ? Kind::Rail : Kind::Ease;
}

namespace detail {
inline float Len3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }
inline void Rot9(const float m[12], float r[9])
{
    for (int i = 0; i < 3; ++i) { for (int k = 0; k < 3; ++k) { r[3 * i + k] = m[4 * i + k]; } }
}
}  // namespace detail

// The mag's measures against the seat: s along `out` (unit), lateral off the line, the rotation angle (deg; 180 if degenerate).
inline void Measure(const float seat[12], const float out[3], const float mag[12], float* s, float* lateral, float* alignDeg)
{
    const float d[3] = { mag[3] - seat[3], mag[7] - seat[7], mag[11] - seat[11] };
    *s = d[0] * out[0] + d[1] * out[1] + d[2] * out[2];
    *lateral = detail::Len3(d[0] - *s * out[0], d[1] - *s * out[1], d[2] - *s * out[2]);
    float tr = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float la = detail::Len3(mag[c], mag[4 + c], mag[8 + c]);
        const float lb = detail::Len3(seat[c], seat[4 + c], seat[8 + c]);
        if (!(la > 1e-6f) || !(lb > 1e-6f)) { *alignDeg = 180.0f; return; }
        tr += (mag[c] * seat[c] + mag[4 + c] * seat[4 + c] + mag[8 + c] * seat[8 + c]) / (la * lb);
    }
    float ca = (tr - 1.0f) * 0.5f;
    ca = ca > 1.0f ? 1.0f : (ca < -1.0f ? -1.0f : ca);
    *alignDeg = std::acos(ca) * 57.2957795f;
}

// The pose to draw. sRail = the travel the rail pose uses (Pulling: the hand's travel along `out` since the grab; Inserting: s from
// Measure). inHand = the mag in the fingers (H x rel). Rail: the seat moved out by clamp(sRail, 0, clearM), the seat's rotation.
// Ease: Rail at clearM -> inHand by t = (sRail - clearM) / easeM. Lip: inHand moved back out along `out` to s = clearM. Free: inHand.
inline void Draw(Kind k, const float seat[12], const float out[3], float sRail, float sHand, const float inHand[12], const Params& p,
                 float w[12])
{
    for (int i = 0; i < 12; ++i) { w[i] = inHand[i]; }
    if (k == Kind::Free) { return; }
    if (k == Kind::Lip) {
        const float back = p.clearM - sHand;
        if (back > 0.0f) { w[3] += out[0] * back; w[7] += out[1] * back; w[11] += out[2] * back; }
        return;
    }
    const float along = sRail < 0.0f ? 0.0f : (sRail > p.clearM ? p.clearM : sRail);
    float railW[12];
    for (int i = 0; i < 12; ++i) { railW[i] = seat[i]; }
    railW[3] += out[0] * along; railW[7] += out[1] * along; railW[11] += out[2] * along;
    if (k == Kind::Rail) {
        for (int i = 0; i < 12; ++i) { w[i] = railW[i]; }
        return;
    }
    float t = p.easeM > 1e-6f ? (sRail - p.clearM) / p.easeM : 1.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float ra[9], rb[9], rr[9];
    detail::Rot9(railW, ra);
    detail::Rot9(inHand, rb);
    twohand::SlerpM3(ra, rb, t, rr);
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) { w[4 * i + c] = rr[3 * i + c]; }
        w[4 * i + 3] = railW[4 * i + 3] + (inHand[4 * i + 3] - railW[4 * i + 3]) * t;
    }
}

}  // namespace rail
}  // namespace reload
}  // namespace dlvr
