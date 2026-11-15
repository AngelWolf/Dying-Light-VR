// The launcher window (spec docs/superpowers/specs/2026-11-15-launcher-design.md sections 5 and 9): one borderless
// Dear ImGui / D3D11 window, 1280 x 720 layout units scaled by launcher_size.h, its own drag area and close / minimise
// buttons (no Windows title bar), and the five tabs.
#pragma once

#include <string>

namespace dlvr::launcher {

enum class Tab { Play = 0, Comfort, Config, Controls, Support, Count };

struct UiArgs {
    std::wstring releaseDir;   // --release <dir>: a local release folder in place of GitHub (plan Global Constraints)
    std::wstring gameDir;      // --game-dir <dir>: skip discovery (tests)
    bool afterUpdate = false;  // --after-update: started by the old exe during a self-update; signal it once up
};

int RunUi(const UiArgs& a);

// --shot: one frame of a fixed state rendered off-screen to a PNG. dpiScale and the work area go through WindowSize
// exactly as a real monitor would, so a shot shows the size a user gets. Exit 0 = written.
int RenderShot(const UiArgs& a, const std::string& state, const std::wstring& png, float dpiScale, int workW, int workH);

// One line to <exe dir>\launcher.log (the log zip carries it, spec 11).
void Log(const std::string& line);

}  // namespace dlvr::launcher
