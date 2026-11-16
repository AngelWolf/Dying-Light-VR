// What the launcher window allows and says (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 4, 5, 8,
// 12.8). Pure and header-only - tested by src_beta/src/tests/launcher_view_test.cpp; launcher_ui.cpp draws from it.
#pragma once

#include <algorithm>
#include <string>

#include "launcher_release.h"

namespace dlvr::launcher {

enum class Mode { Vr, Flat, Unknown };

struct LauncherView {
    bool haveGame = false;
    bool gameRunning = false;
    bool busy = false;
    bool programFiles = false;    // refused on every store (D-242)
    bool haveRelease = false;     // a release to install from (local folder or GitHub)
    bool newerRelease = false;    // that release differs from the installed one
    InstallMatch match = InstallMatch::None;
    Mode mode = Mode::Vr;
};

// Nothing is written while the game runs (its DLLs are locked and it saves over our files on exit), while a job runs,
// without a game folder, or on a Program Files folder.
inline bool CanWrite(const LauncherView& v) { return v.haveGame && !v.gameRunning && !v.busy && !v.programFiles; }

inline bool Installed(const LauncherView& v) { return v.match == InstallMatch::Exact || v.match == InstallMatch::Modified; }

inline bool CanLaunchVr(const LauncherView& v) { return CanWrite(v) && Installed(v); }
// Flat works whatever is installed - except an install the launcher did not make: it cannot switch that mod off.
inline bool CanLaunchFlat(const LauncherView& v) { return CanWrite(v) && v.match != InstallMatch::Foreign; }

inline const char* FlatButtonLabel(const LauncherView& v) { return v.mode == Mode::Flat ? "Launch Flat (mod is off)" : "Launch Flat"; }

inline std::string ModeLine(const LauncherView& v)
{
    if (v.mode == Mode::Flat) { return "The mod is OFF (flat). Launch in VR turns it back on."; }
    return Installed(v) ? "The mod is ON (VR)." : "The mod is not installed.";
}

// A newer release is offered to any launcher install, damaged or not: Update re-copies the damaged files too (PlanUpdate's
// differing), and Repair only works from the installed release's own files (2026-11-16, a tester: "Repair" dead-ended on 1.01).
inline bool OffersUpdate(const LauncherView& v) { return Installed(v) && v.newerRelease; }

inline const char* InstallButtonLabel(const LauncherView& v)
{
    switch (v.match) {
        case InstallMatch::None: return "Install";
        case InstallMatch::Foreign: return "Uninstall first";
        case InstallMatch::Modified: return v.newerRelease ? "Update" : "Repair";
        case InstallMatch::Exact: break;
    }
    return v.newerRelease ? "Update" : "Installed";
}

inline Mode ModeFromRecord(const std::string& recordText)
{
    std::string rel, mode, store;
    if (!ParseReleaseRecord(recordText, &rel, &mode, &store)) { return Mode::Unknown; }
    if (mode == "flat") { return Mode::Flat; }
    if (mode == "vr") { return Mode::Vr; }
    return Mode::Unknown;
}

struct FitRect {
    float x = 0, y = 0, side = 0;
    float w = 0, h = 0;   // FitImage
};

// An image of imgW x imgH pixels in an area (pixels): its own shape kept, as large as fits, never above its own size,
// centred. Jay's controls picture is 2560 x 1440 (2026-11-15), not the square first planned.
inline FitRect FitImage(float areaW, float areaH, int imgW, int imgH)
{
    FitRect r;
    if (imgW <= 0 || imgH <= 0 || areaW <= 0 || areaH <= 0) { return r; }
    float s = (std::min)(areaW / static_cast<float>(imgW), areaH / static_cast<float>(imgH));
    s = (std::min)(s, 1.0f);
    r.w = static_cast<float>(imgW) * s;
    r.h = static_cast<float>(imgH) * s;
    r.x = (areaW - r.w) * 0.5f;
    r.y = (areaH - r.h) * 0.5f;
    return r;
}

// A square image of imgPx pixels in an area (pixels): as large as fits, never above its own size, centred.
inline FitRect FitSquare(float areaW, float areaH, int imgPx)
{
    FitRect r;
    if (imgPx <= 0 || areaW <= 0 || areaH <= 0) { return r; }
    r.side = (std::min)((std::min)(areaW, areaH), static_cast<float>(imgPx));
    r.x = (areaW - r.side) * 0.5f;
    r.y = (areaH - r.side) * 0.5f;
    return r;
}

}  // namespace dlvr::launcher
