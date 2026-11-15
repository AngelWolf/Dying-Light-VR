// gunshape_logic.h - the solid gun's geometry decisions, header-only and unit-tested (tests/gunshape_logic_test.cpp). No game reads.
// Spec docs/superpowers/specs/2026-11-02-solid-guns-reload-sound-haptics-design.md G1 / G2; plan
// docs/superpowers/plans/2026-11-02-solid-guns-reload-sound-haptics.md Task 1; rulings in .superpowers/sdd/2026-11-02-solid-guns/progress.md.
//
// FRAMES. A gun's shapes are in its MODEL frame (node 0 of the mesh - RESEARCH-SOLID-GUNS-2026-11-02 step 3), posed with the gun's
// world matrix (mtx34 row-major: row r = [4r..4r+2], translation [4r+3]; rotation taken with unit columns, as handcoll PoseShape).
// THE WELL is empty space between `wall` boxes (the generator fits walls around it); the Channel describes that space so a held mag
// can be guided in: open end `origin`, `axis` = unit direction INTO the gun, cross-section halfW along `wAxis` and halfD along
// axis x wAxis, `depth` from the open end to the top of the well.
// A MOVING box (the held mag) is given in its hand frame; its axis[0] is its insertion direction (toward the gun when seated).
#pragma once

#include "handcoll_logic.h"   // ContactEdge (the one-pulse-per-touch rule, reused)

#include <algorithm>
#include <cmath>
#include <vector>

namespace dlvr {
namespace gunshape {

struct Obb {
    float c[3];          // centre
    float axis[3][3];    // axis[i] = unit axis i (rows)
    float half[3];       // half extent along axis[i]
    bool  wall;          // beside the magwell (the mag stop treats walls by the channel rule)
    unsigned char part;  // which geometry node of the mesh it came from (0..31) - see GunShape::hiddenParts
};

struct Channel {
    float origin[3];     // the open end (gun model frame)
    float axis[3];       // unit, into the gun
    float wAxis[3];      // unit, across the well (halfW's direction); halfD is along axis x wAxis
    float halfW, halfD, depth;
};

struct GunShape {
    const Obb* boxes;
    int        nBoxes;
    Channel    well;
    bool       hasWell;
    unsigned   hiddenParts;  // bit p set = part p is hidden by the game this update (a suppressor, shells): its boxes are skipped.
                             // 0 = every part solid. The runtime sets it from the parts' element hidden flags.
};

namespace detail {

inline bool Finite(float v) { return std::isfinite(v) != 0; }
inline bool Finite3(const float v[3]) { return Finite(v[0]) && Finite(v[1]) && Finite(v[2]); }
inline bool Finite12(const float m[12])
{
    for (int i = 0; i < 12; ++i) { if (!Finite(m[i])) { return false; } }
    return true;
}
inline float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

// The pose's rotation with unit columns (n, row-major 3x3) and its translation.
inline void UnitRot(const float m34[12], float n[9])
{
    for (int col = 0; col < 3; ++col) {
        const float x = m34[col], y = m34[4 + col], z = m34[8 + col];
        const float len = std::sqrt(x * x + y * y + z * z);
        const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
        n[col] = x * inv; n[3 + col] = y * inv; n[6 + col] = z * inv;
    }
}
inline void ToWorldPt(const float n[9], const float m34[12], const float p[3], float o[3])
{
    for (int r = 0; r < 3; ++r) { o[r] = n[r * 3] * p[0] + n[r * 3 + 1] * p[1] + n[r * 3 + 2] * p[2] + m34[r * 4 + 3]; }
}
inline void ToWorldDir(const float n[9], const float v[3], float o[3])
{
    for (int r = 0; r < 3; ++r) { o[r] = n[r * 3] * v[0] + n[r * 3 + 1] * v[1] + n[r * 3 + 2] * v[2]; }
}
inline void ToLocalPt(const float n[9], const float m34[12], const float p[3], float o[3])
{
    const float d[3] = { p[0] - m34[3], p[1] - m34[7], p[2] - m34[11] };
    for (int c = 0; c < 3; ++c) { o[c] = n[c] * d[0] + n[3 + c] * d[1] + n[6 + c] * d[2]; }
}
inline void ToLocalDir(const float n[9], const float v[3], float o[3])
{
    for (int c = 0; c < 3; ++c) { o[c] = n[c] * v[0] + n[3 + c] * v[1] + n[6 + c] * v[2]; }
}

inline bool Hidden(const GunShape& g, const Obb& b) { return b.part < 32 && (g.hiddenParts & (1u << b.part)) != 0; }

inline bool InsideBox(const Obb& b, const float p[3])
{
    const float d[3] = { p[0] - b.c[0], p[1] - b.c[1], p[2] - b.c[2] };
    for (int i = 0; i < 3; ++i) { if (std::fabs(Dot(d, b.axis[i])) > b.half[i]) { return false; } }
    return true;
}

// Slab test: the parameter t in [0, len] where o + u t first enters the box; -1 = never.
inline float RayEnterBox(const Obb& b, const float o[3], const float u[3], float len)
{
    float t0 = 0.0f, t1 = len;
    const float d[3] = { o[0] - b.c[0], o[1] - b.c[1], o[2] - b.c[2] };
    for (int i = 0; i < 3; ++i) {
        const float po = Dot(d, b.axis[i]), pu = Dot(u, b.axis[i]);
        if (std::fabs(pu) < 1e-9f) {
            if (std::fabs(po) > b.half[i]) { return -1.0f; }
            continue;
        }
        float a = (-b.half[i] - po) / pu, c = (b.half[i] - po) / pu;
        if (a > c) { const float s = a; a = c; c = s; }
        if (a > t0) { t0 = a; }
        if (c < t1) { t1 = c; }
        if (t0 > t1) { return -1.0f; }
    }
    return t0;
}

// Projection radius of a box onto unit L.
inline float Radius(const Obb& b, const float L[3])
{
    return b.half[0] * std::fabs(Dot(b.axis[0], L)) + b.half[1] * std::fabs(Dot(b.axis[1], L)) + b.half[2] * std::fabs(Dot(b.axis[2], L));
}

// Separating-axis minimal translation that moves `m` out of `a`. False = not overlapping.
inline bool Mtv(const Obb& a, const Obb& m, float out[3])
{
    float axes[15][3];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) { axes[n][k] = a.axis[i][k]; }
        ++n;
        for (int k = 0; k < 3; ++k) { axes[n][k] = m.axis[i][k]; }
        ++n;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float c[3];
            Cross(a.axis[i], m.axis[j], c);
            const float l = std::sqrt(Dot(c, c));
            if (l < 1e-5f) { continue; }
            for (int k = 0; k < 3; ++k) { axes[n][k] = c[k] / l; }
            ++n;
        }
    }
    const float d[3] = { m.c[0] - a.c[0], m.c[1] - a.c[1], m.c[2] - a.c[2] };
    float best = -1.0f;
    float bestL[3] = { 0, 0, 0 };
    for (int i = 0; i < n; ++i) {
        const float* L = axes[i];
        const float dl = Dot(d, L);
        const float ov = Radius(a, L) + Radius(m, L) - std::fabs(dl);
        if (ov <= 0.0f) { return false; }
        if (best < 0.0f || ov < best) {
            best = ov;
            const float s = dl >= 0.0f ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) { bestL[k] = L[k] * s; }
        }
    }
    for (int k = 0; k < 3; ++k) { out[k] = bestL[k] * best; }
    return true;
}

