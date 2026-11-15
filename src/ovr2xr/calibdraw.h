// The calibration screen's layout - build A plain shapes (docs/superpowers/plans/2026-10-14-calibration-screen-build-a.md task 4).
#pragma once

#include "calibscreen_logic.h"

namespace dlvr {

// What the panel shows this frame, filled by calibscreen.cpp from the state machine and armfit's progress.
struct CalibView {
    calib::Screen screen = calib::Screen::Closed;
    int posture = 0, pose = 0, pausedArm = -1, vR = 0, vL = 0, nR = 0, nL = 0;
    bool reading = false;
    bool mainMenu = false;    // build B: the raw path at the main menu ("Face forward", ruling B3)
    float ring = 0.0f, skipFrac = 0.0f;
    char resultLine[400] = "";
};

// The panel's layout on an ImGui draw list (passed as void* so this header needs no imgui.h), w x h pixels.
// `view` is a const CalibView*. Render thread, inside an ImGui frame (OverlayRenderCalibQuad).
void CalibDraw(void* drawList, float w, float h, const void* view);
// Build B: the floor grid under the calibration void (OverlayRenderCalibFloor). `user` unused.
void CalibDrawFloor(void* drawList, float w, float h, const void* user);

}  // namespace dlvr
