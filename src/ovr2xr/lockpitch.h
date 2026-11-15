// lockpitch - lockpick position P1 (2026-10-25, spec docs/superpowers/specs/2026-10-25-lockpick-position-p1-design.md; Jay:
// "P1 first." -> "Yes. Both builds are neccessary"). The lockpick look asks for the HEAD's pitch, so the game places the lock where
// the player is looking. See lockpitch.cpp.
#pragma once

namespace dlvr {

// Swap the lockpick look interface's slot 0 (Look_GetWantedDev) for ours. Idempotent; camhook's 1 Hz thread. No key - always on;
// Steam and the Epic build (buildrva family 9); anything else refuses by name.
void LockPitchInstall();

}  // namespace dlvr
