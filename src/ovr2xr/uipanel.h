// MENU PANEL, stage 1 - while a menu is open, the game's menu is shown on a flat panel fixed in the room instead of on
// the eyes (backlog 5 / 6). Design, evidence, unknowns: docs/SPEC-UI-MENU-PANEL-2026-10-11.md. Jay 2026-10-11: "Do the
// spec." -> "1. Sure. 2. That's fine. 3. Slightly below eye level, we'll adjust if needed. 4. That's fine, but it
// shouldn't be an issue".
//
// WHY: the main menu and every in-game menu are drawn inside the one render pass the bootstrap already brackets and
// redirects (INV-531). Handing that capture to the runtime as its own quad makes the runtime hold it still as the head
// moves - whatever way the game draws each element (the native camera rule moved only one box, INV-530).
//
// WHAT, each submit: uipin::MenuState() says whether a menu shows. While one does: the bootstrap is asked for the size of
// an eye's pass (that call is also the handshake that keeps the pass redirected - so the regular HUD lay-on must NOT be
// called, see Active()), the menu's rectangle of eye 0's last pass is drawn into this file's own OpenXR swapchain image
// (DlvrMenuPanelCopy), and a quad is returned for the frame's layer list. The quad's pose is latched ONCE per opening:
// ui_panel_distance_m ahead of the head, ui_panel_height_m above it (negative = below), upright, facing the head.
//
// COST: ui_panel_menus off = one bool test per submit. On, no menu: the level walk (a few dozen pointer reads). On, menu
// open: three OpenXR swapchain calls, one region copy and one small full-screen draw in the bootstrap, one more layer for
// the runtime; the two full-eye HUD lay-on blends are not done.
#pragma once

#include "xrloader.h"
#include "uipanel_logic.h"   // EyeTan / UiCrop - the UI plane from each headset's own frustum (INV-735)

namespace dlvr {

// See the [DEP:] markers at their definitions in uipanel.cpp.
extern bool  UiPanelMenus;
extern float UiPanelDistanceM;
extern float UiPanelHeightM;
extern float UiPanelScale;
extern float UiPanelTabsUpM;
extern float UiPanelTabsXM;
extern bool  UiPanelAllMenus;
// HUD PANEL, stage 1 (docs/SPEC-UI-HUD-PANEL-2026-10-11.md): in play the SAME pass goes on its own room-fixed quad pair.
extern bool  UiHudPanel;
extern float UiHudDistanceM;
extern float UiWheelDistanceM;   // ui_wheel_distance_m - the selection wheel's own placement while open (D13)
extern float UiHudHeightM;
extern float UiHudXM;
extern float UiHudScale;
extern bool  UiHudHide;
// HUD PANEL LAZY FOLLOW (docs/superpowers/specs/2026-10-20-hud-panel-lazy-follow-design.md).
extern bool  UiHudFollow;
extern float UiHudFollowDeg;
extern int   UiHudFollowDelayMs;
extern int   UiHudFollowMs;
extern float UiHudFollowDriftM;

namespace uipanel {

// SELECTION WHEELS (plan 2026-10-27-vr-selection-wheels, spec D12 - Jay: "hand over the wheel as I see it"): the game's item
// wheel as DRAWN on the HUD panel - its centre (the eye image's centre: hud.xui centres the wheels on the canvas, INV-529's UI
// plane is centred on the eye image) and the panel's right / up / normal (toward the viewer) unit axes, in the layer's appSpace,
// from the last HUD submit. extent = the drawn quad's edges measured from the wheel centre along right / up, metres: x lo, x hi,
// y lo, y hi (while a wheel shows, the quad is the wheel's crop). False = no HUD panel shown within 250 ms. Any thread. COST: one
// shared SRW lock + a 16-float copy.
bool WheelFrame(float centre[3], float right[3], float up[3], float normal[3], float extent[4]);

// The frustum the GAME rendered eye `eye` (0 / 1) with - the FOV it echoed in ovrLayerEyeFov at the last submit (ovr2xr.cpp
// g_lmEye[].game; INV-735: every headset differs, nothing is assumed). False until a submit has described that eye. Any thread
// (a few aligned float reads). Defined in ovr2xr.cpp.
bool GameEyeTan(int eye, EyeTan* out);

// Once, when the layer has its device and session (ovr2xr.cpp, beside OverlayInit).
void Init(ID3D11Device* device, XrSession session);
void Shutdown();

// Once per submit, before the frame's layer list is closed. `eyes` = the two eye poses the game image is declared with
// this frame in `appSpace`, `fov0` = eye 0's declared field of view; `viewsValid` false = no usable projection this
// frame (nothing is latched from it). Returns how many of `out[0..1]` are filled and must be submitted: 0 or 2 (v3, the
// stereo panel: out[0] is eye 0's own image for the LEFT eye only, out[1] eye 1's for the RIGHT eye only, same pose).
// v4: `rendered0` = eye 0's orientation as the image was RENDERED (the declared pose before the declared pitch) - the
// panel is latched in that frame and re-expressed in the declared one every submit (uipanel_logic.h Redeclare). Each call
// also keeps the tab-menu patch (gamedll +7AF08D) in step with ui_panel_menus.
// ui_hud_panel: when no menu owns the pass and the in-game HUD is showing, the two quads are the HUD panel's instead.
// fov0 / fov1: each eye's DECLARED frustum (projViews[e].fov) - each quad is sized and centred from its own (EyeQuadOffset,
// trace docs/traces/20261124_1345_panel_eye_offcentre.md); identical and unshifted for a symmetric frustum.
int Render(XrSpace appSpace, bool viewsValid, const XrPosef* eyes, const XrQuaternionf& rendered0, const XrFovf& fov0,
           const XrFovf& fov1, XrCompositionLayerQuad* out, XrTime displayTime);

// True while a panel - the menu's or the HUD's (ui_hud_panel) - owns the bracketed pass: the caller must skip
// DlvrHudLayerSubmit this submit (the pass is going to the panel, not onto the eyes).
bool Active();

// The 3D HUD's own HUD-screen reading (uipin::HudShowing, taken by RenderHud each submit): 1 = the HUD screen is in the play
// level's visible list, 0 = not, -1 = not readable, -2 = not read in the last 100 ms (ui_hud_panel off, a menu panel owns the
// pass, no session). Any thread - the wrist router's stand-down reads it on the render thread. COST: two volatile reads +
// GetTickCount64.
int HudScreenSig();

// ui_hud_panel: latch the HUD panel in front of the head again at the next submit it is shown (the settings panel's
// "Re-centre the HUD panel" button). Does nothing while the HUD panel is off.
void HudRecentre();

// ui_hud_panel: the runtime recentred (XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING - a Meta hold on VDXR, INV-300;
// SteamVR's own recentre raises the same event). The runtime moves LOCAL under the latched pose, so the HUD panel is
// latched again - the button's path - at the first submit whose display time is past `changeTime` + 100 ms. Event-driven:
// called from PollEvents, twice per hold (LOCAL then STAGE), harmless.
void HudRecentreRuntime(XrTime changeTime, int spaceType);

}  // namespace uipanel
}  // namespace dlvr
