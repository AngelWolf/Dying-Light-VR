// The calibration screen - build A (docs/superpowers/plans/2026-10-14-calibration-screen-build-a.md) + build B (the main-menu
// raw path, the auto-open, the void + floor - docs/superpowers/plans/2026-10-14-calibration-screen-build-b.md). Opened from the
// mod menu's "Calibrate my arms" or by itself at the main menu; the decisions are calibscreen_logic.h.
#pragma once

#include "xrloader.h"

namespace dlvr {

extern int CalibPosture;          // calib_posture: 0 standing, 1 seated (saved by the screen)
extern bool CalibScreenAuto;      // calib_screen_auto: open by itself at the first main-menu frame with no calibration
void CalibScreenOpen();           // the overlay's "Calibrate my arms", or the auto-open
bool CalibScreenActive();         // one volatile read - the input policy and the overlay toggle test it
// Once per submit, submit thread, BEFORE the projection layer is added: the once-per-launch auto-open, the void flag, the
// floor space. COST once the auto-open is decided and the screen is closed: one bool test + one volatile read.
void CalibScreenFrameBegin(XrSession session, XrSpace appSpace);
// True this submit = the screen is up at the main menu: do NOT submit the game image (build B ruling B4).
bool CalibScreenVoid();
// Once per submit, after OverlayRender: ticks the flow (and takes the raw sample) and fills `out` with the panel.
bool CalibScreenRender(XrSpace viewSpace, XrSpace appSpace, XrTime time, XrCompositionLayerQuad* out);
// The floor under the void (only while CalibScreenVoid()). False = nothing to submit.
bool CalibScreenRenderFloor(XrCompositionLayerQuad* out);
// The void's walls (4 quads) and ceiling (1) from Jay's videos (2026-10-16), only while CalibScreenVoid(). Fills up to `max`
// quads in `out`, returns how many (0 = nothing; the walls need 4 slots, they are skipped whole if fewer are offered).
int CalibScreenRenderVoidBox(XrCompositionLayerQuad* out, int max);

}  // namespace dlvr
