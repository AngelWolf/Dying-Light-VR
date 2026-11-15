// foveation_logic.h - fixed foveated DLSS / FSR geometry. Header-only, no Windows headers: the bootstrap (dlssflat.cpp, namespace
// fovdlss) uses it, and tests/foveation_logic_test.cpp pins it - including CompositeWeight, the composite shader's weight maths.
// Design: docs/superpowers/specs/2026-11-11-foveated-dlss-design.md §4.2 / §4.5. Plan: docs/superpowers/plans/2026-11-11-foveated-dlss.md
// Task 1.
//
// CompositeWeight's formula is ported from CheekyFoveatedDLSS (https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS), GPL-3.0,
// src/d3d11_composite_shader.hpp (ShapeDistance + the feather weight of CompositeMain), used with the author's permission. The one
// change: his shape is centred on the image centre plus an offset; ours on the eye's straight-ahead point plus the offsets
// (FoveaCentre).
//
// The geometry g[12] is the layer's dlvr_FovGeom layout = dlvr_LensGeom's (lenscrop_logic.h header comment) with margin 0.
#pragma once

#include <cmath>
#include <cstdint>

#include "lenscrop_logic.h"

namespace dlvr {
namespace fov {

struct Params {
    float width, height, heightOffset, xOffset, roundness, transition, supersample, peripheryScale;
    int peripheryPreset;
    bool periphery, border;
};

// The eye's STRAIGHT-AHEAD direction in the image's uv (0, 0 = top left) - Jay, 2026-11-11: "Straight ahead" (over the middle of the
// displayable region, which on BK12 sits ~10 % of the width toward each temple). The eye's forward is (0, 0, -1) in the eye's frame.
// In Visible() (dlssflat.cpp, namespace lensmask) the declared-frame direction (tx, ty, -1) reaches the eye's frame through Rx(pitch):
// y2 = ty c + s, so forward is hx = 0 -> tx = 0 and y2 = 0 -> ty = -s / c. In uv (the mask's lerps inverted):
// u = img L / (img L + img R), v = (img U - ty) / (img U + img D). g[0..9] is validated as lenscrop::VisibleRows does. false = bad
// geometry.
inline bool LensCentre(const float g[12], float* u, float* v)
{
    if (!g || !u || !v) { return false; }
    lenscrop::Band b{};
    if (!lenscrop::VisibleRows(g, &b)) { return false; }   // validates g[0..9] (finite, tangents > 0, a rotation, c > 0)
    const double imgL = g[0], imgR = g[1], imgU = g[2], imgD = g[3];
    const double c = g[8], s = g[9];
    const double ty = -s / c;
    double um = imgL / (imgL + imgR);
    double vm = (imgU - ty) / (imgU + imgD);
    if (!std::isfinite(um) || !std::isfinite(vm)) { return false; }
    um = um < 0.0 ? 0.0 : (um > 1.0 ? 1.0 : um);
    vm = vm < 0.0 ? 0.0 : (vm > 1.0 ? 1.0 : vm);
    *u = static_cast<float>(um);
    *v = static_cast<float>(vm);
    return true;
}

// The fovea's centre: the straight-ahead point (u, v) moved by the two offsets, in Cheeky's units (d3d11_composite_shader.hpp:
// centered -= ShapeOffset * (1 - ShapeSize), centered in [-1, 1] -> uv shift offset * (1 - size) / 2).
// - heightOffset: + = DOWN (Cheeky's sign), both eyes alike.
// - xOffset (ours, Jay 2026-11-11 "add the horizontal slider"): + = toward the TEMPLE, mirrored per eye. eye = the history label;
//   0 = LEFT (INV-719), whose temple is the image's left (-u); 1 = RIGHT, temple +u.
inline void FoveaCentre(float u, float v, int eye, const Params& p, float* cu, float* cv)
{
    if (!cu || !cv) { return; }
    const float side = eye == 0 ? -1.0f : 1.0f;
    *cu = u + side * p.xOffset * (1.0f - p.width) * 0.5f;
    *cv = v + p.heightOffset * (1.0f - p.height) * 0.5f;
}

namespace detail {
// One axis: the uv span [a0, a1] (inside [0, 1]) snapped outward at the output size, its outward cover at the render size, and the
// output span re-derived from that cover by the ratio (still outward) - lenscrop::SnapRows' rule, without its "covers everything =
// refuse" (a fovea may cover the whole image).
inline bool SnapAxis(double a0, double a1, uint32_t renderN, uint32_t outN, uint32_t* in0, uint32_t* inLen, uint32_t* out0,
                     uint32_t* outLen)
{
    const double s = static_cast<double>(outN) / static_cast<double>(renderN);
    double o0 = std::floor(a0 * outN), o1 = std::ceil(a1 * outN);
    if (o0 < 0.0) { o0 = 0.0; }
    if (o1 > outN) { o1 = outN; }
    double i0 = std::floor(o0 / s), i1 = std::ceil(o1 / s);
    if (i0 < 0.0) { i0 = 0.0; }
    if (i1 > renderN) { i1 = renderN; }
    double r0 = std::floor(i0 * s + 1e-9), r1 = std::ceil(i1 * s - 1e-9);
    if (r0 > o0) { r0 = o0; }
    if (r1 < o1) { r1 = o1; }
    if (r0 < 0.0) { r0 = 0.0; }
    if (r1 > outN) { r1 = outN; }
    if (!(i1 > i0) || !(r1 > r0)) { return false; }
    *in0 = static_cast<uint32_t>(i0);
    *inLen = static_cast<uint32_t>(i1 - i0);
    *out0 = static_cast<uint32_t>(r0);
    *outLen = static_cast<uint32_t>(r1 - r0);
    return true;
}

// [c - len / 2, c + len / 2] moved (not shrunk) to lie inside [0, 1]; a length >= 1 is the whole axis.
inline void FitSpan(double c, double len, double* a0, double* a1)
{
    if (len >= 1.0) { *a0 = 0.0; *a1 = 1.0; return; }
    double lo = c - 0.5 * len;
    if (lo < 0.0) { lo = 0.0; }
    if (lo + len > 1.0) { lo = 1.0 - len; }
    *a0 = lo;
    *a1 = lo + len;
}
}  // namespace detail

constexpr uint32_t kMaxPrivate = 16384;   // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION

// The fovea's rectangles for one evaluation. The box is p.width x p.height of the image centred at the fovea centre (cu, cv) (from
// FoveaCentre - the offsets are already in it), moved inside the image. `in` = its outward cover in render pixels, `out` = the
// matching output rectangle (both axes as lenscrop::SnapRows), and the centre feature's private output is out x p.supersample,
// rounded, capped at kMaxPrivate. false = bad input.
inline bool FoveaRects(float cu, float cv, const Params& p, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                       lenscrop::Rect* in, lenscrop::Rect* out, uint32_t* outPrivW, uint32_t* outPrivH)
{
    if (!in || !out || !outPrivW || !outPrivH) { return false; }
    if (renderW == 0 || renderH == 0 || outW == 0 || outH == 0) { return false; }
    if (!std::isfinite(cu) || !std::isfinite(cv) || !std::isfinite(p.width) || !std::isfinite(p.height) ||
        !std::isfinite(p.supersample)) { return false; }
    if (!(p.width > 0.0f) || !(p.height > 0.0f) || !(p.supersample > 0.0f)) { return false; }
    double x0, x1, y0, y1;
    detail::FitSpan(cu, p.width, &x0, &x1);
    detail::FitSpan(cv, p.height, &y0, &y1);
    uint32_t ix, iw, ox, ow, iy, ih, oy, oh;
    if (!detail::SnapAxis(x0, x1, renderW, outW, &ix, &iw, &ox, &ow)) { return false; }
    if (!detail::SnapAxis(y0, y1, renderH, outH, &iy, &ih, &oy, &oh)) { return false; }
    *in = lenscrop::Rect{ ix, iy, iw, ih };
    *out = lenscrop::Rect{ ox, oy, ow, oh };
    double pw = std::floor(static_cast<double>(ow) * p.supersample + 0.5);
    double ph = std::floor(static_cast<double>(oh) * p.supersample + 0.5);
    if (pw < 1.0) { pw = 1.0; }
    if (ph < 1.0) { ph = 1.0; }
    if (pw > kMaxPrivate) { pw = kMaxPrivate; }
    if (ph > kMaxPrivate) { ph = kMaxPrivate; }
    *outPrivW = static_cast<uint32_t>(pw);
    *outPrivH = static_cast<uint32_t>(ph);
    return true;
}

// The periphery's working size: round(render x scale) per dimension, at least 32.
inline void WorkingSize(uint32_t renderW, uint32_t renderH, float scale, uint32_t* w, uint32_t* h)
{
    if (!w || !h) { return; }
    const double sc = std::isfinite(scale) && scale > 0.0f ? static_cast<double>(scale) : 0.0;
    double ww = std::floor(static_cast<double>(renderW) * sc + 0.5);
    double hh = std::floor(static_cast<double>(renderH) * sc + 0.5);
    if (ww < 32.0) { ww = 32.0; }
    if (hh < 32.0) { hh = 32.0; }
    *w = static_cast<uint32_t>(ww);
    *h = static_cast<uint32_t>(hh);
}

namespace detail {
// Cheeky's ShapeDistance: lerp(max(|x| / w, |y| / h), length of the same, roundness).
inline float ShapeDistance(float cx, float cy, float w, float h, float roundness)
{
    const float sw = w > 0.0001f ? w : 0.0001f, sh = h > 0.0001f ? h : 0.0001f;
    const float sx = std::fabs(cx) / sw, sy = std::fabs(cy) / sh;
    const float mx = sx > sy ? sx : sy;
    const float len = std::sqrt(sx * sx + sy * sy);
    const float r = roundness < 0.0f ? 0.0f : (roundness > 1.0f ? 1.0f : roundness);
    return mx + (len - mx) * r;
}
inline float Smoothstep(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace detail

// The centre's weight at output pixel (px, py) (pixel index; its centre is px + 0.5) - CompositeMain's: centered in [-1, 1], minus the
// fovea centre (2 cu - 1, 2 cv - 1) (FoveaCentre: Cheeky's offsets already in it); weight = d <= 1 when transition <= 0, else
// 1 - smoothstep(max(0, 1 - transition / min(w, h)), 1, d).
inline float CompositeWeight(float px, float py, uint32_t outW, uint32_t outH, float cu, float cv, const Params& p)
{
    if (outW == 0 || outH == 0) { return 0.0f; }
    float cx = (px + 0.5f) / (0.5f * static_cast<float>(outW)) - 1.0f;
    float cy = (py + 0.5f) / (0.5f * static_cast<float>(outH)) - 1.0f;
    cx -= 2.0f * cu - 1.0f;
    cy -= 2.0f * cv - 1.0f;
    const float d = detail::ShapeDistance(cx, cy, p.width, p.height, p.roundness);
    if (p.transition <= 0.0f) { return d <= 1.0f ? 1.0f : 0.0f; }
    const float mn = p.width < p.height ? p.width : p.height;
    const float nf = p.transition / (mn > 0.0001f ? mn : 0.0001f);
    const float a = 1.0f - nf > 0.0f ? 1.0f - nf : 0.0f;
    return 1.0f - detail::Smoothstep(a, 1.0f, d);
}

// Review F2: the DLSS mode index for a scaling ratio (output / input), nearest match on EnsureUpscale's table - 0 DLAA 1.00,
// 1 Quality 1.50, 2 Balanced 1.72, 3 Performance 2.00, 4 Ultra Performance 3.00 - the first minimum wins a tie (as EnsureUpscale).
// The foveated centre's own ratio includes fov_supersample, so its mode follows it instead of the path's.
inline int NearestModeIndex(double ratio)
{
    static const double kRatio[5] = { 1.00, 1.50, 1.72, 2.00, 3.00 };
    int best = 0;
    double bestErr = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double e = ratio > kRatio[i] ? ratio - kRatio[i] : kRatio[i] - ratio;
        if (e < bestErr) { bestErr = e; best = i; }
    }
    return best;
}

// ---- The adoption rules (plan Task 3). fovdlss::Get (dlssflat.cpp) turns each eval's answer into a Plan with MakePlan and adopts it
// with AdoptStep; the unit test drives the same two functions with scripted slider drags.

// Everything a RECREATE depends on (the centre + periphery features and their textures are sized from it) plus the fovea centre
// that goes with the rectangle. Roundness, transition and the border are NOT here: they are composite-only and apply every frame.
struct Plan {
    lenscrop::Rect in, out;          // the centre's input (render) and output rectangles
    uint32_t privW, privH;           // the centre feature's private output (out x supersample)
    uint32_t workW, workH;           // the periphery's working size
    int peripheryPreset;
    bool periphery;
    float cu, cv;                    // the fovea centre (uv) - FoveaCentre
};

inline bool SamePlan(const Plan& a, const Plan& b)
{
    return lenscrop::SameRect(a.in, b.in) && lenscrop::SameRect(a.out, b.out) && a.privW == b.privW && a.privH == b.privH &&
           a.workW == b.workW && a.workH == b.workH && a.peripheryPreset == b.peripheryPreset && a.periphery == b.periphery &&
           a.cu == b.cu && a.cv == b.cv;
}

// One eye's plan from its geometry (dlvr_FovGeom g12) and the sliders. eye = the history label (the temple side of xOffset).
inline bool MakePlan(const float g[12], int eye, const Params& p, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                     Plan* out)
{
    if (!out) { return false; }
    float u = 0.0f, v = 0.0f;
    if (!LensCentre(g, &u, &v)) { return false; }
    Plan pl{};
    FoveaCentre(u, v, eye, p, &pl.cu, &pl.cv);
    if (!FoveaRects(pl.cu, pl.cv, p, renderW, renderH, outW, outH, &pl.in, &pl.out, &pl.privW, &pl.privH)) { return false; }
    WorkingSize(renderW, renderH, p.peripheryScale, &pl.workW, &pl.workH);
    pl.peripheryPreset = p.peripheryPreset;
    pl.periphery = p.periphery;
    *out = pl;
    return true;
}

// The adopted plan per eye and the candidate waiting to settle (lenscrop::Get's debounce, same rules).
struct Adoption {
    Plan last, pend;
    bool haveLast, pending;
    uint64_t pendSince;
};

// Adopt `cand` under the debounce. Returns true when the ADOPTED plan changed this call (the caller recreates and resets history).
// - the first answer: adopted at once;
// - the adopted plan no longer fits the images (a resolution change): the new one at once - no stale rectangle reaches NGX;
// - otherwise a different answer is adopted only after it has held, unchanged, for settleMs (a slider drag recreates once).
inline bool AdoptStep(Adoption& s, const Plan& cand, uint64_t nowMs, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                      uint64_t settleMs = 300)
{
    const bool same = s.haveLast && SamePlan(cand, s.last);
    const bool fits = s.haveLast && s.last.in.x + s.last.in.w <= renderW && s.last.in.y + s.last.in.h <= renderH &&
                      s.last.out.x + s.last.out.w <= outW && s.last.out.y + s.last.out.h <= outH;
    if (!s.haveLast || (!same && !fits)) {
        s.last = cand;
        s.haveLast = true;
        s.pending = false;
        return true;
    }
    if (same) { s.pending = false; return false; }
    if (!s.pending || !SamePlan(cand, s.pend)) {
        s.pend = cand;
        s.pending = true;
        s.pendSince = nowMs;
        return false;
    }
    if (nowMs - s.pendSince >= settleMs) {
        s.last = cand;
        s.pending = false;
        return true;
    }
    return false;
}

}  // namespace fov
}  // namespace dlvr