// The push across the channel only (along wAxis or the depth axis, whichever overlap is smaller). False = not overlapping.
inline bool AcrossMtv(const Obb& a, const Obb& m, const Channel& ch, float out[3])
{
    float dAxis[3];
    Cross(ch.axis, ch.wAxis, dAxis);
    const float* L2[2] = { ch.wAxis, dAxis };
    const float d[3] = { m.c[0] - a.c[0], m.c[1] - a.c[1], m.c[2] - a.c[2] };
    float best = -1.0f;
    float bestL[3] = { 0, 0, 0 };
    for (int i = 0; i < 2; ++i) {
        const float dl = Dot(d, L2[i]);
        const float ov = Radius(a, L2[i]) + Radius(m, L2[i]) - std::fabs(dl);
        if (ov <= 0.0f) { return false; }
        if (best < 0.0f || ov < best) {
            best = ov;
            const float s = dl >= 0.0f ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) { bestL[k] = L2[i][k] * s; }
        }
    }
    for (int k = 0; k < 3; ++k) { out[k] = bestL[k] * best; }
    return true;
}

// a x b, both row-major 3x4 (implicit last row 0 0 0 1).
inline void Mul34(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + (c == 3 ? a[4 * r + 3] : 0.0f);
        }
    }
}

}  // namespace detail

// Review finding 3: the held mag in the COMMANDED hand frame = inv(lastH) x drawnW, where lastH = the solve's commanded handholder
// pose of the previous update and drawnW = the mag as the reload drew it from that update's result. lastH's rotation is taken with
// unit columns (rigid). False = a degenerate / non-finite lastH or drawnW.
inline bool RelFromLast(const float lastH[12], const float drawnW[12], float rel[12])
{
    using namespace detail;
    if (!Finite12(lastH) || !Finite12(drawnW)) { return false; }
    float n[9];
    for (int col = 0; col < 3; ++col) {
        const float x = lastH[col], y = lastH[4 + col], z = lastH[8 + col];
        const float len = std::sqrt(x * x + y * y + z * z);
        if (!(len > 1e-6f)) { return false; }
        n[col] = x / len; n[3 + col] = y / len; n[6 + col] = z / len;
    }
    // inv = [n^T | -n^T t]
    float inv[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { inv[4 * r + c] = n[3 * c + r]; }
        inv[4 * r + 3] = -(n[r] * lastH[3] + n[3 + r] * lastH[7] + n[6 + r] * lastH[11]);
    }
    Mul34(inv, drawnW, rel);
    return true;
}

// RUN SG2's escape probe (Jay: "mag can also come out the side of the magwell"): true when the held mag (in its hand frame `hand34`)
// has its TOP past the well's lip (its centre along the axis + its reach > the open end, centre no deeper than `depth`) while its
// CENTRE is outside the well's footprint - i.e. it is leaving (or has left) the well sideways. *lateralOut = the centre's distance
// from the well's axis (m). False without a well or on a non-finite input.
inline bool MagEscapingWell(const GunShape& g, const float gun34[12], const Obb& mag, const float hand34[12], float* lateralOut)
{
    using namespace detail;
    *lateralOut = 0.0f;
    if (!g.hasWell || !Finite12(gun34) || !Finite12(hand34)) { return false; }
    float ng[9], nh[9];
    UnitRot(gun34, ng);
    UnitRot(hand34, nh);
    Obb m = mag;
    float wc[3];
    ToWorldPt(nh, hand34, mag.c, wc);
    ToLocalPt(ng, gun34, wc, m.c);
    for (int i = 0; i < 3; ++i) {
        float wa[3];
        ToWorldDir(nh, mag.axis[i], wa);
        ToLocalDir(ng, wa, m.axis[i]);
    }
    const Channel& ch = g.well;
    float dAxis[3];
    Cross(ch.axis, ch.wAxis, dAxis);
    const float rel[3] = { m.c[0] - ch.origin[0], m.c[1] - ch.origin[1], m.c[2] - ch.origin[2] };
    const float along = Dot(rel, ch.axis);
    const bool topIn = along + Radius(m, ch.axis) > 0.0f && along <= ch.depth;
    const float w = Dot(rel, ch.wAxis), d = Dot(rel, dAxis);
    *lateralOut = std::sqrt(w * w + d * d);
    return topIn && (std::fabs(w) > ch.halfW || std::fabs(d) > ch.halfD);
}

// Whose hand the gun stops (Task 4): only the LEFT (off) hand (armIdx 1), never while it grips the gun (the two-hand grip, a live
// reload step - it sits where the grip puts it), never when the held gun has no shape row.
inline bool ShouldStop(int armIdx, bool inGrip, bool haveRow) { return armIdx == 1 && !inGrip && haveRow; }

// The hand's fingertip against the hull. True + outTip = where S -> tip first enters a box, 1 mm back toward S (handcoll's rule); a
// box that already contains S is ignored. False + outTip = tip (no box entered, no boxes, or a non-finite input). All world space.
inline bool StopPointOnHull(const GunShape& g, const float gun34[12], const float S[3], const float tip[3], float outTip[3])
{
    using namespace detail;
    outTip[0] = tip[0]; outTip[1] = tip[1]; outTip[2] = tip[2];
    if (!g.boxes || g.nBoxes <= 0 || !Finite12(gun34) || !Finite3(S) || !Finite3(tip)) { return false; }
    float n[9];
    UnitRot(gun34, n);
    float s[3], t[3];
    ToLocalPt(n, gun34, S, s);
    ToLocalPt(n, gun34, tip, t);
    const float dv[3] = { t[0] - s[0], t[1] - s[1], t[2] - s[2] };
    const float len = std::sqrt(Dot(dv, dv));
    if (!(len > 1e-5f)) { return false; }
    const float u[3] = { dv[0] / len, dv[1] / len, dv[2] / len };
    float best = -1.0f;
    for (int i = 0; i < g.nBoxes; ++i) {
        if (Hidden(g, g.boxes[i])) { continue; }
        if (InsideBox(g.boxes[i], s)) { continue; }
        const float e = RayEnterBox(g.boxes[i], s, u, len);
        if (e < 0.0f) { continue; }
        if (best < 0.0f || e < best) { best = e; }
    }
    if (best < 0.0f) { return false; }
    float k = best - 0.001f;
    if (k < 0.0f) { k = 0.0f; }
    const float p[3] = { s[0] + u[0] * k, s[1] + u[1] * k, s[2] + u[2] * k };
    ToWorldPt(n, gun34, p, outTip);
    return true;
}

