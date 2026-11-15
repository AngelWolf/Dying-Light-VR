// Grab bodies by hand - docs/superpowers/specs/2026-10-23-body-grab-design.md (plan docs/superpowers/plans/2026-10-23-body-grab.md,
// Task 3). Squeeze a grip on a dead body away from its torso (or on the torso and move): the ragdoll part under the hand follows the
// hand, the rest drags; let go and it keeps its swing. The decision is loot by hand's machine (looting_logic.h, State::Dragging); the
// pull is bodygrab_logic.h's SpringForce (unit-tested); this file resolves the body in the game and applies the force.
//
// HAND INDEX: 0 = LEFT, 1 = RIGHT - looting.cpp's convention. (ITEMHIDE's WantHide uses the opposite: 0 = right.)
// The kSettings rows are in ovr2xr.cpp beside the loot_* rows (plan Task 5). body_grab needs loot_by_hand=1: the bodies come from loot
// by hand's gather and the press from its machine.
#pragma once

namespace dlvr {

extern bool  BodyGrab;           // body_grab - master. DEFAULT ON (was OFF, Tier C).
extern float BodyGrabReachM;     // body_grab_reach_m - hand to the body's box for a grab away from the torso
extern float BodyGrabHz;         // body_grab_hz - the spring's natural frequency
extern float BodyGrabDamping;    // body_grab_damping - damping ratio (1 = critical)
extern float BodyGrabMaxAccel;   // body_grab_max_accel - m/s^2, the force cap per kg ("a bit of weight")
extern float BodyGrabLimbMaxAccel;    // body_grab_limb_max_accel - the same cap for a severed limb only
extern float BodyGrabLimbTurnHz;      // body_grab_limb_turn_hz - the severed-limb rotation hold's stiffness
extern float BodyGrabLimbTurnDamping; // body_grab_limb_turn_damping - its damping ratio
extern float BodyGrabHitboxMarginM;   // body_grab_hitbox_margin_m - how far outside a part's hitbox a grip still takes it
extern bool  EnemyGrabStiffHold;      // enemy_grab_stiff_hold - a held live enemy's ragdoll locked + weightless (spike, DEFAULT ON)
extern float EnemyGrabStiffForce;     // enemy_grab_stiff_force - the locked joint motors' max force
extern float EnemyGrabWeight;         // enemy_grab_weight - the share of its weight a held stiff live enemy keeps (0..1)

// Pad thread (LootPoll), on the machine's DRAG event: queue a grab of `ai` at the hand's world point. Returns false and logs why when
// refused here (body_grab off, bad hand, no body); every game-side refusal happens in BodyGrabTick and is logged there.
bool BodyGrabBegin(int hand, void* ai, const float handWorld[3]);
// The open-hand push and grab (plan 2026-10-31-open-hand-push-and-grab Task 4), game thread: queue a grab of a LIVE enemy the game
// has knocked down (its ragdoll up) at the hand's world point - no "must be dead" test, no ragdoll restart (refused when none is up).
// Its END is BodyGrabEnd, or the enemy's ragdoll ending (it gets up). False = refused here (body_grab off, bad arguments).
bool BodyGrabBeginLive(int hand, void* ai, const float handWorld[3]);
// Pad thread, on any END of a drag: stop pushing (the body keeps the velocity it has - the fling). Never sets a velocity.
void BodyGrabEnd(int hand, const char* why);
// Game thread, once per player update: the player-update tail (boneblend.cpp HookTail), after the frame's physics step
// (RESEARCH-BODY-GRAB "Build constants" (a2)-(a4)) - a force added here lands in the next step.
void BodyGrabTick();
// AFW item 5: the NPC draw ranges learned so far, triples (index count, start index, base vertex). Exported as dlvr_AfwNpcRanges.
int NpcRanges(unsigned* out, int maxTriples, long* gen);
// Any thread: this hand holds a body now (START seen by the game thread, no END yet). For the weapon hide and the attack blocks.
bool BodyGrabHandActive(int hand);
// The camouflage gesture (camogesture.cpp, plan 2026-11-07-camouflage-gesture). Game thread: the corpse AI or severed limb the hand
// holds now (nullptr = none, or a LIVE knocked-down enemy - never a corpse); *relict = it is a severed limb.
void* BodyGrabHeldObject(int hand, bool* relict);
// Any thread: an object's engine id (IGSObject at +40). False = not resolved yet / fault.
bool BodyGrabObjectId(void* obj, int* id);
// Game thread: is `obj` still the engine's live object with `id` (the engine's id lookup; obj's memory is not read)?
bool BodyGrabObjectAlive(void* obj, int id);
// Any thread: the PlayerDI last published by the HUD hook (nullptr before the first).
void* BodyGrabPlayer();
// One measured ragdoll hitbox (handcoll.cpp SHAPECHECK): its body index, shape 1 sphere / 2 capsule / 3 box, world centre, rotation
// quaternion (x y z w, the engine's order as body grab reads it) and its fields (sphere r / capsule r + cylinder length along local Z
// / box full sides).
struct BodyGrabHitbox { int body; int shape; float c[3]; float q[4]; float prm[3]; };
// The AI's ragdoll hitboxes as body grab measures them, up to cap. Returns the count, or -1 body_grab=0, -2 body grab not READY,
// -3 no ragdoll (or a fault), -4 bad arguments. Game thread only.
int BodyGrabRagdollHitboxes(void* ai, BodyGrabHitbox* out, int cap);
// Pad thread, every poll the hand is mapped: the hand's world point and its time (looting.cpp NowS, QPC seconds), seqlocked. Tick
// drives toward the newest sample and derives the hand's velocity from successive ones.
// `rot` = the hand's world rotation (x, y, z, w) - the severed-limb rotation hold turns the limb with it.
void BodyGrabPublishHand(int hand, const float world[3], const float rot[4], double tS);
// Pad thread, every poll: the gathered body nearest the hand (null = none in reach). BodyGrabTick (game thread) reads that body's
// skeleton under the hand at most every 50 ms - the bone reads stay on the game thread.
void BodyGrabPublishCandidate(int hand, void* body);
// Pad thread, at a press: 1 = the hand is on a torso bone (spine* / pelvis) of `body`, 0 = on another bone, -1 = not known (no
// fresh reading for that body - the caller keeps its own torso rule).
int BodyGrabTorsoOf(int hand, void* body);
// Severed limbs (AIRelict). Pad thread, every poll: the hand's grip squeeze 0..1 - the game thread looks for severed limbs near
// the hand only while it is >= 0.2 (reaching to grab). Any thread: the player object (looting's GetNearbyTriggers detour) - the
// level query's ILevel comes from it.
void BodyGrabPublishSqueeze(int hand, float squeeze);
void BodyGrabPublishPlayer(void* player);
// Pad thread: the severed limb nearest the hand from a fresh game-thread reading (null = none), and its distance in metres.
void* BodyGrabRelictOf(int hand, float* distM);
// Any thread: is `obj` a severed limb (AIRelict vftable)?
bool BodyGrabIsRelict(void* obj);
// Pad thread: the hand's distance to the published candidate `body` from a fresh game-thread reading - its nearest hitbox's surface
// (0 inside) when its ragdoll is up, else its bone line; -1 = no fresh reading for that body. The measure a severed limb is
// compared against (the limb's own distance is its own hitbox's surface).
float BodyGrabHitOf(int hand, void* body);
// Pad thread: true = this squeeze reached the severed-limb search's threshold but no search has reported since - a press now
// should wait for it (looting_logic Params.lateArmS). False when the search is not available or not due.
bool BodyGrabRelictPending(int hand);
// Pad thread, on a grip press edge: ask the game thread for a fresh severed-limb query at once (catching a thrown limb - a
// reading up to 100 ms old misses it); BodyGrabRelictPending stays true until one at least this new has reported.
void BodyGrabRequestRelict(int hand);

}  // namespace dlvr
