// gunsound.h - the game's own reload sounds, played when the player does each manual-reload step (spec
// docs/superpowers/specs/2026-11-02-solid-guns-reload-sound-haptics-design.md G3; plan Task 5; research
// docs/RESEARCH-SOLID-GUNS-2026-11-02.md steps 1 + 6). Route: our replica of the engine export IGSObject::PlaySound2D on the player's
// IGSObject (PlayerDI + 40) - the object the game's own AnimActionPlaySound2DExternal plays on - bus 4 (EGameVolumeSource::SoundPlayer),
// plus a start position on the handle so each click lands at the step (Jay after RUN SG2: "Do it").
#pragma once

#include <cstdint>

namespace dlvr {

// Game thread (the reload tick). Plays `name` (the script's file name, e.g. "SubMachineA_offload_01.wav") at `vol` after `delayS`
// seconds (0 = now), STARTED `startS` seconds into the sound (the engine seeks there while paused - research step 6). A delayed sound
// is queued (up to 4) and played by GunSoundTick. False = refused / not played (the log says why).
// COST: off (manual_reload=0 never calls it); per sound our replica of the export (NewSound + 7 handle calls) under the engine's own
// sound lock + 9 guarded reads before it.
bool GunSoundPlay(uintptr_t playerDI, const char* name, float vol, float delayS, float startS);

// Game thread, once per reload tick: plays the queued sounds that are due. COST: empty queue = one compare.
void GunSoundTick(uintptr_t playerDI);

}  // namespace dlvr
