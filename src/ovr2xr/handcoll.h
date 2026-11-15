// handcoll.h - live-enemy hand collision (spec docs/superpowers/specs/2026-10-30-live-enemy-hand-collision-design.md, plan
// docs/superpowers/plans/2026-10-30-live-enemy-hand-collision.md Task 3). An empty hand (fists included) stops on a live grapple-able
// enemy's body - its own ragdoll shapes rebuilt on its live bones - with one controller buzz when contact begins. Nothing is sent to
// the game. The shapes and the stop are handcoll_logic.h (unit-tested).
#pragma once

namespace dlvr {

extern bool  HandCollision;          // hand_collision - master. DEFAULT ON (was OFF, Tier C).
extern float HandCollisionBuzzAmp;   // hand_collision_buzz_amp - the contact pulse's strength 0..1
extern float HandCollisionBuzzMs;    // hand_collision_buzz_ms  - its length, ms
extern float PushGrabReachM;         // push_grab_reach_m - a fingertip this close to a body surface is a contact for the push / grab

// Game thread, once per player update, in HookTail BEFORE the tail solve (INV-661: the enemies are animated for this update by then).
// `ent` = HookTail's PlayerFppVis. Tracks the nearby enemies (a level query <= 10 Hz) and poses their shapes for this update.
void HandCollisionTick(void* ent);

// Inside SolveArmIk, before the reach clamp: arm 0 = right, 1 = left; S = the shoulder, T = the wrist target (game world); tipOff =
// wrist -> fingertip (the solve's wristBack + the fingers along handAxis), so the HAND's front stops on the surface, not the wrist.
// handEmpty = the caller's "nothing in that hand" (fists = empty). True = T was moved (by the same shift as the fingertip). Acts only
// on the thread that ran the tick (the pad-poll fallback solve gets no stop).
bool HandCollisionStop(int armIdx, bool handEmpty, const float S[3], const float tipOff[3], float T[3]);

// The open-hand push and grab (spec 2026-10-31-open-hand-push-and-grab-design, plan Task 3): the contact the last tail solve of THIS
// player update made with arm `armIdx` (0 = right, 1 = left). `point` = where the fingertip meets the surface, `outwardNormal` = the
// touched shape's surface normal there (body -> hand), `realVel` = the REAL fingertip's velocity (before the stop, game world, from
// successive tail solves; zero until two solves are seen), `bodyIndex` = the ragdoll body the touched shape pairs with (the kBones
// order = ragdoll_ai.phx Bones() order, SHAPECHECK's pairing), `player` = the PlayerDI the tick read. Game thread only.
struct HandContact {
    void* ai;
    void* player;
    int aiId;
    int bodyIndex;
    float point[3];
    float outwardNormal[3];
    float realVel[3];
    double tS;
    float gapM;   // 0 = the fingertip reached the surface (the stop); > 0 = this far short of it, within push_grab_reach_m
};
bool HandCollisionContact(int armIdx, HandContact* out);
// Any thread: the last solve's smallest gap from that arm's empty-hand fingertip to a tracked body surface (0 = touching) and when
// (NowS) - for the push / grab's miss line. False = none measured yet (no shapes, hand holding something, or no solve).
bool HandCollisionNearest(int armIdx, float* gapM, double* atS);
// Game thread: is the contact's enemy still the live, standing, tracked object the engine knows by its id (the hand collision's own
// per-update checks: FindObjectByID through the player, vftable, IsDead, no ragdoll)? A fault or any doubt = false.
bool HandCollisionRevalidate(const HandContact& c);
// The grab (plan Task 4), game thread: the enemy's state by the same checks - -1 = gone (the id lookup no longer returns it, not a
// HumanAI any more, or a fault), 0 = alive and standing (no ragdoll), 1 = alive with its ragdoll up (fallen), 2 = dead.
int HandCollisionAiState(void* ai, int id, void* player);
// Game thread: arm `armIdx`'s real fingertip velocity (game world, m/s) from the last tail solves; false = none yet.
bool HandCollisionArmVelocity(int armIdx, float out[3]);
// Game thread: arm `armIdx`'s real fingertip (game world) at the last tail solve; false = none yet.
bool HandCollisionArmTip(int armIdx, float out[3]);
// Any thread: the PlayerDI the last tick read (null before the first tick / while hand_collision is off) - one pointer-sized load.
void* HandCollisionPlayer();
// Any thread: IGSObject::GetID of an object whose IGSObject is at +40 (PlayerDI / HumanAI). False = not resolved yet or a fault.
bool HandCollisionObjectId(void* obj, int* out);

}  // namespace dlvr
