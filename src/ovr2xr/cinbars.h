// cinbars - no cutscene side bars in VR (2026-10-25, INV-620 (b); spec docs/superpowers/specs/2026-10-25-cutscene-camera-design.md
// ADDENDUM 1 R1). See cinbars.cpp.
#pragma once

namespace dlvr {

// Swap UIGameMovieBackground's and MenuGameMovie's per-update slot 114 (the side bars) for ours. Idempotent; camhook's 1 Hz thread. No key - acts only in
// a VR session; found by RTTI name + the slot's own bytes, so every build either arms or refuses by name.
void CinBarsInstall();

}  // namespace dlvr
