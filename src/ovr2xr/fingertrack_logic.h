#pragma once
// Controller finger tracking on an EMPTY hand - the pure part (spec docs/superpowers/specs/2026-11-11-controller-finger-tracking-design.md,
// plan ...-controller-finger-tracking.md Task 3). Header-only, no Windows includes; unit test src/tests/fingertrack_logic_test.cpp.
// Inputs (one controller) -> target weights per finger group (0 = Jay's OPEN shape, 1 = his CLOSED shape) -> eased weights -> each pin's
// local = a blend of its OPEN and CLOSED rows (rotation slerp, translation lerp).
//   index = trigger TOUCH (resting, not pressing - Jay: "Not from 'Squeezing' the trigger or grip buttons. Resting on them.")
//   thumb = ANY thumb touch (stick, A/B or X/Y, thumbrest) - one thumb-down shape for every surface
//   lower (middle / ring / little + hand1) = squeeze VALUE / fistAt, clamped (no grip touch sensor exists - VDXR mappings.cpp;
//           Jay: "That'll be fine then")
#include <cmath>

namespace dlvr {
namespace ft {

struct Inputs {
    bool  active;         // false: no bound / active touch actions this frame -> the hand opens (never frozen on a stale curl)
    bool  triggerTouch;
    bool  thumbTouch;
    float squeeze;        // 0..1, the runtime's raw squeeze/value
};

struct Weights {
    float index;
    float thumb;
    float lower;
};

enum class Group { Thumb, Index, Lower, Hand1 };

inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline Weights Target(const Inputs& in, float fistAt)
{
    if (!in.active) { return Weights{ 0.0f, 0.0f, 0.0f }; }
    float lower;
    if (fistAt > 1e-4f) {
        lower = Clamp01(in.squeeze / fistAt);
    } else {
        lower = in.squeeze > 0.0f ? 1.0f : 0.0f;   // fistAt 0: any press is a fist (no divide by zero)
    }
    return Weights{ in.triggerTouch ? 1.0f : 0.0f, in.thumbTouch ? 1.0f : 0.0f, lower };
}

// Each weight moves toward its target by at most dtSec / (easeMs / 1000) per call (a full 0 -> 1 takes easeMs); easeMs <= 0 -> target.
inline float EaseOne(float prev, float target, float step)
{
    const float d = target - prev;
    if (d > step) { return prev + step; }
    if (d < -step) { return prev - step; }
    return target;
}

inline Weights Ease(const Weights& prev, const Weights& target, float dtSec, float easeMs)
{
    if (easeMs <= 0.0f) { return target; }
    const float step = (dtSec > 0.0f ? dtSec : 0.0f) / (easeMs * 0.001f);
    return Weights{ EaseOne(prev.index, target.index, step), EaseOne(prev.thumb, target.thumb, step),
                    EaseOne(prev.lower, target.lower, step) };
}

// j in kFtPinName order: hand1, finger01, 01extra, 02, 03, 11, 12, 13, 21, 22, 23, 31, 32, 33, 41, 42, 43
inline Group GroupOfPin(int j)
{
    if (j == 0) { return Group::Hand1; }
    if (j <= 4) { return Group::Thumb; }
    if (j <= 7) { return Group::Index; }
    return Group::Lower;
}

inline float WeightOf(const Weights& w, Group g)
{
    switch (g) {
    case Group::Thumb: return w.thumb;
    case Group::Index: return w.index;
    case Group::Lower:
    case Group::Hand1: return w.lower;   // hand1 carries the ring + little fingers
    }
    return 0.0f;
}

// Rotation matrix (row-major 3x3) <-> unit quaternion (w, x, y, z).
inline void QuatFromMat(const float m[9], float q[4])
{
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[0] = (m[7] - m[5]) / s; q[1] = 0.25f * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25f * s; q[3] = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25f * s;
    }
}

inline void MatFromQuat(const float q[4], float m[9])
{
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}

// out = OPEN at w <= 0 and CLOSED at w >= 1, bit-exact; between them rotation by shortest-arc slerp, translation by lerp.
inline void BlendPin(const float openR[9], const float openT[3], const float closedR[9], const float closedT[3], float w,
                     float outR[9], float outT[3])
{
    if (w <= 0.0f) {
        for (int k = 0; k < 9; ++k) { outR[k] = openR[k]; }
        for (int k = 0; k < 3; ++k) { outT[k] = openT[k]; }
        return;
    }
    if (w >= 1.0f) {
        for (int k = 0; k < 9; ++k) { outR[k] = closedR[k]; }
        for (int k = 0; k < 3; ++k) { outT[k] = closedT[k]; }
        return;
    }
    float a[4], b[4];
    QuatFromMat(openR, a);
    QuatFromMat(closedR, b);
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) { for (int k = 0; k < 4; ++k) { b[k] = -b[k]; } d = -d; }
    float ka, kb;
    if (d > 0.9995f) {
        ka = 1.0f - w; kb = w;   // nearly equal: lerp (then normalised)
    } else {
        const float th = std::acos(d), s = std::sin(th);
        ka = std::sin((1.0f - w) * th) / s; kb = std::sin(w * th) / s;
    }
    float q[4], n = 0.0f;
    for (int k = 0; k < 4; ++k) { q[k] = ka * a[k] + kb * b[k]; n += q[k] * q[k]; }
    n = std::sqrt(n);
    for (int k = 0; k < 4; ++k) { q[k] /= n; }
    MatFromQuat(q, outR);
    for (int k = 0; k < 3; ++k) { outT[k] = openT[k] + (closedT[k] - openT[k]) * w; }
}

}  // namespace ft
}  // namespace dlvr