// The held mag (`mag`, in the hand frame `hand34`) against the hull. True + outShift (world) = the move that takes the mag out of
// every box (up to 4 passes, each box by the rule below); false + zero = no overlap or a non-finite input.
//   Admitted = the mag's insertion axis is within alignDeg of the channel axis AND its centre is inside the channel footprint
//   (|across| <= halfW / halfD, centre no deeper than depth). Walls: admitted -> pushed across the channel only (it slides in);
//   in the footprint but not admitted -> pushed back out along -axis until its top clears the walls' lip; outside the footprint ->
//   the full minimal translation. Non-wall boxes (the receiver above the well, the body): always the full minimal translation.
inline bool StopBoxOnHull(const GunShape& g, const float gun34[12], const Obb& mag, const float hand34[12], float alignDeg,
                          float outShift[3])
{
    using namespace detail;
    outShift[0] = outShift[1] = outShift[2] = 0.0f;
    if (!g.boxes || g.nBoxes <= 0 || !Finite12(gun34) || !Finite12(hand34) || !Finite(alignDeg)) { return false; }
    float ng[9], nh[9];
    UnitRot(gun34, ng);
    UnitRot(hand34, nh);
    // The mag in the gun's model frame.
    Obb m = mag;
    {
        float wc[3];
        ToWorldPt(nh, hand34, mag.c, wc);
        ToLocalPt(ng, gun34, wc, m.c);
        for (int i = 0; i < 3; ++i) {
            float wa[3];
            ToWorldDir(nh, mag.axis[i], wa);
            ToLocalDir(ng, wa, m.axis[i]);
        }
    }
    const float cosAlign = std::cos(alignDeg * 3.14159265358979f / 180.0f);
    float total[3] = { 0, 0, 0 };
    bool moved = false;
    for (int pass = 0; pass < 4; ++pass) {
        bool any = false;
        bool inFoot = false, admitted = false;
        float dAxis[3] = { 0, 0, 0 };
        if (g.hasWell) {
            const Channel& ch = g.well;
            Cross(ch.axis, ch.wAxis, dAxis);
            const float rel[3] = { m.c[0] - ch.origin[0], m.c[1] - ch.origin[1], m.c[2] - ch.origin[2] };
            // Review finding 1: the mag must also REACH the opening (its centre along the axis + its own reach >= the open end) -
            // a mag held lower (mid-air, at the pouch) is not "in the channel".
            const float along = Dot(rel, ch.axis);
            inFoot = std::fabs(Dot(rel, ch.wAxis)) <= ch.halfW && std::fabs(Dot(rel, dAxis)) <= ch.halfD && along <= ch.depth &&
                     along + Radius(m, ch.axis) >= 0.0f;
            admitted = inFoot && Dot(m.axis[0], ch.axis) >= cosAlign;
        }
        for (int i = 0; i < g.nBoxes; ++i) {
            const Obb& a = g.boxes[i];
            if (Hidden(g, a)) { continue; }
            float push[3] = { 0, 0, 0 };
            bool hit = false;
            if (a.wall && g.hasWell && inFoot) {
                if (admitted) {
                    // Review finding 1: only a wall the mag really overlaps (all 15 axes) pushes it - AcrossMtv alone tests two axes.
                    float probe[3];
                    hit = Mtv(a, m, probe) && AcrossMtv(a, m, g.well, push);
                } else {
                    float probe[3];
                    if (Mtv(a, m, probe)) {
                        // Out along -axis: the mag's top (its highest projection on the axis) down to the wall's lowest.
                        const float* ax = g.well.axis;
                        const float magTop = Dot(m.c, ax) + Radius(m, ax);
                        const float lip = Dot(a.c, ax) - Radius(a, ax);
                        const float over = magTop - lip;
                        if (over > 0.0f) {
                            for (int k = 0; k < 3; ++k) { push[k] = -ax[k] * over; }
                            hit = true;
                        }
                    }
                }
            } else {
                hit = Mtv(a, m, push);
            }
            if (!hit) { continue; }
            for (int k = 0; k < 3; ++k) { m.c[k] += push[k]; total[k] += push[k]; }
            any = true;
            moved = true;
        }
        if (!any) { break; }
    }
    if (!moved) { return false; }
    ToWorldDir(ng, total, outShift);
    return true;
}

// ===== THE HAND AS SPHERES (Jay 2026-11-02, after RUN SG4: "consider the players hand & fingers, I have a feeling that you're only
// basing it off of the controller & not the modelled hand ingame"). The old stop tested ONE point (the handholder + 6 cm); Blender
// against the game's own hand mesh: when that point touched, up to 554 of 2216 hand vertices were already inside the pistol. The hand
// is now the game hand's spheres (tools/hand_shape.py -> hand_shape_table.inc, placed on the drawn finger joints by gunshape.cpp). =====

// The sphere (centre c, radius r - the box's frame) touches the box: the distance from c to the box <= r.
inline bool SphereTouchesBox(const Obb& b, const float c[3], float r)
{
    using namespace detail;
    const float d[3] = { c[0] - b.c[0], c[1] - b.c[1], c[2] - b.c[2] };
    float s = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float q = Dot(d, b.axis[i]);
        const float o = std::fabs(q) - b.half[i];
        if (o > r) { return false; }
        if (o > 0.0f) { s += o * o; }
    }
    return s <= r * r;
}

// The smallest move (box frame) that leaves the sphere just touching the box's surface: outside the box -> along (c - closest point);
// centre inside -> out through the nearest face. False + zero = not touching.
inline bool SpherePushOut(const Obb& b, const float c[3], float r, float out[3])
{
    using namespace detail;
    out[0] = out[1] = out[2] = 0.0f;
    const float d[3] = { c[0] - b.c[0], c[1] - b.c[1], c[2] - b.c[2] };
    float q[3], cl[3];
    bool inside = true;
    for (int i = 0; i < 3; ++i) {
        q[i] = Dot(d, b.axis[i]);
        cl[i] = q[i] > b.half[i] ? b.half[i] : (q[i] < -b.half[i] ? -b.half[i] : q[i]);
        if (cl[i] != q[i]) { inside = false; }
    }
    if (!inside) {
        const float e[3] = { q[0] - cl[0], q[1] - cl[1], q[2] - cl[2] };
        const float dist = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (dist >= r) { return false; }
        inside = !(dist > 1e-9f);   // the centre ON the surface: no direction from it - out through the nearest face below
    }
    if (!inside) {
        const float e[3] = { q[0] - cl[0], q[1] - cl[1], q[2] - cl[2] };
        const float dist = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        const float k = (r - dist) / dist;
        for (int i = 0; i < 3; ++i) {
            for (int w = 0; w < 3; ++w) { out[w] += b.axis[i][w] * e[i] * k; }
        }
        return true;
    }
    int best = 0;
    float bestPen = b.half[0] - std::fabs(q[0]);
    for (int i = 1; i < 3; ++i) {
        const float p = b.half[i] - std::fabs(q[i]);
        if (p < bestPen) { bestPen = p; best = i; }
    }
    const float s = (q[best] >= 0.0f ? 1.0f : -1.0f) * (bestPen + r);
    for (int w = 0; w < 3; ++w) { out[w] = b.axis[best][w] * s; }
    return true;
}

