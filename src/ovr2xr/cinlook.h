// cinlook - a cutscene's wanted look no longer feeds design D's head yaw back into the body (2026-10-25, INV-620 (a); spec
// docs/superpowers/specs/2026-10-25-cutscene-camera-design.md). See cinlook.cpp.
#pragma once

#include <cstdint>

namespace dlvr {

// Swap CinematicController's look interface slot 0 (the wanted look) for ours. Idempotent; camhook's 1 Hz thread. No key - acts only
// while design D's addend is in +4460; found by RTTI name + the slot's own bytes, so every build either arms or refuses by name.
void CinLookInstall();
// True once the slot is ours; *vtRva = the interface vtable's rva (0 when not armed). (c) refuses while this is false.
bool CinLookArmed(uintptr_t* vtRva);
// The cutscene just ended (the look owner left CinematicController) - log its totals now. Game thread (camyaw::Hook's hand-back).
void CinLookReport();

}  // namespace dlvr
