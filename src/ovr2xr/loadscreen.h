// LOADING SCREEN VOID - docs/superpowers/specs/2026-10-20-loading-screen-void-design.md (Jay 2026-10-20: "Approved").
//
// WHY: the engine's background path (loading screens and the like, CGame+2496 = IsBackgroundRender) draws a flat picture
// with no camera. Submitted as the game image it is declared at the head's own pose, so it sits stuck to the face and
// steps at the ~32 fps the game manages while loading (Jay: "The image is stuck to my face"). Multiple users reported
// motion sickness from it.
//
// WHAT, while the engine's flag is set: the game image is not submitted (a black void, as the calibration void), every
// release of an eye image is preceded by a copy of it, and each eye's flat-screen HUD box (where the picture sits) goes on
// its own quad at one room pose latched in front of the head when the screen started. No setting: always on; any refusal
// gives the game image back for the rest of the session and says why.
//
// COST, not loading: one call into a two-load engine export per submit, one bool test per eye-image release, one bool
// test in Render. Loading: one full-image GPU copy per release, two rectangle copies and three OpenXR calls per submit.
#pragma once

#include <cstdint>

#include "xrloader.h"

namespace dlvr {
namespace loadscreen {

// Once, when the layer has its device and session (beside uipanel::Init) / at session teardown.
void Init(ID3D11Device* device, XrSession session);
void Shutdown();
// Once per submit, at the top of ovr_SubmitFrame: reads the engine's flag and runs the start / end edges.
void Tick();
// In the submit's eye loop, before the image is released: this OpenXR view's chain and its DXGI format.
void NoteEyeChain(int eye, XrSwapchain chain, int64_t dxgiFormat);
// Immediately before EVERY release of an eye-chain image: keeps a copy of the image while the flag is set.
void CaptureBeforeRelease(XrSwapchain chain, ID3D11Texture2D* image);
// The projection-layer decision: true = do not submit the game image this submit.
bool SkipProjection(bool calibVoid);
// After the menu panel: fills up to two quads (eye-visibility LEFT / RIGHT), returns how many. posesValid = the eye poses
// were LOCATED this frame (the panel never latches from the game's echo pose).
int Render(XrSpace appSpace, bool viewsValid, bool posesValid, const XrPosef eyes[2], const XrFovf& fov0, const XrRect2Di vp[2],
           bool calibVoid, XrCompositionLayerQuad out[2]);

}  // namespace loadscreen
}  // namespace dlvr