// COST (2026-11-02 bench, /O2, the real hand table): testing every sphere against every box near the hand cost 0.4-2.5 ms per solve
// near a gun and 1.0-7.7 ms touching one (wn_shisha_gun, 1896 boxes) - the hand is ~0.2 m across, so "near the hand" was every box.
// THE BOX GRID: a gun's boxes binned once (per row, when it is drawn) into cubes of kGridCell in its model frame; a sphere tests only
// the boxes in the cells its own bounds touch. Built by BuildBoxGrid; passed per body (nullptr = test every box, as before).
constexpr float kGridCell = 0.02f;
struct BoxGrid {
    float mn[3] = { 0, 0, 0 };   // the boxes' bounds (model frame)
    float mx[3] = { 0, 0, 0 };
    int   n[3] = { 0, 0, 0 };    // cells per axis
    std::vector<int> start;      // n0*n1*n2 + 1 offsets into idx
    std::vector<unsigned short> idx;
    bool  ok = false;
};
// Bins every box of g by its own bounds (an OBB's world-axis bounds). False (grid.ok false) for no boxes, > 65535 boxes or > 2M cells.
inline bool BuildBoxGrid(const GunShape& g, BoxGrid* grid)
{
    grid->ok = false;
    grid->start.clear();
    grid->idx.clear();
    if (!g.boxes || g.nBoxes <= 0 || g.nBoxes > 65535) { return false; }
    float bmn[3] = { 1e9f, 1e9f, 1e9f }, bmx[3] = { -1e9f, -1e9f, -1e9f };
    std::vector<float> lo(static_cast<size_t>(g.nBoxes) * 3), hi(static_cast<size_t>(g.nBoxes) * 3);
    for (int k = 0; k < g.nBoxes; ++k) {
        const Obb& b = g.boxes[k];
        for (int a = 0; a < 3; ++a) {
            const float e = b.half[0] * std::fabs(b.axis[0][a]) + b.half[1] * std::fabs(b.axis[1][a]) + b.half[2] * std::fabs(b.axis[2][a]);
            lo[3 * k + a] = b.c[a] - e;
            hi[3 * k + a] = b.c[a] + e;
            if (lo[3 * k + a] < bmn[a]) { bmn[a] = lo[3 * k + a]; }
            if (hi[3 * k + a] > bmx[a]) { bmx[a] = hi[3 * k + a]; }
        }
    }
    long long cells = 1;
    for (int a = 0; a < 3; ++a) {
        grid->mn[a] = bmn[a];
        grid->mx[a] = bmx[a];
        grid->n[a] = static_cast<int>(std::floor((bmx[a] - bmn[a]) / kGridCell)) + 1;
        cells *= grid->n[a];
    }
    if (cells > 2000000) { return false; }
    std::vector<int> count(static_cast<size_t>(cells), 0);
    auto cellRange = [&](int k, int a, int* c0, int* c1) {
        *c0 = static_cast<int>(std::floor((lo[3 * k + a] - bmn[a]) / kGridCell));
        *c1 = static_cast<int>(std::floor((hi[3 * k + a] - bmn[a]) / kGridCell));
        if (*c0 < 0) { *c0 = 0; }
        if (*c1 >= grid->n[a]) { *c1 = grid->n[a] - 1; }
    };
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            grid->start.assign(static_cast<size_t>(cells) + 1, 0);
            for (long long c = 0; c < cells; ++c) { grid->start[static_cast<size_t>(c) + 1] = grid->start[static_cast<size_t>(c)] + count[static_cast<size_t>(c)]; }
            grid->idx.assign(static_cast<size_t>(grid->start[static_cast<size_t>(cells)]), 0);
            std::fill(count.begin(), count.end(), 0);
        }
        for (int k = 0; k < g.nBoxes; ++k) {
            int x0, x1, y0, y1, z0, z1;
            cellRange(k, 0, &x0, &x1);
            cellRange(k, 1, &y0, &y1);
            cellRange(k, 2, &z0, &z1);
            for (int x = x0; x <= x1; ++x) {
                for (int y = y0; y <= y1; ++y) {
                    for (int z = z0; z <= z1; ++z) {
                        const size_t c = (static_cast<size_t>(x) * grid->n[1] + y) * grid->n[2] + z;
                        if (pass == 1) { grid->idx[static_cast<size_t>(grid->start[c] + count[c])] = static_cast<unsigned short>(k); }
                        ++count[c];
                    }
                }
            }
        }
    }
    grid->ok = true;
    return true;
}

// A solid the hand meets: a shape and its world pose (the gun's hull; the seated mag in bone_magazine's frame), and its box grid
// (nullptr / not ok = every box is tested).
struct HandBody {
    const GunShape* g;
    const float*    pose34;
    const BoxGrid*  grid = nullptr;
};
struct HandHit {
    bool startedInside;   // the hand overlapped at `from` (the gun swung into a still hand) - resolved by pushing out
    int  sphere;          // the first sphere that touched (-1 none)
    int  body;            // its body (-1 none)
};
// THE WELL'S MOUTH during a reload's mag step (Pulling / MagInHand / FreshInHand): the hand holds the mag at the gun's opening, so
// against body 0 the well's walls are ignored, and so is every box for a sphere whose centre is inside the channel's footprint widened
// by kMouthMargin and no deeper into the gun than kMouthIn. RULING (2026-11-02, not Jay's numbers - the hand's fingers wrap a mag
// base about one finger-width past its sides): 30 mm / 10 mm. Cost if wrong: fingers sink a little into the grip's bottom edge while
// a mag is held there, or a mag step's hand is shoved off the mag.
constexpr float kMouthMargin = 0.030f;
constexpr float kMouthIn = 0.010f;

