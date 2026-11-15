// bladeline_logic.h - the pure half of the blade-length hit line (mode-2 melee, B1).
// docs/superpowers/specs/2026-10-26-blade-length-hit-line-design.md §4.2-4.3; test src/src/tests/bladeline_logic_test.cpp.
// The rows come from bladeline_table.inc (tools/bladeline_table.py). No Windows, no logging, no globals, no allocation.
#pragma once

#include <cmath>
#include <cstring>

namespace dlvr {
namespace bladeline {

// One oh_ box of a mesh (spec §7.2, INV-629), in its Holder frame, UNPADDED: centre, the half-axis vector of its largest extent
// (l) and of its second (w); the thinnest axis is dropped (the box sweeps as the plane of l and w).
struct BladeBox { float c[3]; float l[3]; float w[3]; };

// One in-hand melee mesh: its shaft direction in its Holder frame (unit), its real tip along that shaft from Holder (m), and its
// boxes kBladeBoxes[box0 .. box0 + boxN) (boxN 0 = none: the primary line only).
struct BladeRow { const char* mesh; float dir[3]; float tipM; unsigned short box0; unsigned short boxN; };

constexpr int kMaxBoxSegs = 12;   // 6 boxes x 2 edges (the generator refuses a mesh with more boxes)

// The row for a NormMesh-normalised mesh name; -1 when absent, null or empty.
inline int FindRow(const char* norm, const BladeRow* rows, unsigned n)
{
    if (!norm || !norm[0] || !rows) { return -1; }
    for (unsigned i = 0; i < n; ++i) {
        if (rows[i].mesh && std::strcmp(rows[i].mesh, norm) == 0) { return static_cast<int>(i); }
    }
    return -1;
}

// dirLocal (Holder frame) into the world through the Holder's world 3x4 (g_getWorld layout: rows, t at 3 / 7 / 11), normalised.
inline void ShaftWorld(const float m34[12], const float dirLocal[3], float out[3])
{
    for (int i = 0; i < 3; ++i) {
        out[i] = m34[4 * i] * dirLocal[0] + m34[4 * i + 1] * dirLocal[1] + m34[4 * i + 2] * dirLocal[2];
    }
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 1e-6f) { out[0] /= l; out[1] /= l; out[2] /= l; }
}

// The drawn weapon's hit segment: start = Holder + dirW x along, along = the grip's projection on the shaft clamped to
// [0, tip - 0.01] (the further up the shaft of Holder and the hand); end = Holder + dirW x max(tip + margin, along + minReach)
// (spec §7.3 - minReach 0 = off). False on a non-finite input or a degenerate shaft.
inline bool Segment(const float m34[12], const BladeRow& r, const float grip[3], float margin, float minReach, float start[3],
                    float end[3], float* along)
{
    for (int i = 0; i < 12; ++i) { if (!std::isfinite(m34[i])) { return false; } }
    for (int i = 0; i < 3; ++i) { if (!std::isfinite(grip[i])) { return false; } }
    if (!std::isfinite(margin) || !std::isfinite(minReach) || !std::isfinite(r.tipM) || !(r.tipM > 0.01f)) { return false; }
    float d[3];
    ShaftWorld(m34, r.dir, d);
    if (!(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 0.5f)) { return false; }
    const float h[3] = { m34[3], m34[7], m34[11] };
    float a = (grip[0] - h[0]) * d[0] + (grip[1] - h[1]) * d[1] + (grip[2] - h[2]) * d[2];
    const float aMax = r.tipM - 0.01f;
    a = a < 0.0f ? 0.0f : (a > aMax ? aMax : a);
    float e = r.tipM + margin;
    if (a + minReach > e) { e = a + minReach; }
    for (int i = 0; i < 3; ++i) {
        start[i] = h[i] + d[i] * a;
        end[i] = h[i] + d[i] * e;
    }
    if (along) { *along = a; }
    return true;
}

// One ring frame's segment: blade = from the drawn weapon (a row and a good read), else unused.
struct Seg { bool blade; float s[3]; float e[3]; };

// A sub-step's pair: both blade segments as given, otherwise BOTH today's line (grip, grip + reach x bladeDir), blade = false -
// a quad never bridges a drawn segment and a controller one.
inline void PickPair(const Seg& a, const Seg& b, const float aGrip[3], const float aBladeDir[3], const float bGrip[3],
                     const float bBladeDir[3], float reach, Seg* outA, Seg* outB)
{
    if (a.blade && b.blade) {
        *outA = a;
        *outB = b;
        return;
    }
    outA->blade = outB->blade = false;
    for (int i = 0; i < 3; ++i) {
        outA->s[i] = aGrip[i];
        outA->e[i] = aGrip[i] + reach * aBladeDir[i];
        outB->s[i] = bGrip[i];
        outB->e[i] = bGrip[i] + reach * bBladeDir[i];
    }
}

// ===== Revision 2 (spec §7.2 / §7.4): the oh_ boxes as swept edges =====

// A Holder-frame point into the world through the Holder's world 3x4 (rows, t at 3 / 7 / 11).
inline void PointWorld(const float m34[12], const float p[3], float out[3])
{
    for (int i = 0; i < 3; ++i) {
        out[i] = m34[4 * i] * p[0] + m34[4 * i + 1] * p[1] + m34[4 * i + 2] * p[2] + m34[4 * i + 3];
    }
}

// v scaled up (along itself) to length >= minLen; a zero vector stays zero.
inline void PadTo(const float v[3], float minLen, float out[3])
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float k = (l > 1e-9f && l < minLen) ? minLen / l : 1.0f;
    for (int i = 0; i < 3; ++i) { out[i] = v[i] * k; }
}

