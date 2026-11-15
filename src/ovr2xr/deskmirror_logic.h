#pragma once
// DESKTOP MIRROR, ONE EYE - the pure parts (deskmirror.cpp; trace docs/traces/20261114_1030_desktop_mirror_single_eye.md).
// Jay 2026-11-14: "The desktop window should show what the headset sees" - single eye, no add-ons yet, at most 60 per second.
// No Windows / D3D types here so the unit test (src/tests/deskmirror_logic_test.cpp) builds alone.
#include <cstdint>

namespace dlvr {
namespace deskmirror {

struct RectI { int x, y, w, h; };          // a region of the eye texture, pixels
struct RectF { double x, y, w, h; };       // the source rectangle actually sampled, pixels

// a / (a + b): where the forward direction sits across a frustum with tangents a (left / up) and b (right / down), measured from the
// left / top edge. 0.5 (the rect's centre) when either is not a positive number.
inline double CentreFrac(double a, double b)
{
    if (!(a > 0.0) || !(b > 0.0)) { return 0.5; }
    return a / (a + b);
}

// The largest rectangle of shape `aspect` (width / height) inside `r`, centred at fraction (u, v) of `r` and moved back inside it where
// that centre is too near an edge. `r` is first taken in by half a pixel on each side, so bilinear sampling never reaches the pixel
// beyond the rect (in a double-wide texture, the other eye). aspect not positive = the rect's own shape. A degenerate rect = itself.
inline RectF CropToAspect(const RectI& r, double aspect, double u, double v)
{
    RectF in{ r.x + 0.5, r.y + 0.5, r.w - 1.0, r.h - 1.0 };
    if (!(in.w > 0.0) || !(in.h > 0.0)) { return RectF{ static_cast<double>(r.x), static_cast<double>(r.y),
                                                        static_cast<double>(r.w), static_cast<double>(r.h) }; }
    if (!(aspect > 0.0)) { aspect = in.w / in.h; }
    double w = in.w, h = in.h;
    if (in.w / in.h > aspect) { w = in.h * aspect; } else { h = in.w / aspect; }
    if (!(u >= 0.0 && u <= 1.0)) { u = 0.5; }
    if (!(v >= 0.0 && v <= 1.0)) { v = 0.5; }
    double x = in.x + u * in.w - 0.5 * w;
    double y = in.y + v * in.h - 0.5 * h;
    if (x < in.x) { x = in.x; }
    if (y < in.y) { y = in.y; }
    if (x + w > in.x + in.w) { x = in.x + in.w - w; }
    if (y + h > in.y + in.h) { y = in.y + in.h - h; }
    return RectF{ x, y, w, h };
}

// A deadline clock for "at most N per second": Due(now) is true when now has reached the deadline; the deadline then moves on by one
// period, so the AVERAGE is exactly N per second at any submit rate above N (72 Hz -> 5 of 6 submits, 90 -> 2 of 3, 120 -> 1 of 2).
// The first call is due. Behind by more than one period (a pause, a hitch, the path just switched on) = resynced to now: no burst.
struct RateGate {
    int64_t next = 0;
    bool    started = false;
    bool Due(int64_t now, int64_t period)
    {
        if (period <= 0) { return true; }
        if (!started || now - next > period) {
            started = true;
            next = now + period;
            return true;
        }
        if (now >= next) {
            next += period;
            return true;
        }
        return false;
    }
    void Reset() { started = false; next = 0; }
};

}  // namespace deskmirror
}  // namespace dlvr