namespace detail {

constexpr int kHandMaxCand = 2048;   // the largest row has 1896 boxes (wn_shisha_gun) - above this every box is a candidate
struct HandBodyCache {
    float n[9];
    float bc[3];    // the body's bounding sphere (model frame)
    float br;
    // COST: the boxes near the hand's swept path (centre within the hand's reach + the box's own reach + 5 cm of the path), found once
    // per call - every overlap test after that walks only these. nCand < 0 = all boxes.
    unsigned short cand[kHandMaxCand];
    int nCand;
};

inline bool InMouth(const GunShape& g, const float lc[3])
{
    if (!g.hasWell) { return false; }
    const Channel& ch = g.well;
    float dAxis[3];
    Cross(ch.axis, ch.wAxis, dAxis);
    const float rel[3] = { lc[0] - ch.origin[0], lc[1] - ch.origin[1], lc[2] - ch.origin[2] };
    return std::fabs(Dot(rel, ch.wAxis)) <= ch.halfW + kMouthMargin && std::fabs(Dot(rel, dAxis)) <= ch.halfD + kMouthMargin &&
           Dot(rel, ch.axis) <= kMouthIn;
}

// Does the hand (wrist at w) touch any body? First (sphere, body, box) found; with `push` set, the largest single push-out (world);
// with `sum` set too (push required), the SUM of every sphere-box push-out (world) - the ghost's contact normal, which averages the
// voxel boxes' faces instead of picking one.
inline bool HandOverlap(const HandBody* bodies, const HandBodyCache* cache, int nb, const float (*off)[3], const float* r, int n,
                        const float offC[3], float handR, const float w[3], bool mouth, int* sOut, int* bOut, float* push,
                        float* sum = nullptr)
{
    if (sum) { sum[0] = sum[1] = sum[2] = 0.0f; }
    bool any = false;
    float bestLen = -1.0f;
    for (int b = 0; b < nb; ++b) {
        const GunShape& g = *bodies[b].g;
        const HandBodyCache& hc = cache[b];
        const float hcw[3] = { w[0] + offC[0], w[1] + offC[1], w[2] + offC[2] };
        float hcl[3];
        ToLocalPt(hc.n, bodies[b].pose34, hcw, hcl);
        const float dd[3] = { hcl[0] - hc.bc[0], hcl[1] - hc.bc[1], hcl[2] - hc.bc[2] };
        const float lim = hc.br + handR;
        if (Dot(dd, dd) > lim * lim) { continue; }   // broad phase: the whole hand is clear of this body
        const BoxGrid* gr = bodies[b].grid && bodies[b].grid->ok ? bodies[b].grid : nullptr;
        for (int i = 0; i < n; ++i) {
            const float cw[3] = { w[0] + off[i][0], w[1] + off[i][1], w[2] + off[i][2] };
            float lc[3];
            ToLocalPt(hc.n, bodies[b].pose34, cw, lc);
            const float ri = r[i];
            if (gr) {
                // the sphere's bounds against the boxes' bounds: clear of every box
                if (lc[0] + ri < gr->mn[0] || lc[0] - ri > gr->mx[0] || lc[1] + ri < gr->mn[1] || lc[1] - ri > gr->mx[1] ||
                    lc[2] + ri < gr->mn[2] || lc[2] - ri > gr->mx[2]) { continue; }
            } else {
                const float de[3] = { lc[0] - hc.bc[0], lc[1] - hc.bc[1], lc[2] - hc.bc[2] };
                const float le = hc.br + ri;
                if (Dot(de, de) > le * le) { continue; }
            }
            const bool exemptAll = mouth && b == 0 && InMouth(g, lc);
            if (exemptAll) { continue; }
            // the boxes to test: the grid's cells under the sphere's bounds, or the candidate list / every box
            int c0[3] = { 0, 0, 0 }, c1[3] = { 0, 0, 0 };
            if (gr) {
                for (int a = 0; a < 3; ++a) {
                    c0[a] = static_cast<int>(std::floor((lc[a] - ri - gr->mn[a]) / kGridCell));
                    c1[a] = static_cast<int>(std::floor((lc[a] + ri - gr->mn[a]) / kGridCell));
                    if (c0[a] < 0) { c0[a] = 0; }
                    if (c1[a] >= gr->n[a]) { c1[a] = gr->n[a] - 1; }
                }
            }
            auto testBox = [&](int k) -> bool {   // true = stop looking (a touch found, touch-only mode)
                const Obb& box = g.boxes[k];
                if (Hidden(g, box)) { return false; }
                if (mouth && b == 0 && box.wall) { return false; }
                if (!push) {
                    if (SphereTouchesBox(box, lc, ri)) {
                        *sOut = i; *bOut = b;
                        return true;
                    }
                    return false;
                }
                float pl[3];
                if (!SpherePushOut(box, lc, ri, pl)) { return false; }
                const float len = Dot(pl, pl);
                if (!any) { *sOut = i; *bOut = b; }
                any = true;
                if (len > bestLen) {
                    bestLen = len;
                    ToWorldDir(hc.n, pl, push);
                }
                if (sum) {
                    float pw[3];
                    ToWorldDir(hc.n, pl, pw);
                    sum[0] += pw[0]; sum[1] += pw[1]; sum[2] += pw[2];
                }
                return false;
            };
            if (gr) {
                for (int x = c0[0]; x <= c1[0]; ++x) {
                    for (int y = c0[1]; y <= c1[1]; ++y) {
                        for (int z = c0[2]; z <= c1[2]; ++z) {
                            const size_t c = (static_cast<size_t>(x) * gr->n[1] + y) * gr->n[2] + z;
                            for (int q = gr->start[c]; q < gr->start[c + 1]; ++q) {
                                if (testBox(gr->idx[static_cast<size_t>(q)])) { return true; }
                            }
                        }
                    }
                }
            } else {
                const int nk = hc.nCand >= 0 ? hc.nCand : g.nBoxes;
                for (int kk = 0; kk < nk; ++kk) {
                    if (testBox(hc.nCand >= 0 ? hc.cand[kk] : kk)) { return true; }
                }
            }
        }
    }
    return any;
}

// The per-call set-up shared by StopHandOnBodies and GhostStep: each body's rotation and bounding sphere, and (without a grid) the
// candidate boxes within reach of the hand centre's path from -> to plus `margin`; the hand's centre offset, reach and smallest
// radius. False = no boxes, a non-finite pose / sphere, or nb outside 1..4.
inline bool PrepareHand(const HandBody* bodies, int nb, const float (*off)[3], const float* r, int n, const float from[3],
                        const float to[3], float margin, HandBodyCache* cache, float offC[3], float* handROut, float* minROut)
{
    if (!bodies || nb <= 0 || nb > 4 || !off || !r || n <= 0 || !Finite3(from) || !Finite3(to)) { return false; }
    bool anyBoxes = false;
    for (int b = 0; b < nb; ++b) {
        const GunShape& g = *bodies[b].g;
        if (!g.boxes || g.nBoxes <= 0 || !Finite12(bodies[b].pose34)) { return false; }
        anyBoxes = true;
        UnitRot(bodies[b].pose34, cache[b].n);
        float c[3] = { 0, 0, 0 };
        for (int k = 0; k < g.nBoxes; ++k) { for (int w = 0; w < 3; ++w) { c[w] += g.boxes[k].c[w]; } }
        for (int w = 0; w < 3; ++w) { c[w] /= static_cast<float>(g.nBoxes); }
        float br = 0.0f;
        for (int k = 0; k < g.nBoxes; ++k) {
            const Obb& bx = g.boxes[k];
            const float d[3] = { bx.c[0] - c[0], bx.c[1] - c[1], bx.c[2] - c[2] };
            const float e = std::sqrt(Dot(d, d)) + std::sqrt(bx.half[0] * bx.half[0] + bx.half[1] * bx.half[1] + bx.half[2] * bx.half[2]);
            if (e > br) { br = e; }
        }
        for (int w = 0; w < 3; ++w) { cache[b].bc[w] = c[w]; }
        cache[b].br = br;
    }
    if (!anyBoxes) { return false; }
    offC[0] = offC[1] = offC[2] = 0.0f;
    float minR = 1e9f;
    for (int i = 0; i < n; ++i) {
        if (!Finite3(off[i]) || !Finite(r[i]) || !(r[i] > 0.0f)) { return false; }
        for (int w = 0; w < 3; ++w) { offC[w] += off[i][w]; }
        if (r[i] < minR) { minR = r[i]; }
    }
    for (int w = 0; w < 3; ++w) { offC[w] /= static_cast<float>(n); }
    float handR = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d[3] = { off[i][0] - offC[0], off[i][1] - offC[1], off[i][2] - offC[2] };
        const float e = std::sqrt(Dot(d, d)) + r[i];
        if (e > handR) { handR = e; }
    }
    // The candidates: boxes within reach of the hand's centre's path from -> to (model frame), + margin.
    for (int b = 0; b < nb; ++b) {
        HandBodyCache& hc = cache[b];
        const GunShape& g = *bodies[b].g;
        hc.nCand = -1;
        if (g.nBoxes > kHandMaxCand || (bodies[b].grid && bodies[b].grid->ok)) { continue; }   // the grid replaces the list
        const float pw0[3] = { from[0] + offC[0], from[1] + offC[1], from[2] + offC[2] };
        const float pw1[3] = { to[0] + offC[0], to[1] + offC[1], to[2] + offC[2] };
        float p0[3], p1[3];
        ToLocalPt(hc.n, bodies[b].pose34, pw0, p0);
        ToLocalPt(hc.n, bodies[b].pose34, pw1, p1);
        const float sg[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const float sl2 = Dot(sg, sg);
        int m = 0;
        for (int k = 0; k < g.nBoxes; ++k) {
            const Obb& bx = g.boxes[k];
            const float q[3] = { bx.c[0] - p0[0], bx.c[1] - p0[1], bx.c[2] - p0[2] };
            float t = sl2 > 1e-12f ? Dot(q, sg) / sl2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float e[3] = { q[0] - sg[0] * t, q[1] - sg[1] * t, q[2] - sg[2] * t };
            const float reach = handR + std::sqrt(bx.half[0] * bx.half[0] + bx.half[1] * bx.half[1] + bx.half[2] * bx.half[2]) + margin;
            if (Dot(e, e) <= reach * reach) { hc.cand[m++] = static_cast<unsigned short>(k); }
        }
        hc.nCand = m;
    }
    *handROut = handR;
    *minROut = minR;
    return true;
}

// The sweep from -> to, the rotation held, in steps of at most the smallest radius (<= 512; the broad phase skips a body the whole
// hand is clear of), the first touching step bisected 8 times. True = it touched: *tFree = the last free fraction, *tHit = the first
// touching one, *sOut / *bOut = the touching sphere / body. False = clear all the way (or from == to). `from` must be clear.
// COST vs tunnelling: one smallest radius per step - consecutive positions of the smallest sphere still overlap, so no box (the
// thinnest is a 5 mm voxel) fits between two of them (HandNeverTunnelsThinPlate).
inline bool SweepHand(const HandBody* bodies, const HandBodyCache* cache, int nb, const float (*off)[3], const float* r, int n,
                      const float offC[3], float handR, float minR, const float from[3], const float to[3], bool mouth, float* tFreeOut,
                      float* tHitOut, int* sOut, int* bOut)
{
    const float dv[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float len = std::sqrt(Dot(dv, dv));
    if (!(len > 1e-6f)) { return false; }
    int steps = static_cast<int>(std::ceil(len / minR));
    if (steps > 512) { steps = 512; }
    if (steps < 1) { steps = 1; }
    float tFree = 0.0f, tHit = -1.0f;
    int s = -1, b = -1;
    for (int k = 1; k <= steps; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const float p[3] = { from[0] + dv[0] * t, from[1] + dv[1] * t, from[2] + dv[2] * t };
        if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, p, mouth, &s, &b, nullptr)) {
            tHit = t;
            *sOut = s;
            *bOut = b;
            break;
        }
        tFree = t;
    }
    if (tHit < 0.0f) { return false; }
    for (int it = 0; it < 8; ++it) {   // 8 halvings: a step / 256 (~0.02 mm for a 5 mm step)
        const float t = 0.5f * (tFree + tHit);
        const float p[3] = { from[0] + dv[0] * t, from[1] + dv[1] * t, from[2] + dv[2] * t };
        int s2 = -1, b2 = -1;
        if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, p, mouth, &s2, &b2, nullptr)) {
            tHit = t;
            *sOut = s2;
            *bOut = b2;
        } else {
            tFree = t;
        }
    }
    *tFreeOut = tFree;
    *tHitOut = tHit;
    return true;
}

