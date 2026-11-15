// MENU PIN - the maths, with no engine and no Windows in it (docs/SPEC-UI-MENU-PIN-2026-10-11.md §3). Tested offline by
// src/src/tests/uipin_logic_test.cpp; used by uipin.cpp inside the engine's per-eye UI camera function.
//
// A camera matrix here is the engine's: 12 floats, row-major 3x4. Its COLUMNS are the camera's axes in the world -
// column 0 right, column 1 up, column 2 BACK (the view looks along -column 2, INV-273) - and column 3 is its position.
// The scene's cameras are right-handed and in metres. The in-game UI camera is the same three axes in a pixel world
// whose y runs down the screen, so its matrix is mirrored (determinant -1); everything below is written in the
// camera's OWN axes, so it holds for both.
#pragma once

#include <cmath>

namespace dlvr {
namespace uipin {

struct Mat34 {
    float m[12];
};

inline float Det(const Mat34& a)
{
    const float* m = a.m;
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

// True when the 3x3 part is orthonormal within `tol` and every value is finite. Written so a NaN fails.
inline bool IsRigid(const Mat34& a, float tol)
{
    const float* m = a.m;
    for (int i = 0; i < 12; ++i) {
        if (!(m[i] > -1.0e9f && m[i] < 1.0e9f)) { return false; }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            const float d = m[i] * m[j] + m[4 + i] * m[4 + j] + m[8 + i] * m[8 + j];   // column i . column j
            const float want = (i == j) ? 1.0f : 0.0f;
            if (!(std::fabs(d - want) < tol)) { return false; }
        }
    }
    return true;
}

// a then-in-b's-frame: the matrix of camera `b` expressed in the world, when `b` is given in `a`'s own axes.
inline Mat34 Mul(const Mat34& a, const Mat34& b)
{
    Mat34 o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            o.m[r * 4 + c] = a.m[r * 4 + 0] * b.m[0 * 4 + c] + a.m[r * 4 + 1] * b.m[1 * 4 + c] + a.m[r * 4 + 2] * b.m[2 * 4 + c];
        }
        o.m[r * 4 + 3] += a.m[r * 4 + 3];
    }
    return o;
}

// The inverse of an orthonormal matrix (mirrored or not): the transpose, and the position taken back through it.
inline Mat34 Inverse(const Mat34& a)
{
    Mat34 o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { o.m[r * 4 + c] = a.m[c * 4 + r]; }
    }
    for (int r = 0; r < 3; ++r) {
        o.m[r * 4 + 3] = -(o.m[r * 4 + 0] * a.m[3] + o.m[r * 4 + 1] * a.m[7] + o.m[r * 4 + 2] * a.m[11]);
    }
    return o;
}

// Camera `b` in camera `a`'s own axes. The positions are subtracted FIRST: far from the world's origin (the wasteland
// is kilometres across) taking each position through the inverse and then subtracting loses a millimetre in float,
// which the pixels-per-metre scale turns into a visible pixel of shake.
inline Mat34 Relative(const Mat34& a, const Mat34& b)
{
    Mat34 o;
    const float d[3] = { b.m[3] - a.m[3], b.m[7] - a.m[7], b.m[11] - a.m[11] };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            o.m[r * 4 + c] = a.m[0 * 4 + r] * b.m[0 * 4 + c] + a.m[1 * 4 + r] * b.m[1 * 4 + c] + a.m[2 * 4 + r] * b.m[2 * 4 + c];
        }
        o.m[r * 4 + 3] = a.m[0 * 4 + r] * d[0] + a.m[1 * 4 + r] * d[1] + a.m[2 * 4 + r] * d[2];
    }
    return o;
}

// The offset the engine gave this eye: its copy sits at `src.pos + e * src.column0` (INV-260). Negative = left eye.
inline float EyeOffsetOf(const Mat34& src, const Mat34& engineOut)
{
    const float dx = engineOut.m[3] - src.m[3], dy = engineOut.m[7] - src.m[7], dz = engineOut.m[11] - src.m[11];
    return dx * src.m[0] + dy * src.m[4] + dz * src.m[8];
}

// SPEC §3a - the main menu: the UI is drawn from the scene's own camera, each eye offset along ITS right axis.
inline Mat34 MainMenu(const Mat34& scene, float e)
{
    Mat34 o = scene;
    o.m[3] += e * scene.m[0];
    o.m[7] += e * scene.m[4];
    o.m[11] += e * scene.m[8];
    return o;
}

// How far in front of the UI camera the UI plane lies, in the UI's units: the plane is z = 0 and the camera looks
// along -column 2, so it is the camera's position measured along column 2.
inline float PlaneDistance(const Mat34& src)
{
    return src.m[3] * src.m[2] + src.m[7] * src.m[6] + src.m[11] * src.m[10];
}

// SPEC §3b - an in-game menu. `scene0` is the scene camera when the menu opened, `scene` is it now; `distanceM` is how
// far away the menu should sit, in metres. The UI camera moves, in its own units, by the scene camera's motion since
// the menu opened; the eye offset `e` (metres) is carried in the same units, so the menu has real depth.
// `unitsPerMetre` comes out for the log. A `distanceM` at or below zero leaves the units unscaled (1 unit = 1 metre).
inline Mat34 Pinned(const Mat34& src, const Mat34& scene0, const Mat34& scene, float e, float distanceM, float* unitsPerMetre)
{
    const float k = distanceM > 0.0f ? PlaneDistance(src) / distanceM : 1.0f;
    if (unitsPerMetre) { *unitsPerMetre = k; }
    Mat34 d = Relative(scene0, scene);              // the scene camera now, in its own axes as they were at the opening
    d.m[3] *= k; d.m[7] *= k; d.m[11] *= k;         // metres -> the UI's units
    Mat34 o = Mul(src, d);
    const float ke = k * e;
    o.m[3] += ke * o.m[0];
    o.m[7] += ke * o.m[4];
    o.m[11] += ke * o.m[8];
    return o;
}

// A point of the world as the camera sees it: x right, y up, z back (so a visible point has z < 0).
inline void ToCamera(const Mat34& cam, const float p[3], float out[3])
{
    const Mat34 inv = Inverse(cam);
    for (int r = 0; r < 3; ++r) {
        out[r] = inv.m[r * 4 + 0] * p[0] + inv.m[r * 4 + 1] * p[1] + inv.m[r * 4 + 2] * p[2] + inv.m[r * 4 + 3];
    }
}

}  // namespace uipin
}  // namespace dlvr
