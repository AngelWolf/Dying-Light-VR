// lenscrop_logic.h - the DLSS / FSR lens crop's row band. Header-only, no Windows headers: the bootstrap (dlssflat.cpp) uses it,
// and tests/lenscrop_logic_test.cpp checks it against the lens mask's own Visible() predicate.
// Design: docs/superpowers/specs/2026-11-11-dlss-lens-crop-design.md §4.1. Plan: docs/superpowers/plans/2026-11-11-dlss-lens-crop.md.
//
// The geometry g[12] is the layer's dlvr_LensGeom / dlvr_LensMask layout (ovr2xr.cpp): 0-3 the image's own tangents L R U D (the FOV
// declared for it, all positive), 4-7 the displayable tangents L R U D in the eye's frame with the margin added, 8 cos(pitch),
// 9 sin(pitch) (declared pose = eye x Rx(pitch)), 10 the margin in degrees, 11 the pitch in degrees.
//
// Only ROWS are cropped (design §3: the nose-side columns rebuild the held eye's outer edge under AFW).
#pragma once

#include <cmath>
#include <cstdint>

namespace dlvr {
namespace lenscrop {

struct Band { float v0; float v1; };   // uv rows, 0 = top; rows [v0, v1] can be displayed; 0 <= v0 < v1 <= 1
struct Rect { uint32_t x; uint32_t y; uint32_t w; uint32_t h; };

inline bool Finite(float f) { return std::isfinite(f); }

// The displayable rows, closed form. In Visible() (dlssflat.cpp, namespace lensmask) the declared-frame direction (tx, ty, -1) is
// turned by Rx(pitch) into the eye's frame: y2 = ty c + s, z2 = ty s - c, so vy = y2 / -z2 = (ty c + s) / (c - ty s) - independent
// of tx, and increasing in ty while c - ty s > 0. Inverted: ty = (vy c - s) / (c + vy s). Every row in the band has a visible
// column (hx = 0 is inside it), so the band is exactly the displayable rows.
inline bool VisibleRows(const float g[12], Band* out)
{
    if (!g || !out) { return false; }
    for (int i = 0; i < 10; ++i) { if (!Finite(g[i])) { return false; } }
    for (int i = 0; i < 8; ++i) { if (!(g[i] > 0.0f)) { return false; } }
    const double c = g[8], s = g[9];
    if (std::fabs(c * c + s * s - 1.0) > 1e-3 || !(c > 0.0)) { return false; }   // a rotation, pitch within +-90 deg
    const double imgU = g[2], imgD = g[3], visU = g[6], visD = g[7];
    // top: vy = +visU. A denominator <= 0 = the bound is beyond the horizon in that direction - nothing cropped on that side.
    double v0 = 0.0, v1 = 1.0;
    const double dTop = c + visU * s;
    if (dTop > 1e-9) {
        const double tyTop = (visU * c - s) / dTop;
        v0 = (imgU - tyTop) / (imgU + imgD);
    }
    const double dBot = c - visD * s;
    if (dBot > 1e-9) {
        const double tyBot = (-visD * c - s) / dBot;
        v1 = (imgU - tyBot) / (imgU + imgD);
    }
    if (v0 < 0.0) { v0 = 0.0; }
    if (v1 > 1.0) { v1 = 1.0; }
    if (!(v0 < v1)) { return false; }
    out->v0 = static_cast<float>(v0);
    out->v1 = static_cast<float>(v1);
    return true;
}

inline Band Union(Band a, Band b) { return Band{ a.v0 < b.v0 ? a.v0 : b.v0, a.v1 > b.v1 ? a.v1 : b.v1 }; }

// The pixel rectangles for one evaluation: full width, rows snapped OUTWARD. Output rows cover the band at the output size; the input
// rows are the outward cover of those at the render size; the output rows are then re-derived from the input rows by the ratio (and
// stay outward), so both rectangles describe the same region: |out.y - in.y * s| <= 1, |out.h - in.h * s| <= 2, s = outH / renderH.
// false = do not crop (a zero size, or the result would cover every row of either image).
inline bool SnapRows(Band b, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, Rect* in, Rect* out)
{
    if (!in || !out || renderW == 0 || renderH == 0 || outW == 0 || outH == 0) { return false; }
    if (!(b.v0 >= 0.0f) || !(b.v1 <= 1.0f) || !(b.v0 < b.v1)) { return false; }
    const double s = static_cast<double>(outH) / static_cast<double>(renderH);
    double oy0 = std::floor(static_cast<double>(b.v0) * outH);
    double oy1 = std::ceil(static_cast<double>(b.v1) * outH);
    if (oy0 < 0.0) { oy0 = 0.0; }
    if (oy1 > outH) { oy1 = outH; }
    double iy0 = std::floor(oy0 / s);
    double iy1 = std::ceil(oy1 / s);
    if (iy0 < 0.0) { iy0 = 0.0; }
    if (iy1 > renderH) { iy1 = renderH; }
    double ry0 = std::floor(iy0 * s + 1e-9);
    double ry1 = std::ceil(iy1 * s - 1e-9);
    if (ry0 > oy0) { ry0 = oy0; }   // never inward of the band
    if (ry1 < oy1) { ry1 = oy1; }
    if (ry0 < 0.0) { ry0 = 0.0; }
    if (ry1 > outH) { ry1 = outH; }
    const uint32_t inY = static_cast<uint32_t>(iy0), inEnd = static_cast<uint32_t>(iy1);
    const uint32_t outY = static_cast<uint32_t>(ry0), outEnd = static_cast<uint32_t>(ry1);
    if (inEnd <= inY || outEnd <= outY) { return false; }
    if ((inY == 0 && inEnd == renderH) || (outY == 0 && outEnd == outH)) { return false; }
    *in = Rect{ 0, inY, renderW, inEnd - inY };
    *out = Rect{ 0, outY, outW, outEnd - outY };
    return true;
}

inline double CroppedShare(const Rect& r, uint32_t W, uint32_t H)
{
    if (W == 0 || H == 0) { return 0.0; }
    return 1.0 - (static_cast<double>(r.w) * r.h) / (static_cast<double>(W) * H);
}

inline bool SameRect(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

}  // namespace lenscrop
}  // namespace dlvr