inline bool Norm3(float v[3])
{
    const float l = std::sqrt(Dot(v, v));
    if (!(l > 1e-9f) || !Finite(l)) { return false; }
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}
}  // namespace detail

// THE HAND STOP. The hand = n spheres, off[i] = sphere i's centre minus the wrist (world, already turned by the hand's target
// rotation), radius r[i]. The wrist moves from `from` (where it was last - free) to `to` (this solve's target), the rotation held:
//   - the hand already touching at `from` (the gun moved into it): pushed out at `to` - up to 4 passes, each the largest single
//     sphere-box push-out; hit->startedInside;
//   - else swept from -> to in steps of at most the smallest radius (<= 512 steps; the broad phase skips a body the whole hand is
//     clear of), the first touching step bisected (8 halvings), and the wrist left 1 mm short of contact along the path.
// True + out = the wrist moved; false + out = to (free, nothing to stop on, or a non-finite input). `mouth` = the reload's mag-step
// exemption at body 0's well (kMouthMargin above). All world space.
inline bool StopHandOnBodies(const HandBody* bodies, int nb, const float (*off)[3], const float* r, int n, const float from[3],
                             const float to[3], bool mouth, float out[3], HandHit* hit)
{
    using namespace detail;
    out[0] = to[0]; out[1] = to[1]; out[2] = to[2];
    hit->startedInside = false;
    hit->sphere = -1;
    hit->body = -1;
    if (!bodies || nb <= 0 || nb > 4 || !off || !r || n <= 0 || !Finite3(from) || !Finite3(to)) { return false; }
    HandBodyCache cache[4];
    float offC[3], handR, minR;
    if (!PrepareHand(bodies, nb, off, r, n, from, to, 0.05f, cache, offC, &handR, &minR)) { return false; }
    int s = -1, bb = -1;
    if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, from, mouth, &s, &bb, nullptr)) {
        hit->startedInside = true;
        hit->sphere = s;
        hit->body = bb;
        bool moved = false;
        for (int pass = 0; pass < 4; ++pass) {
            float push[3] = { 0, 0, 0 };
            int s2 = -1, b2 = -1;
            if (!HandOverlap(bodies, cache, nb, off, r, n, offC, handR, out, mouth, &s2, &b2, push)) { break; }
            // 0.5 mm past touching, so the next solve's sweep starts clear
            const float pl = std::sqrt(Dot(push, push));
            const float k = pl > 1e-9f ? (pl + 0.0005f) / pl : 1.0f;
            for (int w = 0; w < 3; ++w) { out[w] += push[w] * k; }
            moved = true;
        }
        return moved;
    }
    float tFree = 0.0f, tHit = -1.0f;
    if (!SweepHand(bodies, cache, nb, off, r, n, offC, handR, minR, from, to, mouth, &tFree, &tHit, &s, &bb)) { return false; }
    hit->sphere = s;
    hit->body = bb;
    const float dv[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float len = std::sqrt(Dot(dv, dv));
    float back = tFree * len - 0.001f;
    if (back < 0.0f) { back = 0.0f; }
    for (int w = 0; w < 3; ++w) { out[w] = from[w] + dv[w] / len * back; }
    return true;
}

