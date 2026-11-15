// Melee throw (VR gesture) - docs/superpowers/specs/2026-09-30-melee-throw-design.md. Settings live in meleethrow.cpp
// (each with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp next to melee_r2_suppress. No OpenXR types
// here, so boneblend.cpp can include it for ThrowViewPhase alone.
#pragma once

namespace dlvr {

extern bool  ThrowEnable;          // throw_enable           - master. DEFAULT ON.
extern float ThrowHeightY;         // throw_height_y         - READY line, head-yaw frame metres, +Y up
extern float ThrowTrigPress;       // throw_trig_press
extern float ThrowTrigRelease;     // throw_trig_release
extern float ThrowReleaseSpeed;    // throw_release_speed    - m/s forward at the let-go
extern float ThrowSwingHoldoffMs;  // throw_swing_holdoff_ms

// Once per primary WeaponMeleeController slot-16 frame, from swing.cpp DriverFrame after the detector (game thread,
// inside the driver gate). Returns true when the swing must NOT call the hit this frame. throw_enable=0 or not armed
// -> false at the first lines. velValid + (vx, vy, vz) = the detector's velocity this frame, APP space, m/s.
bool ThrowFrame(void* self, const float gripPos[3], const float headPos[3], const float headQ[4], bool poseOk,
                bool velValid, float vx, float vy, float vz, double tMs);

// Once a second from camhook.cpp's install loop: checks the settings, resolves the game functions, logs ARMED /
// NOT ARMED (naming the setting) on every change.
void ThrowTick();

// [VIEWBODY]'s throw buckets (render thread): 1 = READYING/READY now, 2 = within 1.5 s of a THROW call, else 0.
int ThrowViewPhase();

// ovr2xr.cpp: melee_r2_suppress's live value (its global is file-local there).
bool MeleeR2SuppressOn();

}  // namespace dlvr
