// handcarry.h - a GAME model object carried on a player element (spec docs/superpowers/specs/2026-10-22-hand-carried-model-design.md,
// plan docs/superpowers/plans/2026-10-22-hand-carried-model.md Task 3; facts INV-613). The engine carries it: take = save the
// object's own local, attach it keep-local to the element, set our local; give back = restore that local and re-attach it where it
// came from. Nothing is written per frame. The decisions are handcarry_logic.h. First user: the bow's carried arrow (bownock.cpp).
// Game thread only.
#pragma once

namespace dlvr {

struct HandCarry {
    void* obj = nullptr;                 // the IModelObject* carried (its IControlObject is obj + 24, INV-402)
    void* homeModel = nullptr;           // where it came from
    int   homeElem = -1;
    float homeLocal[12] = {};            // its LOCAL as found - restored before it goes home (the engine's attach keeps the local)
    void* onModel = nullptr;             // where it is now
    int   onElem = -1;
    float appliedRel[12] = {};           // the relation the current local was built with (any slider change -> SetLocal)
    bool  active = false;
    long  takes = 0, giveBacks = 0, forgets = 0, retakes = 0;
    // A WORLD carry (HandCarryTakeWorld - plan 2026-11-20-world-items-by-hand, trace 20261120_2210): the object hangs off no element,
    // so its home is its WORLD matrix as found; gsoId = its IGSObject id for the "still the live object" check (INV-663).
    bool  world = false;
    float homeWorld[12] = {};
    int   gsoId = -1;
    bool  idOk = false;
};

// The four engine exports, resolved once by name. False = refused (logged once, naming the missing export). COST: one bool test
// after the first call.
bool HandCarryReady();
// `rel` = where the object's HOME element should sit in onElem's frame; the object's local on the hand = rel . its own local L
// (handcarry::ComposeLocal), so it is drawn exactly as it was on its home element, moved by rel (final review 2026-10-22 (4)).
// Save obj's local L, attach it keep-local to onModel's onElem, set rel . L. Refuses (false, nothing changed) unless obj's current
// parent element is homeElem - the check that obj really is the object the owner thinks it is. COST: 4 export calls, once.
bool HandCarryTake(HandCarry& c, void* obj, void* homeModel, int homeElem, void* onModel, int onElem, const float rel[12], float d);
// HandCarryTake, but the object is DRAWN at onElem x `drawn` whatever its own local L is: reads L, rel = drawn x L^-1
// (handcarry::RelForDrawn - a GENERAL 3x3 inverse, the crossbow's bolt_pose carries scale 3.22526, INV-676), then exactly
// HandCarryTake's path with that rel (same parent refusal; c.appliedRel = rel, so RelChanged / SetLocal / Reattach stay rel-based).
// A singular L refuses (false, nothing changed; logged once). First user: the crossbow bolt on l_hand (crossbow plan Task 3 / 5).
// COST: 4 export calls + one 3x3 inverse, once per take (an event) - never per frame.
bool HandCarryTakeAs(HandCarry& c, void* obj, void* homeModel, int homeElem, void* onModel, int onElem, const float drawn[12]);
// The carried object's parent element == c.onElem. COST: one export call (per update while carrying).
bool HandCarryStillOn(const HandCarry& c);
// The game moved it off the hand (INV-613 F5): attach it back with rel . homeLocal; homeLocal is kept. COST: 3 export calls, per event.
bool HandCarryReattach(HandCarry& c, const float rel[12]);
// A new relation while carried (a slider moved): rel . homeLocal. COST: one export call, only on a change.
void HandCarrySetLocal(HandCarry& c, const float rel[12]);
// The relation differs from the one applied (exact compare - the sliders are the only source of change).
bool HandCarryRelChanged(const HandCarry& c, const float rel[12]);
// Restore homeLocal, attach to homeModel / homeElem keep-local, read the parent back. COST: 3 export calls, once.
bool HandCarryGiveBack(HandCarry& c, const char* why);
// The owner is gone or the object changed: drop the pointer and call NOTHING on it (the bow destroys its arrow, INV-613 F7).
void HandCarryForget(HandCarry& c, const char* why);
// The object's world translation (its IModelObject element 0) through the rig's world accessor - the self-check's drawn nock.
bool HandCarryObjectOrigin(const HandCarry& c, float out[3]);

// ---- the WORLD carry (plan 2026-11-20-world-items-by-hand Task 4; trace 20261120_2210; INV-758). Its five engine exports are resolved
// in HandCarryReady but are NOT in its refusal list: a missing one makes only these three return false (named once), the element carry
// (bow, crossbow, chest items) is untouched. Game thread only. ----
// obj must hang off NO mesh element (GetParentMeshElement < 0). Latch its world matrix (homeWorld) and IGSObject id, attach it to
// onModel / onElem and set its local = `drawn`. False = refused / faulted, nothing changed (the reason is logged, first per reason).
// COST: 6 export calls, once per take.
bool HandCarryTakeWorld(HandCarry& c, void* obj, void* onModel, int onElem, const float drawn[12]);
// Detach to the world keeping its world matrix (DetachFromParent(true) - a DroppedItem's own OnDetached restarts its physics), then,
// when `world` is given, SetWorldXform(world). c.active = false. False = a fault (the carry is forgotten). COST: 1-2 export calls, once.
bool HandCarryReleaseWorld(HandCarry& c, const float* world, const char* why);
// True = the carried world object is no longer the live object the engine knows by its id (FindObjectByID on the player's IGSObject),
// or it left onElem: forget it, call nothing on it. COST: 2 export calls - per tick while carried (replaces HandCarryStillOn's one).
bool HandCarryObjectGone(const HandCarry& c, void* player);

}  // namespace dlvr