// ===== THE GHOST HAND - D-309's replacement (Jay 2026-11-02: "I want this fixed from the ground up" -> "Build it."). RUN SG5: the
// solid hand resolved PER SOLVE on TWO threads, each sweeping from the other's last wrist, and pushed a hand the gun had moved into out
// along the deepest box's face - a different face frame to frame: "the left hand/arm are essentially having a seizure". The ghost:
//   - ONE wrist position, kept between steps and advanced by ONE owner (gunshape.cpp decides who; both solves read the result);
//   - the caller keeps it in the GUN's frame, so a moving gun carries the hand - it never starts a step inside because the gun moved;
//   - moved only by SLIDING (collide-and-slide: the part of the motion INTO the surface removed, the rest kept), never by a push-out;
//   - the only start-inside left is the hand's OWN turn / finger curl since the last step: walked back along the LAST contact normal
//     (the same face it was on), not the nearest face. =====

constexpr float kGhostTouchM = 0.0005f;    // the ghost held back from the target by more than this = touching
// RULING (not Jay's numbers - say so at the check-in): the target this far from the ghost AND the hand free there -> the ghost jumps
// to it (the controller went through and out the far side, or the gun was pulled away from a pressing hand).
constexpr float kGhostReleaseM = 0.10f;
// RULING: a hand turned / curled into the gun is walked back along the contact normal at most this far; deeper -> the ghost is dropped
// and the hand passes free until it is clear (never a large shove).
constexpr float kGhostDepenMaxM = 0.03f;
constexpr int   kGhostSlides = 3;          // collide-and-slide iterations per step (a corner takes two)
// THE SLIDE BUDGET (the offline replay, ghost_replay_test: without it the ghost skated 10-34 mm in ONE step - it slid by the
// tangential part of the WHOLE gap to a target deep inside the gun, so a small change of the surface normal swung a 2-8 cm lever).
// After a contact, the step's sliding is at most the controller's own move since the last step (in the gun's frame) plus this
// catch-up. Free motion (no contact) is never limited. RULING (not Jay's number): 1 mm per step (~6 cm/s at the pad-poll's 60 / s).
constexpr float kGhostCatchUpM = 0.001f;

struct GhostState {
    bool  have = false;              // false = no ghost: the hand follows the target. Entered only from a CLEAR target.
    float w[3] = { 0, 0, 0 };        // the ghost wrist (world, in the frame the caller passes this step)
    float nrm[3] = { 0, 0, 0 };      // the last contact normal (unit, out of the gun; zero = none)
    float to[3] = { 0, 0, 0 };       // the last step's target (same frame) - the slide budget's controller move
    bool  haveTo = false;
};
struct GhostResult {
    bool  touching = false;   // the ghost is held back from the target by > kGhostTouchM
    bool  depen = false;      // the hand's own turn / curl put it into the gun: walked back along the normal
    float depenM = 0.0f;
    bool  dropped = false;    // that walk-back needed > kGhostDepenMaxM: the ghost was dropped
    bool  released = false;   // the target was > kGhostReleaseM away and free: the ghost jumped to it
    bool  waiting = false;    // no ghost and the target is inside: the hand passes free until it is clear
    int   sphere = -1;        // the last contact's sphere / body
    int   body = -1;
    float heldBackM = 0.0f;   // |target - ghost| after the step
};

