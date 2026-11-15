// WRIST HUD STAGE 2 - the five HUD pieces on quads held on the hands (spec docs/superpowers/specs/2026-11-12-wrist-hud-design.md §5 /
// §7 / §10 stage 2; trace docs/traces/20261112_1810_wrist_stage2_quads.md). Jay 2026-11-12: "Start Stage 2" -> "I pick starting
// values" -> plan "Yes".
//
// WHAT, each submit while ui_wrist_hud was on at launch (STAGE 3a; was the marker dlvr\wrist_hud.txt): the bootstrap's stage 1 router has drawn eye 0's piece draws into
// its pieces target and published each piece's box there (DlvrWristPieceInfo). Every published piece is given the facing fade (1 up to
// 50 deg between the quad's front and the direction to the head, 0 from 75 deg, smooth between; 0 when its hand is not tracked), and
// each one still visible is copied - times the fade - into its own 1024 x 512 slot of ONE swapchain (DlvrWristPieceCopy) and
// submitted as a quad in its hand's GRIP space at the preset-A offset, so the runtime holds it on the controller.
// NEEDS: the bootstrap's router (the same ui_wrist_hud line, read as text at its start), hud_layer (AFW on) + dlss_cbsnap.txt
// (the router's own needs). Placement / size / fade = the ui_wrist_* settings (STAGE 3a; preset A's values by default).
//
// COST: off - one bool test per submit. On - 5 export calls per submit; per visible piece one small GPU copy and one quad;
// 3 OpenXR swapchain calls per submit while any piece is visible.
#pragma once

#include "xrloader.h"

namespace dlvr {

// STAGE 3a SETTINGS (trace docs/traces/20261113_1040_wrist_stage3a_settings.md; spec §6 / §7 / §9). Defined in wristquad.cpp
// with their [DEP:] markers; registered in ovr2xr.cpp kSettings ("VR Comfort" group, drawn on the HUD tab by overlay.cpp).
// Pieces: 0 minimap, 1 health, 2 quests, 3 weapon, 4 quick item (wristleaf.h kPieces).
extern bool  UiWristHud;            // ui_wrist_hud - read ONCE at launch (wristquad::OnThisLaunch); a flip waits for the next launch
extern int   UiWristPreset;         // ui_wrist_preset - 0 A, 1 B (split; spec §6's "C"), 2 Custom
extern float UiWristFadeStartDeg;   // ui_wrist_fade_start_deg
extern float UiWristFadeEndDeg;     // ui_wrist_fade_end_deg
extern float UiWristSize;           // ui_wrist_size - x 0.000115 m per UI px (stage 2's size)
extern int   UiWristHand[5];        // ui_wrist_<piece>_hand - 0 left, 1 right, 2 off (hidden: on neither the eyes nor a hand)
extern float UiWristX[5], UiWristY[5], UiWristZ[5];               // ui_wrist_<piece>_x / _y / _z - metres in the grip frame
extern float UiWristPitch[5], UiWristYaw[5], UiWristRoll[5];      // ui_wrist_<piece>_pitch / _yaw / _roll - degrees
extern float UiWristScale[5];                                     // ui_wrist_<piece>_scale
extern int   UiWristAnchor[5];      // ui_wrist_<piece>_anchor - 0 controller grip, 1 drawn forearm, 2 drawn hand (arm anchor)

namespace wristquad {

// ui_wrist_hud as it was at the FIRST call (the layer's wristprobe::Start, after the settings file is read) - the whole launch.
bool OnThisLaunch();
// Jay 2026-11-14 (HARD): "WRIST HUD HARD REQUIREMENT - HUD ON A FIXED PANEL ON - NEEDS TO TURN ON AUTOMATICALLY OTHERWISE CAN GLITCH
// FOR USERS" (trace docs/traces/20261114_1200_wrist_holds_hud_panel.md). True while the wrist HUD is ticked OR running this launch:
// 'HUD on a fixed panel' (ui_hud_panel) is then held on - ovr2xr.cpp SaveSettingsIfChanged sets it, overlay.cpp DrawOneSetting greys
// its row. Call only after wristprobe::Start (the OnThisLaunch latch's first caller): menu / submit thread.
bool HudPanelNeeded();
// STAGE A (trace docs/traces/20261113_1300_wrist_ingame_stageA.md): the pieces anchored to the drawn forearm / hand, drawn INTO the
// acquired eye image (vpA eye 0, vpB eye 1) after the AFW rebuild and the HUD lay-on, before the release. afw = afw_alt_eye, rebuilt =
// the rebuild ran this submit, held = the held slot, prevIsHeld = the previous submit rendered this held eye (trace
// docs/traces/20261114_1315_wrist_heldprev_proof.md), sep = the eye separation (m). Submit thread.
void DrawInGame(void* eyeTex, const int vpA[4], const int vpB[4], bool afw, bool rebuilt, int held, bool prevIsHeld, float sep);
// FIX ROUND 2 (trace docs/traces/20261113_1330_wrist_ingame_A2.md): eye e's DECLARED pose + FOV this submit (projViews) - the placement
// maps the dropped piece through the eye the headset shows. Submit thread.
void NoteDeclared(int eye, const XrPosef& pose, const XrFovf& fov);
// Preset 0 (A) / 1 (B, the split): copy its 40 placement values into the settings (Apply), or test whether they hold it now (Matches).
void ApplyPreset(int preset);
bool MatchesPreset(int preset);

// STAGE 3b - PLACE BY HAND, POINT-AND-GRAB (traces docs/traces/20261113_1108_wrist_3b_place.md + 20261113_1122_wrist_3b_pointgrab.md;
// Jay after RUN P3B1: "it's hard to actually see the piece that you're placing"). Submit thread only (the overlay's Place button,
// OverlayPoll, Render). Place: the piece appears 0.5 m ahead, upright, fixed in the room, boxed, fully shown (an outline while the game
// draws nothing for it); a beam from each controller. Grip with the beam on it = carry it; let go = it floats there. Trigger (the
// carrier's, or a pointing controller's) = place it on the NEAREST hand: that hand + its pose there become the piece's settings, the
// layout Custom. The menu button cancels.
constexpr int kMaxQuads = 8;        // Render's out[]: 5 pieces + the placement box + 2 beams
enum PlaceResult { kWpPlaced = 0, kWpCancelled = 1, kWpUntracked = 2, kWpTooFar = 3, kWpHandOff = 4, kWpNotOn = 5 };
bool Placing();
int  PlacingPiece();                // -1 none
int  PlaceBegin(int piece);         // -1 = started; else a PlaceResult refusal (hand Off, wrist HUD not on this launch)
int  PlaceTick(bool menuEdge);      // -1 still waiting; else the PlaceResult it ended with
void ResetPiece(int piece);         // its 8 values from the current layout (A / B), or A while Custom

// Once, when the layer has its device and session (beside uipanel::Init) / at session teardown (beside uipanel::Shutdown).
void Init(ID3D11Device* device, XrSession session);
void Shutdown();

// Once per submit, after the grapple reticle. eyes = the two declared eye poses in appSpace (viewsValid false -> the head pose is
// read from GetHeadPose). room = how many layers may still be appended. Fills up to kMaxQuads quads, returns how many.
int Render(XrSpace appSpace, bool viewsValid, const XrPosef eyes[2], int room, XrCompositionLayerQuad out[kMaxQuads]);

}  // namespace wristquad
}  // namespace dlvr