// One box's world segments: 1 (c - l -> c + l) when the AUTHORED 2|w| < 0.05 m (a shaft - decided before the padding), else 2
// (c - l + w -> c + l + w, c - l - w -> c + l - w: the two long edges). l and w padded to >= 0.025 m half extent (the engine's
// 0.05 m minimum, INV-629). 0 on a non-finite input.
inline int BoxSegments(const float m34[12], const BladeBox& b, float s[2][3], float e[2][3])
{
    for (int i = 0; i < 12; ++i) { if (!std::isfinite(m34[i])) { return 0; } }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(b.c[i]) || !std::isfinite(b.l[i]) || !std::isfinite(b.w[i])) { return 0; }
    }
    const float wl = std::sqrt(b.w[0] * b.w[0] + b.w[1] * b.w[1] + b.w[2] * b.w[2]);
    const int n = (2.0f * wl < 0.05f) ? 1 : 2;
    float l[3], w[3];
    PadTo(b.l, 0.025f, l);
    PadTo(b.w, 0.025f, w);
    for (int k = 0; k < n; ++k) {
        const float sg = (n == 1) ? 0.0f : (k == 0 ? 1.0f : -1.0f);
        float ps[3], pe[3];
        for (int i = 0; i < 3; ++i) {
            ps[i] = b.c[i] - l[i] + sg * w[i];
            pe[i] = b.c[i] + l[i] + sg * w[i];
        }
        PointWorld(m34, ps, s[k]);
        PointWorld(m34, pe, e[k]);
    }
    return n;
}

// A row's segments: boxes [box0, box0 + boxN) clamped to nBoxes, in box order (then edge 0 / 1), at most maxSegs.
inline int RowSegments(const float m34[12], const BladeRow& r, const BladeBox* boxes, unsigned nBoxes, float s[][3], float e[][3],
                       int maxSegs)
{
    if (!boxes || maxSegs <= 0) { return 0; }
    int n = 0;
    const unsigned end = static_cast<unsigned>(r.box0) + r.boxN;
    for (unsigned i = r.box0; i < end && i < nBoxes; ++i) {
        float bs[2][3], be[2][3];
        const int k = BoxSegments(m34, boxes[i], bs, be);
        for (int j = 0; j < k && n < maxSegs; ++j, ++n) {
            for (int c = 0; c < 3; ++c) {
                s[n][c] = bs[j][c];
                e[n][c] = be[j][c];
            }
        }
        if (n >= maxSegs) { break; }
    }
    return n;
}

// One ring frame's drawn weapon for the box quads: a good Holder read (ok), its row, the Holder world 3x4.
struct BoxFrame { bool ok; int row; float m[12]; };

// A sub-step may add box quads only between two good frames of the SAME row - never a quad from one mesh's edge k to another's.
inline bool BoxPairOk(const BoxFrame& a, const BoxFrame& b) { return a.ok && b.ok && a.row >= 0 && a.row == b.row; }

// ===== §9 - the hit capsule (Jay 2026-10-26, the miss probe run) =====

// The primary segment's two capsule ends: start moves `back` metres back along the segment (toward the elbow), end moves
// `thick` further past itself. False (and nothing changed) on a zero-length or non-finite segment.
inline bool Capsule(float start[3], float end[3], float back, float thick)
{
    float d[3];
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(start[i]) || !std::isfinite(end[i])) { return false; }
        d[i] = end[i] - start[i];
    }
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-6f) || !std::isfinite(back) || !std::isfinite(thick)) { return false; }
    for (int i = 0; i < 3; ++i) {
        start[i] -= d[i] / l * back;
        end[i] += d[i] / l * thick;
    }
    return true;
}

// The capsule's two parallel copies of a sub-step's primary pair, offset +-r along the swing plane's normal
// n = normalise(dir(b) x (b.e - a.e)) - one n for both frames, so a copy never twists. Copy 0 at +r x n, copy 1 at -r x n.
// Returns 2, or 0 (radius <= 0, either frame off the blade line, or no motion of the end - no plane).
inline int ThickCopies(const Seg& a, const Seg& b, float r, float as[2][3], float ae[2][3], float bs[2][3], float be[2][3])
{
    if (!(r > 0.0f) || !a.blade || !b.blade) { return 0; }
    const float d[3] = { b.e[0] - b.s[0], b.e[1] - b.s[1], b.e[2] - b.s[2] };
    const float m[3] = { b.e[0] - a.e[0], b.e[1] - a.e[1], b.e[2] - a.e[2] };
    float n[3] = { d[1] * m[2] - d[2] * m[1], d[2] * m[0] - d[0] * m[2], d[0] * m[1] - d[1] * m[0] };
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(l > 1e-6f)) { return 0; }
    for (int i = 0; i < 3; ++i) { n[i] /= l; }
    for (int k = 0; k < 2; ++k) {
        const float o = k == 0 ? r : -r;
        for (int i = 0; i < 3; ++i) {
            as[k][i] = a.s[i] + n[i] * o;
            ae[k][i] = a.e[i] + n[i] * o;
            bs[k][i] = b.s[i] + n[i] * o;
            be[k][i] = b.e[i] + n[i] * o;
        }
    }
    return 2;
}

// §9.3 - the factor the game's OWN line would have given this contact: clamp(1 - |contact - grip| / reach, 0, 1). An entry at
// exactly 1.0 (the factor's default - the game did not compute it) and a reach <= 0 are left as they are.
inline float RestoredFactor(float orig, const float contact[3], const float grip[3], float reach)
{
    if (orig == 1.0f || !(reach > 0.0f)) { return orig; }
    const float d[3] = { contact[0] - grip[0], contact[1] - grip[1], contact[2] - grip[2] };
    float f = 1.0f - std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / reach;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return std::isfinite(f) ? f : orig;
}

}  // namespace bladeline
}  // namespace dlvr
