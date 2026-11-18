// lidhand.h - open top-lid containers by hand, items held until stowed (spec docs/superpowers/specs/2026-11-18-hand-open-lids-
// design.md; plan docs/superpowers/plans/2026-11-18-hand-open-lids-phase2.md; facts RESEARCH-NATIVE-LOOTING §12-§13). The decisions
// are lidhand_logic.h (unit-tested); lidhand.cpp reads the game, writes the lid bone, calls the game's own execute (+B9B930) and
// carries the game's own item objects on the hand (handcarry.h). Trace docs/traces/20261118_1711_hand_open_lids.md.
#pragma once

namespace dlvr {

extern bool  LidByHand;     // lid_by_hand - the feature (needs loot_by_hand: the shared GetNearbyTriggers detour)
extern float LidOpenFrac;   // lid_open_frac - the open point as a fraction of the lid's own open angle (Tier D)

// From looting.cpp's NearbyHook (the HUD trigger update), right after the gaze re-pick, with the game's gathered list and the gaze
// gather's: collects the lid containers / container items, publishes them for LidHandTick, and returns true when the game's
// current pick (player +5272 / +5280) is a lid container or an item a hand carries - the caller then writes the game's own NULL
// pick (no prompt, X has no target). lid_by_hand=0 -> false at one bool test.
bool LidHandOnNearby(void* player, void* const* gameData, unsigned gameCount, void* const* gazeData, unsigned gazeCount);

// Game thread, once per player update (BowDrawTick's top). Grabs, follows, writes, opens, carries, stows. Off = one bool test.
void LidHandTick();

// scePad poll, right after LootPoll and before PushGrabPoll / the grip send. leftOwned / rightOwned = an earlier poller took this
// press. withholdL1 / withholdR1 come back true while that hand owns a lid / an item, or is next to one with the grip squeezed.
void LidHandPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1);

}  // namespace dlvr
