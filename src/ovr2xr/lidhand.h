// lidhand.h - open top-lid containers by hand, items held until stowed (spec docs/superpowers/specs/2026-11-18-hand-open-lids-
// design.md; plan docs/superpowers/plans/2026-11-18-hand-open-lids-phase2.md; facts RESEARCH-NATIVE-LOOTING §12-§13). The decisions
// are lidhand_logic.h (unit-tested); lidhand.cpp reads the game, writes the lid bone, calls the game's own execute (+B9B930) and
// carries the game's own item objects on the hand (handcarry.h). Trace docs/traces/20261118_1711_hand_open_lids.md.
#pragma once

namespace dlvr {

extern bool  LidByHand;     // lid_by_hand - the feature (needs vr_grip; NOT loot_by_hand - the camouflage poll installs the detour)
extern float LidOpenFrac;   // lid_open_frac - the open point as a fraction of the lid's own open angle (Tier D)

// From looting.cpp's NearbyHook (the HUD trigger update), right after the gaze re-pick, with the game's gathered list and the gaze
// gather's: collects the lid containers / container items, publishes them for LidHandTick, and returns true when the game's
// current pick (player +5272 / +5280) is a lid container or an item a hand carries - the caller then writes the game's own NULL
// pick (no prompt, X has no target). lid_by_hand=0 -> false at one bool test.
bool LidHandOnNearby(void* player, void* const* gameData, unsigned gameCount, void* const* gazeData, unsigned gazeCount);

// Game thread, once per player update (BowDrawTick's top). Grabs, follows, writes, opens, carries, stows. Off = one bool test.
void LidHandTick();

// scePad poll, BEFORE LootPoll (trace 20261119_1240 - a press the lid claims is "owned elsewhere" for loot) and before PushGrabPoll /
// the grip send. leftOwned / rightOwned = a holster / the two-handed hold / the reload took this press (never loot). withholdL1 / withholdR1 come back true while that hand owns a lid / an item, or is next to one with the grip squeezed.
void LidHandPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1);

// The hip yield (trace 20261119_1215, Jay: "We'd have to make it both hip holsters"): true while hand `side` (0 = RIGHT, 1 = LEFT)
// was, at the last game tick, in a published lid's edge zone or an item's zone, or owns a lid / an item. LootYieldAt asks it for both
// hips. lid_by_hand=0 / not ready -> false. COST: one interlocked read.
bool LidHandHandInZone(int side);

// True while hand `side` (0 = RIGHT, 1 = LEFT) carries a lid container's item - from the take to the release (stowed, given back, or
// forgotten). The VR swing's hit is withheld while the right hand carries (trace 20261120_1815, Jay: "a swing can't fire in the hand
// it's being held till it's either dropped or put away"). lid_by_hand=0 -> false. COST: one bool test + one volatile load.
bool LidHandCarrying(int side);

// Jay's approved per-item grip (trace 20261120_2057): the carried mesh's row in lid_item_poses.inc for hand `side` (0 RIGHT, 1 LEFT),
// from the take to the release; -1 = not carrying, or the mesh has no row (today's placement and the game's fingers), or lid_by_hand=0.
// Read by boneblend's LidFingersApply on the finger pass. COST: one bool test + one volatile load.
int LidHandPoseRow(int side);
// The row's data for `side`: 17 pins (LidHandPinCount) - local rotation (3x3 row-major), local translation, the skeleton rest rotation,
// the pin names (r_ / l_) - and the mesh name. Pointers into lidhand.cpp's own table (the only TU that includes it). False = bad row.
bool LidHandPoseData(int row, int side, const float (**pin)[9], const float (**pinT)[3], const float (**rest)[9],
                     const char* const** names, const char** mesh);
int LidHandPinCount();

}  // namespace dlvr
