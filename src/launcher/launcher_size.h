// The launcher window's size (spec docs/superpowers/specs/2026-11-15-launcher-design.md section 9).
// Layout: 1280 x 720 units, scaled by the monitor's Windows scaling, capped at 80% of the monitor's work area (the
// screen minus the taskbar), keeping 16:9. Jay 2026-11-15: never fill the screen ("which is what I'm trying to avoid").
// No Windows: tested on its own by src_beta/src/tests/launcher_size_test.cpp.
#pragma once

#include <algorithm>
#include <cmath>

namespace dlvr::launcher {

constexpr int kLayoutW = 1280;
constexpr int kLayoutH = 720;
constexpr float kMaxFraction = 0.8f;

struct WinSize {
    int w, h;
    float scale;   // layout unit -> pixel
};

// dpiScale = the monitor's scaling (1.0 = 100%); workW / workH = its work area in pixels. A non-positive scale counts as
// 1.0, and a non-positive work area applies no cap, so the result is never empty.
inline WinSize WindowSize(float dpiScale, int workW, int workH)
{
    float s = dpiScale > 0.0f ? dpiScale : 1.0f;
    // (std::min) / (std::max): immune to windows.h's min / max macros
    if (workW > 0) { s = (std::min)(s, kMaxFraction * static_cast<float>(workW) / kLayoutW); }
    if (workH > 0) { s = (std::min)(s, kMaxFraction * static_cast<float>(workH) / kLayoutH); }
    if (s <= 0.0f) { s = 1.0f; }
    const int w = (std::max)(1, static_cast<int>(std::floor(kLayoutW * s + 0.5f)));
    const int h = (std::max)(1, static_cast<int>(std::floor(kLayoutH * s + 0.5f)));
    return {w, h, s};
}

}  // namespace dlvr::launcher
