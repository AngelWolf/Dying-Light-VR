// Roomscale - docs/superpowers/specs/2026-10-26-roomscale-physical-crouch-design.md §2.2, plan
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 7 (amended: 7a measures, 7b drives).
// 7a: a LOG-ONLY probe of the walk body, its guards, the controller group and the app -> world map (run 4a passed).
// 7b: roomscale_enable - real steps move the character. The game-thread side lives here (the walk body's pre-step detour, the
// measured share of each step that roomscale caused); ovr2xr.cpp's render thread owns the view (roomscale_logic.h Update and the
// appSpace shift) and hands the command over.
#pragma once

namespace dlvr {

extern bool  DiagRoomscaleProbe;   // diag_roomscale_probe - Task 7a's log-only probe. DEFAULT OFF.
extern bool  RoomscaleEnable;      // roomscale_enable - real steps move the character. DEFAULT ON (was OFF, Tier C).
extern float RoomscaleLeanMaxM;    // roomscale_lean_max_m - how far the view may lean past a blocked body

// Game thread, once per player update (boneblend.cpp HookTail, after BodyGrabTick): the 7a probe, and for 7b the player's walk
// body + controller group (published for the detour / render thread) and the pre-step detour's one-time install. Both off: two
// bool tests.
void RoomscaleProbeTick();

// Render thread (ovr2xr.cpp BeginFrameIfNeeded).
bool RoomscaleLive();                          // the pre-step detour is installed and the player's walk body is known
int  RoomscaleGroup();                         // the player's controller group (PlayerDI+3324), -99 = not read
bool RoomscaleOnFoot(int group);               // Jay's ruling 5: 0-6 and 25 move the character, everything else stands down
void RoomscaleTakeMoved(float* wx, float* wz); // world x/z the body moved BECAUSE of roomscale since the last call (consumed)
void RoomscaleSetCommand(float vx, float vz);  // world x/z velocity (m/s) roomscale adds to the body; 0, 0 = none
// Physics-side totals since the last call (consumed), for the [ROOMSCALE] window line.
void RoomscaleTakeStepStats(long* steps, long* commanded, long* guarded, long* gameChanged);

}  // namespace dlvr
