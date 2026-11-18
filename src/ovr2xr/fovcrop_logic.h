#pragma once
// SteamVR PANEL CROP (Jay 2026-11-18: "Go ahead", "SteamVR only"; trace docs/traces/20261118_1345_steamvr_panel_crop.md).
//
// A Pimax Crystal Light on SteamVR 2.18.4 (driver "aapvr") showed the WHOLE image doubled with the game's symmetric frustum
// (symmetric_fov=2), and correct while the SteamVR dashboard was open. SteamVR hands each layer to the headset driver with its
// own frustum (openvr_driver.h SubmitLayerPerEye_t: bounds + mProjection); that driver shows it with the PANEL's frustum instead,
// so a layer rendered wider than the panel lands squeezed and pushed outward in each eye. With the dashboard up SteamVR composites
// the layers itself at the panel's frustum - hence correct.
//
// The game keeps its symmetric frustum. This cuts the projection view down to the panel window inside it: the pixel rectangle
// whose edges are the panel's edges rounded OUTWARD (so the whole panel is covered), declared with the exact tangents of those
// pixel edges (so the declaration still describes exactly the pixels - same rays, no resampling). The layer's frustum then IS the
// panel's (to under a pixel), and a driver that ignores it shows it right.
//
// Pure maths, no globals, no OpenXR types: fovcrop_logic_test.cpp compiles this same header with cl.

#include <cmath>

namespace dlvr::fovcrop {

struct Fov { float left, right, up, down; };   // radians, OpenXR signs: left / down negative on a centred eye
struct Rect { int x, y, w, h; };               // pixels; y down from the image's top row (D3D / OpenXR imageRect)

enum Why : int {
    kCropped  = 0,   // rect / fov = the panel window
    kWhole    = 1,   // the panel window is the whole image: nothing to cut (rect / fov = the input)
    kOutside  = 2,   // the panel reaches past the image by more than kTolPx on some edge: not cut (the image does not contain it)
    kBadInput = 3,   // non-finite, empty or inverted frustum / viewport: not cut
};

constexpr double kTolPx = 0.5;

struct Result {
    int  why = kBadInput;
    Rect rect{ 0, 0, 0, 0 };
    Fov  fov{ 0.0f, 0.0f, 0.0f, 0.0f };
    double marginPx[4] = { 0.0, 0.0, 0.0, 0.0 };   // image beyond the panel: left, right, top, bottom (negative = panel past it)
};

// image: the frustum the game rendered into vp. panel: the runtime's own view frustum, in the same frame.
inline Result Crop(const Fov& image, const Fov& panel, const Rect& vp)
{
    Result r;
    r.rect = vp;
    r.fov = image;
    const double iL = std::tan(static_cast<double>(image.left)), iR = std::tan(static_cast<double>(image.right));
    const double iU = std::tan(static_cast<double>(image.up)), iD = std::tan(static_cast<double>(image.down));
    const double pL = std::tan(static_cast<double>(panel.left)), pR = std::tan(static_cast<double>(panel.right));
    const double pU = std::tan(static_cast<double>(panel.up)), pD = std::tan(static_cast<double>(panel.down));
    const bool finite = std::isfinite(iL) && std::isfinite(iR) && std::isfinite(iU) && std::isfinite(iD) && std::isfinite(pL) &&
                        std::isfinite(pR) && std::isfinite(pU) && std::isfinite(pD);
    if (!finite || !(iR - iL > 1e-6) || !(iU - iD > 1e-6) || !(pR - pL > 1e-6) || !(pU - pD > 1e-6) || vp.w <= 0 || vp.h <= 0) {
        r.why = kBadInput;
        return r;
    }
    const double sx = iR - iL, sy = iU - iD;
    const double fx0 = (pL - iL) / sx * vp.w;
    const double fx1 = (pR - iL) / sx * vp.w;
    const double fy0 = (iU - pU) / sy * vp.h;
    const double fy1 = (iU - pD) / sy * vp.h;
    r.marginPx[0] = fx0;
    r.marginPx[1] = vp.w - fx1;
    r.marginPx[2] = fy0;
    r.marginPx[3] = vp.h - fy1;
    if (fx0 < -kTolPx || fx1 > vp.w + kTolPx || fy0 < -kTolPx || fy1 > vp.h + kTolPx) {
        r.why = kOutside;
        return r;
    }
    int x0 = static_cast<int>(std::floor(fx0)), x1 = static_cast<int>(std::ceil(fx1));
    int y0 = static_cast<int>(std::floor(fy0)), y1 = static_cast<int>(std::ceil(fy1));
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > vp.w) { x1 = vp.w; }
    if (y1 > vp.h) { y1 = vp.h; }
    if (x1 - x0 < 1 || y1 - y0 < 1) {
        r.why = kBadInput;
        return r;
    }
    if (x0 == 0 && y0 == 0 && x1 == vp.w && y1 == vp.h) {
        r.why = kWhole;
        return r;
    }
    r.rect = Rect{ vp.x + x0, vp.y + y0, x1 - x0, y1 - y0 };
    r.fov = Fov{ static_cast<float>(std::atan(iL + sx * x0 / vp.w)), static_cast<float>(std::atan(iL + sx * x1 / vp.w)),
                 static_cast<float>(std::atan(iU - sy * y0 / vp.h)), static_cast<float>(std::atan(iU - sy * y1 / vp.h)) };
    r.why = kCropped;
    return r;
}

inline const char* WhyName(int why)
{
    switch (why) {
        case kCropped:  return "CROPPED to the panel window";
        case kWhole:    return "NOT CUT: the panel window is the whole image";
        case kOutside:  return "NOT CUT: the panel reaches past the image (the image does not contain it)";
        default:        return "NOT CUT: bad input (empty / non-finite frustum or viewport)";
    }
}

}  // namespace dlvr::fovcrop
