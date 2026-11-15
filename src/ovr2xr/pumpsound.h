// pumpsound.h - silence the game's per-shot rack sound while a manual pump shotgun is the held item.
// Plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md Task 3; spec 2026-11-04-manual-reload-pump-shotgun-design.
// The skip rule is pumpsound_logic.h (unit-tested); this is the gamedll detour on AnimActionPlaySound2DExternal's play slot (+110D890).
#pragma once

namespace dlvr {

// Installs the detour ONCE (the reload Resolve calls it). Returns true when it is live. A false return is final for the session and
// has logged ONE line saying why (the Epic / GOG build has no address for it; the first 16 bytes differ; the write was refused) -
// the pump then keeps the game's own rack sound (it degrades, it never breaks). Further calls return the first result without
// logging. COST: once.
bool PumpSoundInstall();

// The reload tick: true while a manual pump is the held item. COST: one volatile store. Safe from any thread.
void PumpSoundSetSilenced(bool on);

// How many game sounds the detour has dropped so far. COST: one volatile load.
long PumpSoundSkipped();

}  // namespace dlvr