// ONE STEP of the ghost. `to` = this step's wrist target; off / r = the hand's n spheres turned by this step's rotation (world, centre
// minus the wrist); `mouth` = the reload's mag-step exemption (kMouthMargin). out = where the wrist goes (= st->w when there is a
// ghost). True = touching (out held back from `to`). All world space; the caller moves st->w / st->nrm in and out of the gun's frame.
// COST: as StopHandOnBodies, up to kGhostSlides sweeps + one sum-of-pushes per contact; a walk-back <= 15 overlap tests + 6 halvings.
inline bool GhostStep(const HandBody* bodies, int nb, const float (*off)[3], const float* r, int n, const float to[3], bool mouth,
                      GhostState* st, float out[3], GhostResult* res)
{
    using namespace detail;
    *res = GhostResult{};
    if (!Finite3(to)) { st->have = false; return false; }
    out[0] = to[0]; out[1] = to[1]; out[2] = to[2];
    if (st->have && !Finite3(st->w)) { st->have = false; }
    float g[3] = { to[0], to[1], to[2] };
    if (st->have) { g[0] = st->w[0]; g[1] = st->w[1]; g[2] = st->w[2]; }
    const float span[3] = { to[0] - g[0], to[1] - g[1], to[2] - g[2] };
    HandBodyCache cache[4];
    float offC[3], handR, minR;
    if (!PrepareHand(bodies, nb, off, r, n, g, to, 0.05f + std::sqrt(Dot(span, span)), cache, offC, &handR, &minR)) {
        st->have = false;
        return false;
    }
    int s = -1, b = -1;
    if (!st->have) {
        if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, to, mouth, &s, &b, nullptr)) {
            res->waiting = true;   // never enter a ghost from inside - that would be a push-out
            return false;
        }
        st->have = true;
        for (int k = 0; k < 3; ++k) { st->w[k] = to[k]; st->nrm[k] = 0.0f; st->to[k] = to[k]; }
        st->haveTo = true;
        return false;
    }
    // The slide budget for this step: the controller's own move since the last step + the catch-up (no previous target: unlimited).
    float slideBudget = 1e9f;
    if (st->haveTo && Finite3(st->to)) {
        const float mv[3] = { to[0] - st->to[0], to[1] - st->to[1], to[2] - st->to[2] };
        slideBudget = std::sqrt(Dot(mv, mv)) + kGhostCatchUpM;
    }
    for (int k = 0; k < 3; ++k) { st->to[k] = to[k]; }
    st->haveTo = true;
    const bool wasTouching = Dot(st->nrm, st->nrm) > 0.5f;   // the last step ended in contact (the normal clears on a free step)
    // 1. The hand's own turn / curl since the last step put it into the gun: walk it back along the last contact normal.
    if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, g, mouth, &s, &b, nullptr)) {
        res->sphere = s;
        res->body = b;
        float dir[3] = { st->nrm[0], st->nrm[1], st->nrm[2] };
        if (!Norm3(dir)) {
            float push[3] = { 0, 0, 0 }, sum[3] = { 0, 0, 0 };
            int s2 = -1, b2 = -1;
            HandOverlap(bodies, cache, nb, off, r, n, offC, handR, g, mouth, &s2, &b2, push, sum);
            for (int k = 0; k < 3; ++k) { dir[k] = sum[k]; }
            if (!Norm3(dir)) {
                for (int k = 0; k < 3; ++k) { dir[k] = push[k]; }
                if (!Norm3(dir)) { st->have = false; res->dropped = true; return false; }
            }
        }
        const float stepM = 0.002f;
        float lo = 0.0f, hi = -1.0f;
        for (int k = 1; k * stepM <= kGhostDepenMaxM + 1e-6f; ++k) {
            const float d = static_cast<float>(k) * stepM;
            const float p[3] = { g[0] + dir[0] * d, g[1] + dir[1] * d, g[2] + dir[2] * d };
            int s2 = -1, b2 = -1;
            if (!HandOverlap(bodies, cache, nb, off, r, n, offC, handR, p, mouth, &s2, &b2, nullptr)) { hi = d; break; }
            lo = d;
        }
        if (hi < 0.0f) {
            st->have = false;   // deeper than the cap: drop it, the hand passes free until clear (no shove)
            res->dropped = true;
            return false;
        }
        for (int it = 0; it < 6; ++it) {
            const float d = 0.5f * (lo + hi);
            const float p[3] = { g[0] + dir[0] * d, g[1] + dir[1] * d, g[2] + dir[2] * d };
            int s2 = -1, b2 = -1;
            if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, p, mouth, &s2, &b2, nullptr)) { lo = d; } else { hi = d; }
        }
        const float d = hi + 0.0005f;
        for (int k = 0; k < 3; ++k) { g[k] += dir[k] * d; st->nrm[k] = dir[k]; }
        res->depen = true;
        res->depenM = d;
    }
    // 2. The target far from the ghost and free: jump to it.
    {
        const float dv[3] = { to[0] - g[0], to[1] - g[1], to[2] - g[2] };
        int s2 = -1, b2 = -1;
        if (Dot(dv, dv) > kGhostReleaseM * kGhostReleaseM &&
            !HandOverlap(bodies, cache, nb, off, r, n, offC, handR, to, mouth, &s2, &b2, nullptr)) {
            for (int k = 0; k < 3; ++k) { st->w[k] = to[k]; st->nrm[k] = 0.0f; }
            res->released = true;
            return false;
        }
    }
    // 3. Collide and slide toward the target. A step that STARTS in contact (the last step touched) is budgeted from its first
    //    sweep - else a held-back hand whose straight path clears an edge would jump the whole gap at once; a free step is budgeted
    //    only after its first contact.
    float rem[3] = { to[0] - g[0], to[1] - g[1], to[2] - g[2] };
    bool contact = false;
    const bool limited = wasTouching;
    for (int it = 0; it < kGhostSlides; ++it) {
        float rl = std::sqrt(Dot(rem, rem));
        if (!(rl > 1e-6f)) { break; }
        if (limited || contact) {
            if (!(slideBudget > 1e-6f)) { break; }
            if (rl > slideBudget) {
                for (int k = 0; k < 3; ++k) { rem[k] *= slideBudget / rl; }
                rl = slideBudget;
            }
        }
        const float tgt[3] = { g[0] + rem[0], g[1] + rem[1], g[2] + rem[2] };
        float tFree = 0.0f, tHit = -1.0f;
        int s2 = -1, b2 = -1;
        if (!SweepHand(bodies, cache, nb, off, r, n, offC, handR, minR, g, tgt, mouth, &tFree, &tHit, &s2, &b2)) {
            for (int k = 0; k < 3; ++k) { g[k] = tgt[k]; }
            break;
        }
        contact = true;
        res->sphere = s2;
        res->body = b2;
        // The contact normal: the SUM of every sphere-box push at the first touching point (averages the voxel faces).
        float nrm[3] = { 0, 0, 0 };
        {
            const float pHit[3] = { g[0] + rem[0] * tHit, g[1] + rem[1] * tHit, g[2] + rem[2] * tHit };
            float push[3] = { 0, 0, 0 }, sum[3] = { 0, 0, 0 };
            int s3 = -1, b3 = -1;
            if (HandOverlap(bodies, cache, nb, off, r, n, offC, handR, pHit, mouth, &s3, &b3, push, sum)) {
                for (int k = 0; k < 3; ++k) { nrm[k] = sum[k]; }
                if (!Norm3(nrm)) {
                    for (int k = 0; k < 3; ++k) { nrm[k] = push[k]; }
                    Norm3(nrm);
                }
            }
        }
        // 1 mm short along the path.
        float back = tFree * rl - 0.001f;
        if (back < 0.0f) { back = 0.0f; }
        for (int k = 0; k < 3; ++k) { g[k] += rem[k] / rl * back; }
        if (limited || it > 0) { slideBudget -= back; }   // the budget counts every move made under it
        if (!(Dot(nrm, nrm) > 0.5f)) { break; }   // no normal: stay at the contact
        for (int k = 0; k < 3; ++k) { st->nrm[k] = nrm[k]; }
        // What is left of the motion, with its part INTO the surface removed - the slide.
        const float left = rl - back;
        float rr[3] = { rem[0] / rl * left, rem[1] / rl * left, rem[2] / rl * left };
        const float into = Dot(rr, nrm);
        if (into < 0.0f) { for (int k = 0; k < 3; ++k) { rr[k] -= nrm[k] * into; } }
        for (int k = 0; k < 3; ++k) { rem[k] = rr[k]; }
    }
    (void)contact;
    for (int k = 0; k < 3; ++k) { st->w[k] = g[k]; out[k] = g[k]; }
    const float hb[3] = { to[0] - g[0], to[1] - g[1], to[2] - g[2] };
    res->heldBackM = std::sqrt(Dot(hb, hb));
    res->touching = res->heldBackM > kGhostTouchM;
    // The normal clears ONLY when the hand is back on its target. (The replay: clearing it on a step whose budget ran out before any
    // contact - still held back - made the next step "free" and jump the whole gap: 10-23 mm.)
    if (!res->touching) { st->nrm[0] = st->nrm[1] = st->nrm[2] = 0.0f; }
    return res->touching;
}

}  // namespace gunshape
}  // namespace dlvr
