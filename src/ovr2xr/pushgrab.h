// pushgrab.h - the open-hand push and grab on live enemies, one feature (spec docs/superpowers/specs/2026-10-31-open-hand-push-and-grab-
// design.md, plan docs/superpowers/plans/2026-10-31-open-hand-push-and-grab.md). An empty hand in contact with a standing hostile
// enemy (handcoll.cpp's contact) shoves it through the game's own hit (+DC7190 with the item Melee_CrowdBump - the game's own body
// bump) in the direction the hand moves, and a really hard shove also knocks it down the way the game's own PUSH handler does
// (SetShouldFall when the enemy's influence for PUSH is non-zero). The decisions are pushgrab_logic.h (unit-tested).
// The GRAB (plan Task 4; Jay: "Yes, build it that way"): squeezing the grip on a contact asks the game to knock the enemy down
// (SetShouldFall - the only native way a live enemy goes limp, RESEARCH §9 (c) / (c2)), waits up to 0.5 s for its ragdoll, then body
// grab's spring holds the ragdoll at the hand; letting go throws it with the hand's velocity through the game's own ragdoll velocity
// (HumanAI slot 307); the game gets it up. A break-free timer ends the hold. Single-player only: an enemy the engine replicates
// (co-op) is refused, named.
#pragma once

namespace dlvr {

extern bool  EnemyPushGrab;      // enemy_push_grab - master. DEFAULT ON (was OFF, Tier C). Needs hand_collision + body_grab.
extern float PushMinMps;         // push_min_mps - below this horizontal hand speed a contact does not push
extern float PushKnockdownMps;   // push_knockdown_mps - at or above it the shove also knocks the enemy down
extern float PushRearmS;         // push_rearm_s - a second push in the same contact after this long
extern float PushHardMps;        // push_hard_mps - at or above it a shove is the game's medium hit, below it the light one
extern float PlayerAiHelperRadiusM;   // player_ai_helper_radius_m - the player's AwayFromAIHelper XZ radius (the game's 0.6)
extern float GrabBreakFreeS;     // grab_break_free_s - a held enemy breaks free after this long
extern float GrabDropMps;        // grab_drop_mps - let go slower than this = dropped, faster = thrown
extern float PushGrabIntentSqueeze;   // push_grab_intent_squeeze - a hand squeezed this much when it would shove does not shove
extern float PushZombieMeleeRangeM;   // push_zombie_melee_range_m - ordinary zombies walk in to this melee range against the player

// Game thread, once per player update: HookTail AFTER the tail solve (the contacts are this update's). Off: one bool test.
void PushGrabTick();
// Pad thread (ovr2xr.cpp, after the loot / camouflage polls): the grab's grip machine. leftOwned / rightOwned = a press another
// feature took first (holster, two-hand, loot) - never a grab. withholdL1 / withholdR1 come back true from the press that starts a
// grab to its end, for that hand only (left grip = L1, right grip = R1). Off / not READY: both false at one bool test.
void PushGrabPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1);
// Any thread: this hand (body grab's index: 0 = LEFT, 1 = RIGHT) is grabbing a live enemy (the press seen, not ended).
bool PushGrabHandActive(int bodyGrabHand);
// Any thread: 1 = single player, 0 = co-op, -1 = unreadable (the grab's own session test; used by pumpsound's co-op fallback).
int PushGrabSessionSolo();

}  // namespace dlvr
