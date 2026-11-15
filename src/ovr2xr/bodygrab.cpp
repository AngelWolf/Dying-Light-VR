// Grab bodies by hand - docs/superpowers/specs/2026-10-23-body-grab-design.md, plan docs/superpowers/plans/2026-10-23-body-grab.md
// Task 3. The decision is looting_logic.h (State::Dragging), the pull is bodygrab_logic.h (both unit-tested).
//
// HOW (docs/RESEARCH-BODY-GRAB-2026-10-23.md "Build constants", OUR engine e9fbfc2c + gamedll 97046bba):
//   - The body: the dead AI loot by hand's gather already holds (INV-616). Refused unless its vftable answers GetTriggerAction at
//     slot 179 with +18E280 (an AI - a standalone container has no ragdoll) and IsDead at slot 37 with HumanAI's +5E41C0 (b1) - our
//     base AI's slot 37 is `return 0`, so any other class is refused BY NAME - and that IsDead reads true (infected carry their
//     container from spawn, spec 6b.1).
//   - The ragdoll: IControlObject::PhysicsBind(AI+24) -> vfunc +168 = its physics object P (gamedll +120A470's own chain, the Task 1
//     control); P vfunc +464(point) = the body nearest the hand; P +248 = body count, +272(i) = body i (used to re-find the body).
//   - The drive: each player update, SpringForce toward the hand at the grab point (kept in the body's LOCAL frame, so the point
//     moves with the body), through IPhBody::AddForceAtPos. One force per frame: the frame's physics step has already run when the
//     tail runs, so it lands in the NEXT step ((a2)-(a4)). Whether the body's accumulator is cleared after every substep is NOT
//     established (Task 1 NOT-established 1) - the per-second line reports what the spring achieved, not what was intended.
//   - Release: stop pushing. Never SetLinearVel (a resting body ignores it, SECOND PASS 3).
//   - Re-ragdoll (U-65 option 2, docs/UNRESOLVED.md U-65 static pass 6): the engine DESTROYS a corpse's ragdoll about a second
//     after it comes to rest (CODEPhysics PostUpdate +395D90, SetMaxWatchingTime) and keeps its config at type 2. A grab that finds
//     no ragdoll restarts it ONCE per press the way the game does (DeathModule +290600): PhysicsBind(AI+24) vfunc +208 = the main
//     config -> IPhysicsConfig::StartPhysics. Created at once, the grab proceeds; queued by a busy physics world, the tick retries
//     for kReRagdollS, then the old refusal.
//   - Which part (Jay: "Option 1", RESEARCH-BODY-GRAB "LATCH PASS"): the ragdoll's own vfunc +464 (+3A4880) picks the body whose
//     CENTRE is nearest - a hand on a forearm's end or a hip's edge can be nearer another part's centre (reragdoll_run1: 5 of 19 at
//     0.25-0.37 m). PickAlongLimbs takes, over every joint linking two of the ragdoll's bodies, the segment between their centres
//     nearest the hand and the end of it the hand is nearer (an unlinked body - a one-part limb - counts by its centre). No link
//     found = the engine's pick. Once per press; START names both picks.
//   - Which part, by the visible skeleton (Jay: "Build it with the distance gate included", RESEARCH-BODY-GRAB "SKELETON PASS"):
//     the limb = the bone segment (parent joint -> child joint, IModelObject element exports on the AI) nearest the hand; the
//     part = the ragdoll body whose centre is nearest that segment (the ragdoll places each body on its bone). GATE: the hand
//     farther than body_grab_reach_m from every bone segment = "hand not on the body", refused BEFORE any re-ragdoll. No
//     skeleton readable = PickAlongLimbs, no gate (START says which).
//   - Which part, by HITBOX (Jay 10-24 (2): "The bone sets should have a radius like an actual hitbox"; RESEARCH-BODY-GRAB "READS
//     FOR THE THREE REFINEMENTS"): each ragdoll body's own ODE collision geoms (sphere / capsule / box, sized by ragdoll_ai.phx) are
//     its hitbox; the part = the body owning the hitbox nearest the hand, the gate = within body_grab_hitbox_margin_m of its surface.
//     Replaces the skeleton pick + bone gate whenever a hitbox is readable. 10-24 (2) (Jay: "the bone fix"): no bone gate at all -
//     a cut corpse keeps a cut part's bones, so a released ragdoll is restarted and its hitboxes decide. A severed limb is its own
//     object with its own geoms - measured on its own, compared with the corpse's hitbox only (TorsoTrack publishes it), never the
//     corpse's box or bones; a corpse refusal takes a limb at the hand instead (LimbInstead).
//   - Severed-limb ROTATION HOLD (Jay 10-24 (2): "Follow my hand's rotation"): a held limb is pulled by its centre to the hand's
//     pose times its START offset and turned to the hand's rotation by an inertia-scaled torque (AngularSpring) - the old pull at
//     an off-centre point left it free to spin about the hand. The limb is pushed ONCE PER PHYSICS STEP (its state changed since
//     the last push; limb_shake_run1: one step per 1.3-2.1 player updates). 10-30 (8) (Jay, the corpse shake: "apply the same
//     logic"): a corpse's held part gets the same hold and once-per-step push; the point pull is left only when the hold cannot arm.
//   - PALM + CATCH (Jay 10-24 (2): a caught part "doesn't exactly latch to the palm"): the grabbed point is the hitbox surface point
//     nearest the hand (ClosestOnShape), pulled into the hand; every press edge asks for a fresh limb query (BodyGrabRequestRelict)
//     and the press waits for it, so a flying limb is measured where it is now.
//   - Waking (spec 6.3, rg12 +399540): a body whose byte +56 has bit 0x04 set DROPS any force not longer than
//     `*(*(body+24)+744) * mass`; a longer one clears the bit and wakes it. START logs both; the second line counts the frames a
//     force was dropped that way.
//
// THREADS. BodyGrabBegin / End / PublishHand run on the scePad poll (LootPoll); BodyGrabTick on the player-update tail (HookTail).
// They share one command word + its fields per hand (interlocked exchange), a seqlocked hand sample per hand, and one active flag per
// hand. Every game read and call is SEH-guarded; a fault ends that hand's grab (named), never the process. Review F3 (INV-663, Jay
// 10-31: "Drive + Start"): a press and every held update first ask the engine's id lookup for the body - a destroyed one is
// refused / dropped without reading its memory (an SEH guard does not catch freed-but-mapped memory).
#include "bodygrab.h"
#include "boneblend.h"      // AfwNpcCollect ([AFWNPC])
#include "camhook.h"        // DiagAfwHeldMap ([AFWNPC])
#include "rtticam.h"        // RttiClassNameOf ([AFWNPC])

#include "bodygrab_logic.h"
#include "looting.h"        // LootByHand - body grab rides loot by hand's gather and machine

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: every fixed gamedll / engine rva below is on tools/egl_allowlist.tsv (family 8, translated exactly by
// tools/egl_rva_table.py); resolved ONCE in Resolve, before g_gd / g_en are published. Engine exports are found by name on both
// builds. The DEV tree has no buildrva.h: Steam rvas only.
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_GRAB_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_GRAB_EN_RVA(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_GRAB_GD_RVA(rva) (rva)
#define DLVR_GRAB_EN_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: body_grab] required-for=grabbing a dead body by hand - drag / fling / throw (BodyGrab) - spec 2026-10-23-body-grab-design.
//      tree=INERT at 0: BodyGrabTick returns at its first test, LootPoll never sees a drag (the machine's dragEnabled is false).
//      HEADSET: needs loot_by_hand=1 (its gather and machine), both grip poses + squeeze, the head pose, the rendered view matrix
//      and the OpenXR->game yaw (looting.cpp HandWorld), the player-update tail detour (boneblend.cpp HookTail), and the Steam
//      gamedll 97046bba or the Epic build (its rvas from buildrva_table.h family 8) - HumanAI's slot 37 is checked against
//      IsDead at READY; any other build REFUSES by that read.
//      A grab on a corpse whose ragdoll the engine already released restarts that ragdoll once (RE-RAGDOLL line) - engine
//      export IPhysicsConfig::StartPhysics on the corpse's own config; nothing else in the world is touched.
//      Severed limbs (AIRelict) are grab targets too: found by the engine's level query around the hand while that grip is
//      squeezed >= 0.2 (RelictTrack, game thread, <= every 100 ms per hand); needs ILevel::FindObjectsInRadius + IGSObject::GetLevel
//      (optional exports - named at READY) and the player from looting's GetNearbyTriggers detour.
//      Compiled default 1 since 2026-10-24 (3) (Jay: "They should be switched on" -> "Yes." - for the tester package;
//      verified 10-24 (2)). Was 0 / held until the native interactions ship together.
bool  BodyGrab = true;
// [DEP: body_grab_reach_m] required-for=how close the hand must be to a body's box to grab it away from the torso
//      (BodyGrabReachM). Its 10-24 (2) bone-segment gate was removed the same day (the bone fix: a cut corpse keeps cut bones).
//      tree=INERT at body_grab=0.
float BodyGrabReachM = 0.25f;
// [DEP: body_grab_hz] required-for=the pull's stiffness, the spring's natural frequency (BodyGrabHz). tree=INERT at body_grab=0.
float BodyGrabHz = 4.0f;
// [DEP: body_grab_damping] required-for=the pull's damping ratio (BodyGrabDamping). tree=INERT at body_grab=0.
float BodyGrabDamping = 0.7f;
// [DEP: body_grab_max_accel] required-for=the force cap per kg - the body's weight in the hand (BodyGrabMaxAccel, m/s^2).
//      tree=INERT at body_grab=0. Compiled 80 = Jay's disk tuning (2026-11-07, for the camouflage gesture: "We might need to change
//      the corpse weight a little" -> 80 -> "Much better"; was 65, 10-24 (2): "65 as the weight default"; before that 30).
float BodyGrabMaxAccel = 80.0f;
// [DEP: body_grab_limb_max_accel] required-for=the same force cap per kg for a SEVERED LIMB only (BodyGrabLimbMaxAccel, m/s^2) -
//      corpses keep body_grab_max_accel. Jay 10-24 (2): "Add a separate weight for severed limb, set it to 80" (higher = lighter;
//      the game reads every arm / leg piece at 45 kg). tree=INERT at body_grab=0.
float BodyGrabLimbMaxAccel = 80.0f;
// [DEP: body_grab_limb_turn_hz] required-for=the rotation hold's stiffness - how fast a held severed limb, and since 10-30 (8) a held
//      corpse part, turns to the hand's rotation (BodyGrabLimbTurnHz). Jay 10-24 (2): "build the shaking fix with the two new keys". Compiled 4 = the pull's own hz,
//      unchanged from the build that shook. tree=INERT at body_grab=0 (and when the rotation hold is off).
float BodyGrabLimbTurnHz = 4.0f;
// [DEP: body_grab_limb_turn_damping] required-for=the rotation hold's damping ratio (BodyGrabLimbTurnDamping; 1 = no overshoot).
//      Compiled 1.0 (the build that shook used the pull's 0.7 with a capped damper). tree=INERT at body_grab=0.
float BodyGrabLimbTurnDamping = 1.0f;
// [DEP: body_grab_hitbox_margin_m] required-for=how far outside a ragdoll part's own collision shape (its hitbox: sphere / capsule
//      / box, read from the engine's geoms - RESEARCH-BODY-GRAB 10-24 (2) A) a grip still takes that part (BodyGrabHitboxMarginM);
//      the same measure for a severed limb's own shape. Jay 10-24 (2): "New key, 0.08 m", then "Set the margins to 0.12" (compiled
//      = his disk). tree=INERT at body_grab=0.
float BodyGrabHitboxMarginM = 0.12f;
// [DEP: enemy_grab_stiff_hold] required-for=the B&S-like hold of a LIVE enemy (EnemyGrabStiffHold; RESEARCH-ENEMY-GRAB §10, Jay
//      2026-10-31 (6): "Okay, do it then. Make sure the weight is adjusted too"): when the open-hand grab (pushgrab.cpp) holds a
//      knocked-down enemy, every joint motor of its ragdoll is locked (velocity 0, max force enemy_grab_stiff_force - the engine's own
//      stiffness mechanism, Mac ApplyStiffness by name) and every body's gravity is switched off (IPhBody::SetGravityMode(0)) - it
//      keeps the shape it fell in and weighs nothing in the hand. Restored at the END while the same ragdoll is still current; a
//      ragdoll the game has already replaced (it got up) is never touched (PG3: a new ragdoll pointer per knock-down). A walk whose
//      joint types are not ball / hinge / angular motor with one motor per ball REFUSES, named, before any write.
//      tree=INERT at 0 (and at enemy_push_grab=0 - no live grab starts). HEADSET: a live grab (hand collision contact + squeeze).
//      Was compiled default 0 - a spike, Tier C until Jay verifies it in the headset.
bool  EnemyGrabStiffHold = true;    // SHIPS ON since 2026-11-03 (Jay: "Ship ON", the tester package; = his disk)
// [DEP: enemy_grab_stiff_force] required-for=the max force each locked joint motor may use (EnemyGrabStiffForce; ODE FMax, param 3
//      per axis - engine +37C7F0). Start value 10 = the Mac ApplyStiffness's stiffness-1 term (1000 x 0.01) - Claude's call, Tier D.
//      tree=INERT at enemy_grab_stiff_hold=0.
float EnemyGrabStiffForce = 10.0f;
// [DEP: enemy_grab_weight] required-for=how much of its weight a held, stiffened live enemy keeps (EnemyGrabWeight, 0..1). Jay
//      2026-10-31 (6): "The "no gravity" also causes the entire zombie to float" - gravity stays off on every part and each part is
//      pulled down every update by this fraction of its own weight (bodygrab_logic.h HeldWeightForce, g 9.81 - the engine's world
//      gravity NOT read), on parts still in a fresh walk only. One force per player update - physics substeps are not compensated
//      (as body grab's spring). Start value 0.30 - Claude's call, Tier D. tree=INERT at enemy_grab_stiff_hold=0.
float EnemyGrabWeight = 0.1f;   // 2026-11-03: Jay's disk value (Jay: "0.1 (your disk)"; was 0.30, Claude's starting value)

namespace {

// ---- OUR gamedll 97046bba / engine e9fbfc2c - RESEARCH-BODY-GRAB "Build constants" ----
constexpr uintptr_t kAiTrigActRva = 0x18E280;     // AI / HumanAI GetTriggerActionInternal, vftable slot 179 (INV-616)
constexpr size_t    kTrigActSlot = 179;
constexpr uintptr_t kHumanAiVtRva = 0x151D1C0;    // HumanAI's primary vftable (b1)
constexpr uintptr_t kIsDeadRva = 0x5E41C0;        // HumanAI::IsDead, slot 37 (b1)
constexpr size_t    kIsDeadSlot = 37;
constexpr size_t    kCtlObjOff = 24;              // AI + 24 = IControlObject (+120A470: add rcx, 18h)
constexpr size_t    kBindRagdollOff = 168;        // IPhysicsBind vfunc +168 -> the ragdoll's physics object P
constexpr size_t    kPCountOff = 248;             // P vfunc +248 -> body count
constexpr size_t    kPBodyOff = 272;              // P vfunc +272(i) -> body i
constexpr size_t    kPNearestOff = 464;           // P vfunc +464(const vec3*) -> the body nearest the point
constexpr size_t    kBodyOwnerOff = 24;           // body + 24 = its physics object (AddForceAtPos tests it, rg11 / rg12)
constexpr size_t    kBodyFlagsOff = 56;           // body byte +56, bit 0x04 = resting: weak forces are dropped (rg12 +399540)
constexpr unsigned char kRestBit = 0x04;
constexpr size_t    kOwnerWakeOff = 744;          // owner float +744 x mass = the force a resting body needs (rg12)
constexpr int       kMaxBodies = 64;              // a ragdoll has ~15; a larger count is a wrong pointer
constexpr double    kStaleS = 0.10;               // a hand sample older than this: no force this frame
constexpr double    kNoPoseS = 0.50;              // a hand sample older than this: the grab ends (Review Focus 2)
constexpr long      kMaxEventLines = 200;         // START / END / REFUSED - human rate; the cap guards a flickering grip (INV-9)

// ---- U-65 no-ragdoll diagnostic - read-only, only at that refusal (docs/UNRESOLVED.md U-65 static passes 3 + 4) ----
constexpr size_t    kVisGetSlot = 194;            // HumanAI slot 194 = +191C50: (AI+816)->vfunc +8 = its BodyVis (ida/bg29)
constexpr uintptr_t kVisGetRva = 0x191C50;
constexpr size_t    kVisOwnerOff = 80;            // vis + 80 = its AI
constexpr size_t    kVisSkipOff = 88;             // vis byte +88 - BodyVis OnPostUpdate +10A9000's physics-off branch is skipped (bg5)
constexpr size_t    kVisLatchOff = 224;           // vis byte +224 - the dead one-shot ragdoll latch (+10ACE80 sets, +10A9000 clears)
constexpr size_t    kVisOffMarkOff = 709;         // vis byte +709 - set 1 where +10A9000 switches the physics off
constexpr uintptr_t kGateMenuRva = 0x1C11AB5;     // gamedll byte - MenuAIDebugDI open (bg22)
constexpr size_t    kGateFlagsOff = 2728;         // AI dword +2728, bit 0x200 - a quest PlayMoviePhase actor (bg23)
constexpr size_t    kGateMovieOff = 912;          // AI byte +912 - ActorDI in-movie (bg15-bg18)
constexpr size_t    kVehAOff = 2456;              // UpdateInDead's veto +5F3EB0 (HumanAI slot 36, bg27): pair A
constexpr size_t    kVehBOff = 2464;              //   (A && B) ? ([B+1568] && [B+1576] && ![B+2048]) : (C && D)
constexpr size_t    kVehCOff = 1864;
constexpr size_t    kVehDOff = 1872;
constexpr size_t    kCoOff = 8;                   // IControlObject + 8 = CControlObject (engine PhysicsBind +13FC50: [this+8] + 584)
constexpr size_t    kCoMainCfgOff = 600;          // CControlObject + 600 = the main IPhysicsConfig (engine IsAux +308990)
constexpr size_t    kCoGsoFlagsOff = 64;          // CControlObject + 64 = GSO flags (engine GetGSOFlags +252200); 0x8 = active on
constexpr uint64_t  kGsoActive = 0x8;             //   module (IsActiveOnModule +2554E0) - EnablePhysics +137AD0 creates only then
constexpr uint64_t  kGsoNoSetup = 0x300000040ull; // SetupPhysicsConfig +137110 returns unchanged while any of these is set
constexpr size_t    kParamsDeferOff = 144;        // params byte +144 = EnablePhysics' "deferred, not created" mark (+137AD0)
constexpr size_t    kCfgPendingOff = 4;           // IPhysicsConfig + 4 = pending type (engine GetPendingPhysicsType +308A20)
constexpr size_t    kCfgParamsOff = 24;           // + 24 -> params; params byte +145 = sleeping because of PLOD (+308D50)
constexpr size_t    kParamsPlodOff = 145;
constexpr size_t    kCfgPhysOff = 40;             // + 40 = the physics; its +64 = working type (GetWorkingPhysicsType +308A30)
constexpr size_t    kPhysTypeOff = 64;
constexpr size_t    kCfgOwnerOff = 56;            // + 56 = the owning CControlObject (GetOwner +308970 / IsAux)
constexpr size_t    kBindCurOff = 160;            // IPhysicsBind vfunc +160 = the current physics, not type-filtered (bg2 +3342A0)
constexpr size_t    kBindCfgOff = 208;            // IPhysicsBind vfunc +208 = the main config (+334260: [[bind+8]+600]) - the game's
                                                  //   own StartPhysics idiom (DeathModule +290600, +5DE1C0, +5F9680)
constexpr uint32_t  kRagdollType = 2;             // EPhysics type 2 = ragdoll (+334350's filter)
// A severed limb is a SINGLE rigid body, type 4 (relict_run1: its config read "pending type 4"; bg53: the type-4 params are
// SingleSleepData). CPhysicsBind slot 23 (+184, +334310) returns the current physics only when its type is 4, and the single-body
// interface (CODEPhysicsSingle COL 1792 +86BB40) carries the ragdoll's own functions at slot 0 / +248 / +272 / +464 (bg54) - so
// the grab drives it unchanged. Used for severed limbs only.
constexpr size_t    kBindSingleOff = 184;
constexpr uint32_t  kSingleType = 4;
constexpr double    kReRagdollS = 0.5;            // a queued restart is retried this long, then the press is refused

const char* const kExpPhysicsBind = "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ";   // engine +13FC50
const char* const kExpAddForceAtPos = "?AddForceAtPos@IPhBody@@QEAAXAEBVvec3@@0@Z";            // +306F30
const char* const kExpGetMass = "?GetMass@IPhBody@@QEBAMXZ";                                   // +306920
const char* const kExpGetPointVel = "?GetPointVel@IPhBody@@QEBA?AVvec3@@AEBV2@@Z";            // +307150
const char* const kExpWorldToLocal = "?PointWorldToLocal@IPhBody@@QEBA?AVvec3@@AEBV2@@Z";     // +3070A0
const char* const kExpLocalToWorld = "?PointLocalToWorld@IPhBody@@QEBA?AVvec3@@AEBV2@@Z";     // +306F50
const char* const kExpGetPosition = "?GetPosition@IPhBody@@QEBAAEBVvec3@@XZ";                  // +3069C0
const char* const kExpStartPhysics = "?StartPhysics@IPhysicsConfig@@QEAAPEAVIPhysics@@XZ";    // +3089B0
const char* const kExpNumJoints = "?GetNumBasicJoints@IPhBody@@QEBAHXZ";                       // +3071B0 (body+40 list)
const char* const kExpGetJoint = "?GetBasicJoint@IPhBody@@QEBAPEAUIPhJoint@@H@Z";              // +3071D0
const char* const kExpJointBody = "?GetBody@IPhJoint@@QEBAPEAUIPhBody@@H@Z";                   // +3074E0 (0 / 1, null = world)
// The stiff live hold (RESEARCH-ENEMY-GRAB §10, ida/bsr9 / bsr10): all OPTIONAL - a missing one = the stiff hold OFF, named at READY.
const char* const kExpJointType = "?GetType@IPhJoint@@QEBA?AW4TYPE@EJointType@@XZ";             // +3074D0 (*(joint+40)+32)
const char* const kExpAmAxes = "?GetNumAxes@IPhJointAmotor@@QEBAHXZ";                           // +307850 (joint+120)
const char* const kExpAmGet = "?GetParam@IPhJointAmotor@@QEBAMW4TYPE@EJointParameter@@@Z";      // +307960 (param | axis << 8)
const char* const kExpAmSet = "?SetParam@IPhJointAmotor@@QEAAXW4TYPE@EJointParameter@@M@Z";     // +307920
const char* const kExpHiGet = "?GetParam@IPhJointHinge@@QEBAMW4TYPE@EJointParameter@@@Z";       // +3084A0
const char* const kExpHiSet = "?SetParam@IPhJointHinge@@QEAAXW4TYPE@EJointParameter@@M@Z";      // +308490
const char* const kExpGravGet = "?GetGravityMode@IPhBody@@QEBAHXZ";                             // +3071A0 (byte +56 bit 8 clear = on)
const char* const kExpGravSet = "?SetGravityMode@IPhBody@@QEAAXH@Z";                            // +307170
constexpr int     kJointBall = 1, kJointHinge = 2, kJointAmotor = 8;   // Mac ApplyStiffness's 2 / 8 + ODE ball 1 - checked per walk
constexpr int     kParamVel = 2, kParamFMax = 3;                       // engine +37C7F0: 2 -> vel, 3 -> fmax (>= 0)
constexpr int     kMaxStiffJoints = 64;
constexpr int       kMaxJointsPerBody = 16;       // a ragdoll body has a few links; more = a wrong pointer
// The corpse's skeleton - IModelObject exports (OUR engine, bg45); the AI is its own IModelObject (offset 0, bg15).
const char* const kExpElemCount = "?GetElementsNumber@IModelObject@@QEBAHXZ";                   // +141F60
const char* const kExpElemParent = "?GetElementParent@IModelObject@@QEBAHH@Z";                  // +142000 (-1 = none)
const char* const kExpElemIsBone = "?IsElementABone@IModelObject@@QEAA_NH@Z";                   // +145240 (type 4)
const char* const kExpElemWorldPos = "?GetElementWorldPos@IModelObject@@QEBA?AVvec3@@H@Z";      // +1429B0 (result buffer)
const char* const kExpElemName = "?GetElementNameCStr@IModelObject@@QEBAPEBDH@Z";               // +1428D0
const char* const kExpElemBox = "?GetElementExtentsInWorld@IModelObject@@QEBA?AVextents@@H@Z";  // +1423A0 (24 B, layout NOT read)
constexpr int       kMaxElements = 512;           // a human model has ~60-100; more = a wrong pointer
// Severed limbs (Jay: "Yes please" - squeeze-gated; RESEARCH-BODY-GRAB "SEVERED LIMBS PASS 2"): an AIRelict (OUR RTTI, primary vt
// +142CB58; IControlObject at +24, its ragdoll by the same PhysicsBind +168 chain). Found with the engine export
// ILevel::FindObjectsInRadius (+299560: clears and fills the caller's vector with the found objects' IControlObject*; NO class
// filter is passed - the CRTTI an AIRelict reports is not established - an entry is a relict when (entry - 24) carries +142CB58).
// The ILevel* = IGSObject::GetLevel (+2521D0) on the player's IGSObject (+40, PlayerDI CHD).
const char* const kExpFindRadius =
    "?FindObjectsInRadius@ILevel@@QEAA_NPEAV?$vector@PEAVIControlObject@@@ttl@@AEBVvec3@@MPEBVCRTTI@@_NPEAH@Z";   // +299560
const char* const kExpGetLevel = "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ";                     // +2521D0
// Review F3, body grab (INV-663; Jay 10-31: "Drive + Start"): the engine's own id lookup re-validates the held body without reading
// it - asked through the PLAYER's IGSObject. A corpse (HumanAI) and an AIRelict both carry their IGSObject at +40.
const char* const kExpGetId = "?GetID@IGSObject@@QEBAHXZ";                                      // +252080
const char* const kExpFindById = "?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z";                    // +251DF0
constexpr uintptr_t kRelictVtRva = 0x142CB58;     // AIRelict's primary vftable (bg50)
constexpr size_t    kGsoOff = 40;                 // PlayerDI / AIRelict + 40 = IGSObject (CHD mdisp 40)
constexpr float     kRelictSqueeze = 0.20f;       // the query runs only while the grip is squeezed this far (reaching)
constexpr float     kRelictRadiusM = 0.60f;       // the query's radius around the hand
constexpr ULONGLONG kRelictEveryMs = 100;         // at most this often per hand
constexpr ULONGLONG kRelictFreshMs = 250;         // older than this = none
constexpr int       kMaxRelictHits = 128;         // objects the query may return that are looked at
// Hitboxes = each body's own ODE collision geoms (OUR engine e9fbfc2c, ida/bg55; RESEARCH-BODY-GRAB 10-24 (2) A). IPhBody +64 = the
// first geom; geom +32 = the next geom on the same body; geom +24 = its body (checked - a mismatch stops that body's walk); geom
// +48 / +56 = pointers to the position / rotation (x, y, z, w) its AABB is built from; fields at +160.. by class, the class by its
// RTTI vftable (engine-relative). Any other class (dxGeomTransform, dxCylinder, ...) is counted, never measured.
constexpr size_t    kBodyGeomOff = 64;
constexpr size_t    kGeomNextOff = 32;
constexpr size_t    kGeomBodyOff = 24;
constexpr size_t    kGeomPosOff = 48;
constexpr size_t    kGeomRotOff = 56;
constexpr size_t    kGeomPrmOff = 160;
constexpr uintptr_t kVtSphereRva = 0x868060;      // ode::dxSphere   - radius +160
constexpr uintptr_t kVtCapsuleRva = 0x868000;     // ode::dxCapsule  - radius +160, cylinder length +164 along local Z
constexpr uintptr_t kVtBoxRva = 0x868030;         // ode::dxBox      - full sides +160 / +164 / +168
constexpr int       kMaxGeomsPerBody = 8;         // a ragdoll part carries one or two (a collision helper); more = a wrong pointer
// hitbox_run1: 9 of a corpse's 14 geoms and a severed limb's 2 were NOT direct shapes. ode::dxGeomTransform (vtable +868090, class
// id 6 - ctor +39A240) wraps one: inner shape at +160, whose own +48 / +56 point at its offset in the transform's frame; the engine's
// AABB pass (+3744E0, ida/bg56) composes the world pose = transform pos + transform rot * offset, rot = transform rot * offset rot
// (+0EA040, ida/bg57). Its cached result (+176 / +188) starts at the origin (ctor), so the pose is composed here, not read there.
constexpr uintptr_t kVtXformRva = 0x868090;
constexpr size_t    kXformInnerOff = 160;
constexpr int       kMaxGeomRecs = 48;            // geoms kept per walk for the HITBOX DUMP lines (a corpse has ~14)
// HITBOX DUMP sets per session - each is one press. 16 for the renders (hitbox_dump_run1); 0 = OFF since 10-24 (2) (Jay: "turn off
// whatever needs to be turned off" - the renders are made; DumpHitboxes returns at its first test, nothing else reads it).
constexpr long      kMaxDumps = 0;

// The vec3-returning members take a hidden result buffer: rcx = this, rdx = result, r8 = the argument (+307150 / +3070A0 disassembled).
using PhysicsBindFn = void*(__fastcall*)(void* ctlObj);
using AddForceAtPosFn = void(__fastcall*)(void* body, const float* force, const float* pos);
using GetMassFn = float(__fastcall*)(const void* body);
using Vec3RetFn = float*(__fastcall*)(const void* body, float* result, const float* arg);
using GetPositionFn = const float*(__fastcall*)(const void* body);
using StartPhysicsFn = void*(__fastcall*)(void* cfg);
using NumJointsFn = int(__fastcall*)(const void* body);
using GetJointFn = void*(__fastcall*)(const void* body, int i);
using JointBodyFn = void*(__fastcall*)(const void* joint, unsigned i);
using ElemCountFn = int(__fastcall*)(const void* model);
using ElemParentFn = int(__fastcall*)(const void* model, int i);
using ElemIsBoneFn = bool(__fastcall*)(void* model, int i);
using ElemVecFn = float*(__fastcall*)(const void* model, float* result, int i);
using ElemNameFn = const char*(__fastcall*)(const void* model, int i);
using FindRadiusFn = bool(__fastcall*)(void* level, void* outVec, const float* centre, float radius, const void* cls, bool exact,
                                       int* flags);
using GetLevelFn = void*(__fastcall*)(const void* gso);
using GetIdFn = int(__fastcall*)(const void* gso);
using FindByIdFn = void*(__fastcall*)(const void* gso, int id);
using VFn0 = void*(__fastcall*)(void* self);
using VFnI = void*(__fastcall*)(void* self, unsigned i);
using VFnP = void*(__fastcall*)(void* self, const float* p);
using CountFn = int(__fastcall*)(void* self);
using IsDeadFn = bool(__fastcall*)(void* self);

PhysicsBindFn   g_physicsBind = nullptr;
using JointIntFn = int(__fastcall*)(const void* joint);
using JointGetFn = float(__fastcall*)(const void* joint, int param);
using JointSetFn = void(__fastcall*)(void* joint, int param, float v);
using GravGetFn = int(__fastcall*)(const void* body);
using GravSetFn = void(__fastcall*)(void* body, int on);
JointIntFn g_jointType = nullptr, g_amAxes = nullptr;
JointGetFn g_amGet = nullptr, g_hiGet = nullptr;
JointSetFn g_amSet = nullptr, g_hiSet = nullptr;
GravGetFn  g_gravGet = nullptr;
GravSetFn  g_gravSet = nullptr;
AddForceAtPosFn g_addForceAtPos = nullptr;
GetMassFn       g_getMass = nullptr;
Vec3RetFn       g_getPointVel = nullptr;
Vec3RetFn       g_worldToLocal = nullptr;
Vec3RetFn       g_localToWorld = nullptr;
GetPositionFn   g_getPosition = nullptr;
StartPhysicsFn  g_startPhysics = nullptr;
NumJointsFn     g_numJoints = nullptr;
GetJointFn      g_getJoint = nullptr;
JointBodyFn     g_jointBody = nullptr;
ElemCountFn     g_elemCount = nullptr;
ElemParentFn    g_elemParent = nullptr;
ElemIsBoneFn    g_elemIsBone = nullptr;
ElemVecFn       g_elemWorldPos = nullptr;
ElemNameFn      g_elemName = nullptr;
ElemVecFn       g_elemBox = nullptr;
FindRadiusFn    g_findRadius = nullptr;   // optional: null = severed limbs are not looked for (named once at READY)
GetLevelFn      g_getLevel = nullptr;
GetIdFn         g_getId = nullptr;       // review F3 - required (missing = body grab REFUSED, named)
FindByIdFn      g_findById = nullptr;
bool            g_idSelfTestSaid = false;
// ---- severed-limb rotation hold (Jay 10-24 (2): "Follow my hand's rotation") ----
// The limb is held as if fixed to the hand: at START its centre and rotation are taken in the hand's frame; each update a spring
// pulls its CENTRE (no lever, so the pull itself no longer twists it - the old off-centre point pull did) to where that offset puts
// it, and AngularSpring turns it to the hand's rotation times the start offset, scaled by its own inertia (body +88, 3 rows of 4
// floats in the body frame - IPhBody::SetMass +397B10 scales exactly these). Engine: GetQuaternion (+1C250 -> body +200),
// GetAngularVel (+306C40 -> +228), AddTorque (+306EA0) - all exports; missing one = the old point pull, named at READY.
const char* const kExpBodyQuat = "?GetQuaternion@IPhBody@@QEBAAEBVquat@@XZ";
const char* const kExpAngVel = "?GetAngularVel@IPhBody@@QEBAAEBVvec3@@XZ";
const char* const kExpAddTorque = "?AddTorque@IPhBody@@QEAAXAEBVvec3@@@Z";
constexpr size_t kBodyInertiaOff = 88;
constexpr float  kHoldMaxAngAccel = 300.0f;   // rad/s^2 - the rotation spring's cap (its 1 s line names the capped share)
using GetVec4Fn = const float*(__fastcall*)(const void* body);
using AddTorqueFn = void(__fastcall*)(void* body, const float* torque);
GetVec4Fn   g_bodyQuat = nullptr;      // optional - null = no rotation hold (named at READY)
GetVec4Fn   g_angVel = nullptr;
AddTorqueFn g_addTorque = nullptr;
uintptr_t       g_gd = 0;
uintptr_t       g_en = 0;         // engine base - the geom classes' vftables are engine-relative
// the rvas as the loaded build has them (Steam values until Resolve translates them, before g_gd / g_en are published)
uintptr_t g_rAiTrigAct = kAiTrigActRva, g_rHumanAiVt = kHumanAiVtRva, g_rIsDead = kIsDeadRva, g_rVisGet = kVisGetRva;
uintptr_t g_rGateMenu = kGateMenuRva, g_rRelictVt = kRelictVtRva;
uintptr_t g_rVtSphere = kVtSphereRva, g_rVtCapsule = kVtCapsuleRva, g_rVtBox = kVtBoxRva, g_rVtXform = kVtXformRva;
int             g_resolved = 0;   // 0 not tried, 1 READY, -1 REFUSED (named once)
ULONGLONG       g_lastTryMs = 0;

// ---- shared pad thread <-> game thread, per hand (0 left, 1 right) ----
volatile LONG g_cmd[2] = {};              // 0 none, 1 begin, 2 end - the newest command wins
void* volatile g_cmdAi[2] = {};
float g_cmdPt[2][3] = {};
const char* volatile g_cmdWhy[2] = {};
volatile LONG g_active[2] = {};           // set at START (game thread), cleared at END or at once by BodyGrabEnd
volatile LONG g_pubSeq[2] = {};
float  g_pubPos[2][3] = {};
float  g_pubRot[2][4] = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };   // the hand's world rotation (x, y, z, w)
double g_pubT[2] = {};
volatile LONG g_nEvent = 0;               // START / END / REFUSED lines written

// ---- game thread only ----
struct Grab {
    bool active;
    void* ai;
    void* gso;        // ai + kGsoOff; with `id`, re-checked by FindObjectByID every update before any read (review F3, INV-663)
    int id;
    uintptr_t aiVt;
    void* p;
    void* body;
    float local[3];
    float mass;
    double startS;
    bool havePrev;
    double prevT;
    float prevPos[3];
    float handVel[3];
    // the per-second window
    double winS;
    long ticks, forces, capped, stale, wakeDropped;
    double errSum, fSum;
    float errMax, fMax;
    bool relict;      // a severed limb: no AI / IsDead re-validation (its vftable has 28 slots)
    bool live;        // a live enemy the game knocked down (pushgrab.cpp): no "still dead" re-validation; its ragdoll ending = the END
    // the rotation hold (a severed limb or, since 10-30 (8), a corpse part; false = the point pull)
    bool hold;
    float comLocal[3];   // the body's centre in the hand's frame at START
    float qRel[4];       // hand rotation^-1 * body rotation at START
    float inertia[9];    // body frame, read at START
    double angSum, wSum;
    float angMax;
    long angCapped;
    // the hand's own rotation speed (from successive published rotations) and the HOLD BURST (every update for ~1 s)
    float prevHandRot[4];
    double rotPrevT;
    float handW;
    int burst;           // 0 = not yet, 1 = recording, -1 = done / not this hold
    long bSet, bCount;
    int bFill;
    double bPrevS;
    float bDt[10], bErr[10], bSpin[10], bTq[10], bHandW[10];
    bool bCap[10];
    // once per physics step (limb_shake_run1: the limb's state changed only every 1.3-2.1 player updates, and a push per update
    // drove its spin to the engine's 50 rad/s cap): the state at the last push, when, and the tallies for the 1 s line
    float lastQ[4], lastP[3];
    double lastPushS;
    long pushes, skips;
};
constexpr double kPushFallbackS = 0.030;   // the state unchanged this long (a body at rest): push anyway, so it is still picked up
constexpr double kBurstStartS = 1.0;   // the burst starts this long into a hold (past the grab's own transient)
constexpr long   kBurstSamples = 120;  // ~1 s of player updates
// holds recorded per session: 3 for the shaking fix (limb_shake_run1, push_once_run1); 0 = OFF since 10-24 (2) (Jay: "build all
// three" - the shake is fixed; HoldBurst returns at its first test, nothing else reads it)
constexpr long   kMaxBursts = 0;
volatile LONG g_nBursts = 0;
Grab g_g[2] = {};
bool g_firstDrive = false;
// A press whose restart the engine queued: Start runs again from BodyGrabTick until the ragdoll is back or kReRagdollS passes.
struct ReRagdoll {
    bool pending;
    void* ai;
    float pt[3];
    double startS;
};
ReRagdoll g_rr[2] = {};
// Torso by the skeleton (Jay: "Do 1", RESEARCH-BODY-GRAB "SKELETON RUN"): looting's press decision asks which bone the hand is on.
// The pad thread publishes the body nearest each hand; the game thread reads its skeleton (TorsoTrack) and publishes the answer,
// seqlocked. The bone reads stay on the game thread.
constexpr ULONGLONG kTorsoEveryMs = 50;           // a fresh reading at most this often per hand, while a body is in reach
constexpr ULONGLONG kTorsoFreshMs = 150;          // older than this = not known (looting keeps its box rule)
void* volatile g_cand[2] = {};                    // pad -> game
volatile LONG g_torSeq[2] = {};                   // game -> pad: odd = writing
void* g_torBody[2] = {};
int g_torIs[2] = {};                              // 1 = a torso bone, 0 = another bone, -1 = no bone reading
float g_torHit[2] = {};                           // the hand to that body: hitbox surface (0 inside), else bone line, -1 = none
ULONGLONG g_torMs[2] = {};
ULONGLONG g_torLastRead[2] = {};                  // game thread only
volatile LONG g_nTorsoReads = 0;
// Severed limbs: pad -> game (squeeze, as float bits), any -> game (the player); game -> pad (the nearest relict), seqlocked.
volatile LONG g_squeezeBits[2] = {};
void* volatile g_player = nullptr;
volatile LONG g_relSeq[2] = {};
void* g_relObj[2] = {};
float g_relDist[2] = {};
ULONGLONG g_relMs[2] = {};
ULONGLONG g_relLastQuery[2] = {};                 // game thread only
volatile ULONGLONG g_sqStartMs[2] = {};           // pad -> pad: when the squeeze reached kRelictSqueeze (0 = below it)
// 10-24 (2) (Jay: catching a thrown part): a press asks for a fresh limb query at once - pad -> game; the press waits for a reading
// at least this new (a reading up to 100 ms old misses a flying limb)
volatile LONG      g_relReq[2] = {};
volatile ULONGLONG g_relReqMs[2] = {};
struct EngineVec { void** data; unsigned count; unsigned capacity; };
EngineVec g_relVec = {};                          // game thread only; grown by the engine's realloc, reused (one buffer, kept)
volatile LONG g_nRelictQueries = 0, g_nRelictsSeen = 0;

double NowS()   // the same clock as looting.cpp's NowS - the published hand times are on it
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
const char* HandName(int h) { return h == 0 ? "left" : "right"; }
bool EventLine()   // true = this START / END / REFUSED line may be written
{
    const LONG k = InterlockedIncrement(&g_nEvent);
    return k <= kMaxEventLines;
}

// ---- SEH-guarded game reads and calls (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadF32(uintptr_t a, float* out)
{
    __try { *out = *reinterpret_cast<const float*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadU8(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallIsDead(void* ai, bool* dead)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(ai);
        *dead = reinterpret_cast<IsDeadFn>(*reinterpret_cast<const uintptr_t*>(vt + 8 * kIsDeadSlot))(ai);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// AI -> PhysicsBind(AI+24) -> vfunc +168 = P (nullptr = no ragdoll). allowSingle (a severed limb): no ragdoll -> vfunc +184, the
// single rigid body. False = a fault.
bool RagdollOf(void* ai, void** pOut, bool allowSingle = false)
{
    __try {
        void* bind = g_physicsBind(static_cast<char*>(ai) + kCtlObjOff);
        if (!bind) { *pOut = nullptr; return true; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        *pOut = reinterpret_cast<VFn0>(*reinterpret_cast<const uintptr_t*>(vt + kBindRagdollOff))(bind);
        if (!*pOut && allowSingle) {
            *pOut = reinterpret_cast<VFn0>(*reinterpret_cast<const uintptr_t*>(vt + kBindSingleOff))(bind);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NearestBody(void* p, const float pt[3], void** bodyOut)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
        *bodyOut = reinterpret_cast<VFnP>(*reinterpret_cast<const uintptr_t*>(vt + kPNearestOff))(p, pt);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The body's index in P (-1 = not among P's bodies, -2 = a count out of range). False = a fault.
bool BodyIndex(void* p, void* body, int* idx, int* count)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
        const int n = reinterpret_cast<CountFn>(*reinterpret_cast<const uintptr_t*>(vt + kPCountOff))(p);
        *count = n;
        if (n < 0 || n > kMaxBodies) { *idx = -2; return true; }
        const VFnI at = reinterpret_cast<VFnI>(*reinterpret_cast<const uintptr_t*>(vt + kPBodyOff));
        for (int i = 0; i < n; ++i) {
            if (at(p, static_cast<unsigned>(i)) == body) { *idx = i; return true; }
        }
        *idx = -1;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BodyMass(void* body, float* m)
{
    __try { *m = g_getMass(body); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BodyVec(Vec3RetFn fn, void* body, const float in[3], float out[3])
{
    __try {
        float r[3] = {};
        fn(body, r, in);
        out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BodyPos(void* body, float out[3])
{
    __try {
        const float* q = g_getPosition(body);
        if (!q) { return false; }
        out[0] = q[0]; out[1] = q[1]; out[2] = q[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AddForce(void* body, const float f[3], const float at[3])
{
    __try { g_addForceAtPos(body, f, at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The resting flag and the force it needs (thr < 0 = no physics owner / unreadable).
void WakeState(void* body, float mass, bool* resting, float* thr)
{
    unsigned char fl = 0;
    uintptr_t owner = 0;
    float k = 0.0f;
    const uintptr_t b = reinterpret_cast<uintptr_t>(body);
    *resting = ReadU8(b + kBodyFlagsOff, &fl) && (fl & kRestBit) != 0;
    *thr = (ReadPtr(b + kBodyOwnerOff, &owner) && owner && ReadF32(owner + kOwnerWakeOff, &k)) ? k * mass : -1.0f;
}

float Len3(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
// The weight in the hand: a severed limb has its own (Jay 10-24 (2)), a corpse part body_grab_max_accel.
float CapAccel(bool relict) { return relict ? BodyGrabLimbMaxAccel : BodyGrabMaxAccel; }
bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// ---- which part the hand takes (PickAlongLimbs) ----
struct LimbPick {
    int n;        // bodies listed (-1 = the list faulted, -2 = a count out of range)
    int links;    // joints linking two listed bodies, each counted once
    int a, b;     // the winning segment's two body indices (b = -1: an unlinked body by its centre)
    float t;      // where along a -> b the hand projects (0..1)
    float d;      // the hand's distance to that segment (or centre)
    int picked;   // the index taken (-1 = the engine's pick is kept)
    int engIdx;   // the engine's nearest-centre body (+3A4880)
    float engD;   // the hand's distance to its centre
};
bool ListBodies(void* p, void* out[], int* n)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
        const int c = reinterpret_cast<CountFn>(*reinterpret_cast<const uintptr_t*>(vt + kPCountOff))(p);
        if (c < 0 || c > kMaxBodies) { *n = -2; return true; }
        const VFnI at = reinterpret_cast<VFnI>(*reinterpret_cast<const uintptr_t*>(vt + kPBodyOff));
        for (int i = 0; i < c; ++i) { out[i] = at(p, static_cast<unsigned>(i)); }
        *n = c;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The bodies a body's basic joints link it to (a null / world end is skipped). False = a fault.
bool JointPartners(void* body, void* out[], int* cnt)
{
    __try {
        *cnt = 0;
        const int nj = g_numJoints(body);
        if (nj < 0 || nj > kMaxJointsPerBody) { return true; }
        for (int j = 0; j < nj; ++j) {
            void* jt = g_getJoint(body, j);
            if (!jt) { continue; }
            void* b0 = g_jointBody(jt, 0);
            void* b1 = g_jointBody(jt, 1);
            void* other = (b0 == body) ? b1 : (b1 == body) ? b0 : nullptr;
            if (other) { out[(*cnt)++] = other; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The body to take (null = keep the engine's `engBody`). COST: once per press - the body list, one position per body, the joint
// lists (a ragdoll has ~14 bodies, ~13 links).
void* PickAlongLimbs(void* p, const float hand[3], void* engBody, LimbPick* lp)
{
    *lp = LimbPick{ -1, 0, -1, -1, 0.0f, -1.0f, -1, -1, -1.0f };
    void* bodies[kMaxBodies] = {};
    float pos[kMaxBodies][3] = {};
    bool have[kMaxBodies] = {};
    int n = 0;
    if (!ListBodies(p, bodies, &n)) { return nullptr; }
    lp->n = n;
    if (n <= 0) { return nullptr; }
    for (int i = 0; i < n; ++i) {
        have[i] = bodies[i] && BodyPos(bodies[i], pos[i]) && Finite3(pos[i]);
        if (have[i] && bodies[i] == engBody) {
            const float d[3] = { hand[0] - pos[i][0], hand[1] - pos[i][1], hand[2] - pos[i][2] };
            lp->engIdx = i;
            lp->engD = Len3(d);
        }
    }
    float best = 1.0e30f;
    for (int i = 0; i < n; ++i) {
        if (!have[i]) { continue; }
        void* partners[kMaxJointsPerBody] = {};
        int pc = 0;
        bool linked = false;
        if (JointPartners(bodies[i], partners, &pc)) {
            for (int k = 0; k < pc; ++k) {
                int o = -1;
                for (int m = 0; m < n; ++m) {
                    if (bodies[m] == partners[k]) { o = m; break; }
                }
                if (o < 0 || !have[o]) { continue; }
                linked = true;
                if (o < i) { continue; }   // each link once
                ++lp->links;
                const float ab[3] = { pos[o][0] - pos[i][0], pos[o][1] - pos[i][1], pos[o][2] - pos[i][2] };
                const float ah[3] = { hand[0] - pos[i][0], hand[1] - pos[i][1], hand[2] - pos[i][2] };
                const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                float t = len2 > 1.0e-6f ? (ah[0] * ab[0] + ah[1] * ab[1] + ah[2] * ab[2]) / len2 : 0.0f;
                t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                const float q[3] = { ah[0] - t * ab[0], ah[1] - t * ab[1], ah[2] - t * ab[2] };
                const float d = Len3(q);
                if (d < best) {
                    best = d;
                    lp->a = i;
                    lp->b = o;
                    lp->t = t;
                    lp->d = d;
                    lp->picked = t < 0.5f ? i : o;
                }
            }
        }
        if (!linked) {   // an unlinked body (a one-part limb) counts by its centre
            const float hc[3] = { hand[0] - pos[i][0], hand[1] - pos[i][1], hand[2] - pos[i][2] };
            const float d = Len3(hc);
            if (d < best) {
                best = d;
                lp->a = i;
                lp->b = -1;
                lp->t = 0.0f;
                lp->d = d;
                lp->picked = i;
            }
        }
    }
    if (lp->links == 0 || lp->picked < 0) {
        lp->picked = -1;
        return nullptr;
    }
    return bodies[lp->picked];
}

// ---- which part the hand takes, by the visible skeleton ----
struct SkelPick {
    int ok;          // 1 = a bone segment found, 0 = none (no skeleton / no bone pairs), -1 = a fault
    int n, bones;    // elements, bone elements
    int limb, child; // the segment's parent end (the limb) and child end, element indices
    float a[3], b[3];
    float t, d;      // where the hand projects on a -> b (0..1), and its distance to the segment
    char name[48];   // the limb element's name ("" = unread)
    float box[6];    // GetElementExtentsInWorld raw (24 B; layout NOT read - logged as read)
    int bodyIdx;     // the ragdoll body mapped to the segment (-1 = none)
    float bodyD;     // that body's centre to the segment
};
float SegDist(const float p[3], const float a[3], const float b[3], float* tOut)
{
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 1.0e-6f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const float q[3] = { ap[0] - t * ab[0], ap[1] - t * ab[1], ap[2] - t * ab[2] };
    if (tOut) { *tOut = t; }
    return Len3(q);
}
// The bone segment nearest the hand. False = a fault (sk->ok = -1). COST: once per press - per element IsElementABone, per bone
// GetElementWorldPos + GetElementParent (~60-100 elements), one name + one box read.
bool SkelLimb(void* model, const float hand[3], SkelPick* sk)
{
    __try {
        sk->ok = 0;
        sk->n = g_elemCount(model);
        if (sk->n <= 0 || sk->n > kMaxElements) { return true; }
        float pos[kMaxElements][3];
        unsigned char bone[kMaxElements];
        for (int e = 0; e < sk->n; ++e) {
            bone[e] = g_elemIsBone(model, e) ? 1 : 0;
            if (bone[e]) {
                float r[3] = {};
                g_elemWorldPos(model, r, e);
                pos[e][0] = r[0]; pos[e][1] = r[1]; pos[e][2] = r[2];
                if (!Finite3(pos[e])) { bone[e] = 0; } else { ++sk->bones; }
            }
        }
        float best = 1.0e30f;
        for (int e = 0; e < sk->n; ++e) {
            if (!bone[e]) { continue; }
            const int par = g_elemParent(model, e);
            if (par < 0 || par >= sk->n || !bone[par]) { continue; }
            float t = 0.0f;
            const float d = SegDist(hand, pos[par], pos[e], &t);
            if (d < best) {
                best = d;
                sk->ok = 1;
                sk->limb = par;
                sk->child = e;
                sk->t = t;
                sk->d = d;
                for (int k = 0; k < 3; ++k) { sk->a[k] = pos[par][k]; sk->b[k] = pos[e][k]; }
            }
        }
        if (sk->ok == 1) {
            const char* nm = g_elemName(model, sk->limb);
            int k = 0;
            for (; nm && k < 47 && nm[k] >= 32 && nm[k] < 127; ++k) { sk->name[k] = nm[k]; }
            sk->name[k] = 0;
            float bx[6] = {};
            g_elemBox(model, bx, sk->limb);
            for (int i = 0; i < 6; ++i) { sk->box[i] = bx[i]; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { sk->ok = -1; return false; }
}
// The ragdoll body whose centre is nearest the picked segment (null = none readable).
void* MapLimbToBody(void* p, SkelPick* sk)
{
    void* bodies[kMaxBodies] = {};
    int n = 0;
    if (!ListBodies(p, bodies, &n) || n <= 0) { return nullptr; }
    float best = 1.0e30f;
    void* pick = nullptr;
    for (int i = 0; i < n; ++i) {
        float c[3] = {};
        if (!bodies[i] || !BodyPos(bodies[i], c) || !Finite3(c)) { continue; }
        const float d = SegDist(c, sk->a, sk->b, nullptr);
        if (d < best) {
            best = d;
            pick = bodies[i];
            sk->bodyIdx = i;
            sk->bodyD = d;
        }
    }
    return pick;
}

// ---- which part the hand takes, by the parts' own hitboxes (Jay 10-24 (2): "a radius like an actual hitbox") ----
struct HitPick {
    int bodies;          // bodies listed (-1 = the list faulted, -2 = a count out of range)
    int shapes;          // geoms measured: spheres + capsules + boxes
    int spheres, capsules, boxes;
    int other;           // geoms of another class, or with fields out of range - counted, not measured
    int unmatched;       // a geom whose +24 is not its body (the walk of that body stopped)
    int faults;          // bodies whose walk faulted
    int idx;             // the nearest hitbox's body index (-1 = none)
    void* body;
    float d;             // hand to that hitbox's surface, metres (<= 0 inside)
    int shape;           // its class (bodygrab::Shape), 0 = none
    float prm[3];        // its fields as read (radius / length / sides)
    float c[3], q[4];    // its world centre / rotation (the palm point is found on it)
    int wrapped;         // hitboxes measured through a dxGeomTransform
    // every geom walked, for the HITBOX DUMP lines (the renders)
    struct Rec {
        int bi;          // body index
        int shape;       // bodygrab::Shape, 0 = not measured
        int wrapped;     // 1 = inside a dxGeomTransform
        int ok;          // 1 = measured; 0 = refused (class / fields / pose out of range)
        unsigned vtRva;  // the shape's own vftable, engine-relative (the inner one when wrapped)
        float c[3], q[4], prm[3], d;
    };
    int nrec;
    Rec recs[kMaxGeomRecs];
};
// One body's geoms. False = a fault (that body is skipped).
bool WalkBodyGeoms(void* body, int bi, const float hand[3], HitPick* hp)
{
    __try {
        uintptr_t g = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(body) + kBodyGeomOff);
        for (int k = 0; g && k < kMaxGeomsPerBody; ++k) {
            if (*reinterpret_cast<void* const*>(g + kGeomBodyOff) != body) { ++hp->unmatched; break; }
            uintptr_t vt = *reinterpret_cast<const uintptr_t*>(g);
            const float* c = *reinterpret_cast<const float* const*>(g + kGeomPosOff);
            const float* q = *reinterpret_cast<const float* const*>(g + kGeomRotOff);
            uintptr_t src = g;   // the geom whose shape fields are read
            bool wrapped = false;
            float tp[3] = {}, tq[4] = {};
            if (vt == g_en + g_rVtXform) {
                // the transform's own pose = the body's; the inner shape's +48 / +56 = its offset in that frame
                const uintptr_t inner = *reinterpret_cast<const uintptr_t*>(g + kXformInnerOff);
                if (inner && c && q) {
                    for (int i = 0; i < 3; ++i) { tp[i] = c[i]; }
                    for (int i = 0; i < 4; ++i) { tq[i] = q[i]; }
                    src = inner;
                    vt = *reinterpret_cast<const uintptr_t*>(inner);
                    c = *reinterpret_cast<const float* const*>(inner + kGeomPosOff);
                    q = *reinterpret_cast<const float* const*>(inner + kGeomRotOff);
                    wrapped = true;
                } else {
                    c = q = nullptr;
                }
            }
            const float* f = reinterpret_cast<const float*>(src + kGeomPrmOff);
            const bodygrab::Shape s = vt == g_en + g_rVtSphere ? bodygrab::Shape::Sphere
                                    : vt == g_en + g_rVtCapsule ? bodygrab::Shape::Capsule
                                    : vt == g_en + g_rVtBox ? bodygrab::Shape::Box : static_cast<bodygrab::Shape>(0);
            const int np = s == bodygrab::Shape::Sphere ? 1 : s == bodygrab::Shape::Capsule ? 2 : 3;
            bool ok = static_cast<int>(s) != 0 && c && q;
            float cc[3] = {}, qq[4] = {}, pp[3] = {};
            if (ok) {
                for (int i = 0; i < 3; ++i) { cc[i] = c[i]; }
                for (int i = 0; i < 4; ++i) { qq[i] = q[i]; }
                for (int i = 0; i < np; ++i) { pp[i] = f[i]; }
                if (wrapped) {
                    const float lp[3] = { cc[0], cc[1], cc[2] };
                    const float lq[4] = { qq[0], qq[1], qq[2], qq[3] };
                    bodygrab::ComposePose(tp, tq, lp, lq, cc, qq);
                    // a wrapped shape's offset is part-sized: a larger one is a wrong read
                    ok = Finite3(lp) && (lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]) < 1.0f;
                }
                const float qn = qq[0] * qq[0] + qq[1] * qq[1] + qq[2] * qq[2] + qq[3] * qq[3];
                ok = ok && Finite3(cc) && std::isfinite(qn) && qn > 0.9f && qn < 1.1f;
                for (int i = 0; i < np && ok; ++i) {
                    // a radius within (0, 1) m, a capsule length / box side within [0, 3) m - else a wrong read
                    ok = std::isfinite(pp[i]) && (i == 0 && s != bodygrab::Shape::Box ? (pp[i] > 0.0f && pp[i] < 1.0f)
                                                                                       : (pp[i] >= 0.0f && pp[i] < 3.0f));
                }
            }
            const float d = ok ? bodygrab::HitboxDist(s, cc, qq, pp, hand) : -1.0f;
            if (hp->nrec < kMaxGeomRecs) {
                HitPick::Rec& r = hp->recs[hp->nrec++];
                r.bi = bi;
                r.shape = static_cast<int>(s);
                r.wrapped = wrapped ? 1 : 0;
                r.ok = ok ? 1 : 0;
                r.vtRva = static_cast<unsigned>(vt >= g_en ? vt - g_en : 0);
                for (int i = 0; i < 3; ++i) { r.c[i] = cc[i]; r.prm[i] = pp[i]; }
                for (int i = 0; i < 4; ++i) { r.q[i] = qq[i]; }
                r.d = d;
            }
            if (!ok) {
                ++hp->other;
            } else {
                ++hp->shapes;
                if (wrapped) { ++hp->wrapped; }
                if (s == bodygrab::Shape::Sphere) { ++hp->spheres; } else if (s == bodygrab::Shape::Capsule) { ++hp->capsules; }
                else { ++hp->boxes; }
                if (hp->idx < 0 || d < hp->d) {
                    hp->idx = bi;
                    hp->body = body;
                    hp->d = d;
                    hp->shape = static_cast<int>(s);
                    for (int i = 0; i < 3; ++i) { hp->prm[i] = pp[i]; hp->c[i] = cc[i]; }
                    for (int i = 0; i < 4; ++i) { hp->q[i] = qq[i]; }
                }
            }
            g = *reinterpret_cast<const uintptr_t*>(g + kGeomNextOff);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const char* ShapeName(int s) { return s == 1 ? "sphere" : s == 2 ? "capsule" : s == 3 ? "box" : "none"; }
volatile LONG g_nHitWalks = 0;
// The physics object's hitbox nearest the hand. COST: the body list (~14 for a corpse, 1 for a severed limb) + per geom ~12 plain
// reads and one distance - no engine call beyond the list. Called once per press (Start), at most every 50 ms per hand near a corpse
// (TorsoTrack), and per severed limb per relict query (<= every 100 ms per squeezed hand).
void NearestHitbox(void* p, const float hand[3], HitPick* hp)
{
    *hp = HitPick{};
    hp->idx = -1;
    hp->d = -1.0f;
    void* bodies[kMaxBodies] = {};
    int n = 0;
    if (!ListBodies(p, bodies, &n)) { hp->bodies = -1; return; }
    hp->bodies = n;
    for (int i = 0; i < n; ++i) {
        if (bodies[i] && !WalkBodyGeoms(bodies[i], i, hand, hp)) { ++hp->faults; }
    }
    if (InterlockedIncrement(&g_nHitWalks) == 1) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HITBOX first walk (logged unconditionally) - physics %p: %d bodies, %d hitboxes measured (%d sphere, "
            "%d capsule, %d box; %d of them inside a transform), %d other / out-of-range geoms, %d geoms not on their body, %d "
            "bodies faulted | nearest: body index %d, %s (%.3f %.3f %.3f), hand %.3f m from its surface (<= 0 inside) | thread %lu",
            p, hp->bodies, hp->shapes, hp->spheres, hp->capsules, hp->boxes, hp->wrapped, hp->other, hp->unmatched, hp->faults,
            hp->idx, ShapeName(hp->shape), hp->prm[0], hp->prm[1], hp->prm[2], hp->d, GetCurrentThreadId());
    }
}

// ---- HITBOX DUMP (Jay 10-24 (2): "show me the sizing and location of the hitboxes for the whole body per limb & include separate
// renders for the severed limbs") - log-only, once per press, kMaxDumps sets a session: every geom walked (world pose, class,
// fields, the bone nearest its centre) and every bone of the model (name, parent, world position), plus the hand. Game thread only.
struct DumpBone { int elem; int parent; float p[3]; char name[32]; };
DumpBone g_dumpBones[kMaxElements];   // game thread only (Start) - kept off the stack
volatile LONG g_nDumps = 0;
int ReadBones(void* model, int* nElems)
{
    __try {
        *nElems = g_elemCount(model);
        if (*nElems <= 0 || *nElems > kMaxElements) { return 0; }
        int n = 0;
        for (int e = 0; e < *nElems; ++e) {
            if (!g_elemIsBone(model, e)) { continue; }
            DumpBone& b = g_dumpBones[n];
            float r[3] = {};
            g_elemWorldPos(model, r, e);
            b.p[0] = r[0]; b.p[1] = r[1]; b.p[2] = r[2];
            if (!Finite3(b.p)) { continue; }
            b.parent = g_elemParent(model, e);
            const char* nm = g_elemName(model, e);
            int k = 0;
            for (; nm && k < 31 && nm[k] >= 32 && nm[k] < 127; ++k) { b.name[k] = nm[k]; }
            b.name[k] = 0;
            b.elem = e;
            ++n;
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void DumpHitboxes(int h, void* model, bool relict, const HitPick& hp, const float hand[3], const char* outcome)
{
    if (kMaxDumps <= 0) { return; }   // OFF (see kMaxDumps)
    const LONG set = InterlockedIncrement(&g_nDumps);
    if (set > kMaxDumps) { return; }
    int nElems = 0;
    const int nb = ReadBones(model, &nElems);
    // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
    LOG("bodygrab: [BODYGRAB] HITBOX DUMP %ld/%ld BEGIN - %s hand, %s %p (%s) | hand (%.4f %.4f %.4f) | %d geoms kept of %d bodies "
        "(%d measured, %d other) | %d bones of %d elements (%s) | rotations (x y z w); capsule = radius, cylinder length along "
        "local Z; box = full sides", set, kMaxDumps, HandName(h), relict ? "severed limb" : "corpse", model, outcome, hand[0],
        hand[1], hand[2], hp.nrec, hp.bodies, hp.shapes, hp.other, nb, nElems, nb < 0 ? "skeleton read FAULTED" : "read");
    for (int i = 0; i < hp.nrec; ++i) {
        const HitPick::Rec& r = hp.recs[i];
        int bone = -1;
        float bd = 1.0e30f;
        for (int b = 0; b < nb; ++b) {
            const float d[3] = { r.c[0] - g_dumpBones[b].p[0], r.c[1] - g_dumpBones[b].p[1], r.c[2] - g_dumpBones[b].p[2] };
            const float l = Len3(d);
            if (l < bd) { bd = l; bone = b; }
        }
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HITBOX DUMP %ld GEOM %d - body %d | %s%s (vt +%X) %s | centre (%.4f %.4f %.4f) rot (%.4f %.4f %.4f "
            "%.4f) | fields (%.4f %.4f %.4f) | hand to surface %.4f m | nearest bone to its centre %s at %.4f m", set, i, r.bi,
            r.wrapped ? "in a transform: " : "", ShapeName(r.shape), r.vtRva, r.ok ? "measured" : "NOT measured", r.c[0], r.c[1],
            r.c[2], r.q[0], r.q[1], r.q[2], r.q[3], r.prm[0], r.prm[1], r.prm[2], r.d,
            bone >= 0 ? g_dumpBones[bone].name : "none", bone >= 0 ? bd : -1.0f);
    }
    for (int b0 = 0; b0 < nb; b0 += 12) {
        char line[1024];
        int len = 0;
        for (int b = b0; b < nb && b < b0 + 12 && len < 900; ++b) {
            const int n = std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " %d<%d %s (%.4f %.4f %.4f);",
                                        g_dumpBones[b].elem, g_dumpBones[b].parent, g_dumpBones[b].name, g_dumpBones[b].p[0], g_dumpBones[b].p[1], g_dumpBones[b].p[2]);
            if (n > 0) { len += n; }
        }
        line[len < 1023 ? len : 1023] = 0;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HITBOX DUMP %ld BONES %d-%d (element<parent name (x y z)):%s", set, b0,
            (b0 + 11 < nb - 1 ? b0 + 11 : nb - 1), line);
    }
    // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
    LOG("bodygrab: [BODYGRAB] HITBOX DUMP %ld END", set);
}

// Once (retried each second until settled): the engine exports and the build check. READY / REFUSED named once.
bool Resolve()
{
    if (g_resolved != 0) { return g_resolved == 1; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTryMs < 1000) { return false; }
    g_lastTryMs = now;
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!gd || !en) { return false; }
    // the build's rvas first (Steam, or the Epic build's from buildrva_table.h), then the bases every reader tests
    g_rAiTrigAct = DLVR_GRAB_GD_RVA(kAiTrigActRva);
    g_rHumanAiVt = DLVR_GRAB_GD_RVA(kHumanAiVtRva);
    g_rIsDead = DLVR_GRAB_GD_RVA(kIsDeadRva);
    g_rVisGet = DLVR_GRAB_GD_RVA(kVisGetRva);
    g_rGateMenu = DLVR_GRAB_GD_RVA(kGateMenuRva);
    g_rRelictVt = DLVR_GRAB_GD_RVA(kRelictVtRva);
    g_rVtSphere = DLVR_GRAB_EN_RVA(kVtSphereRva);
    g_rVtCapsule = DLVR_GRAB_EN_RVA(kVtCapsuleRva);
    g_rVtBox = DLVR_GRAB_EN_RVA(kVtBoxRva);
    g_rVtXform = DLVR_GRAB_EN_RVA(kVtXformRva);
    MemoryBarrier();
    g_gd = reinterpret_cast<uintptr_t>(gd);
    g_en = reinterpret_cast<uintptr_t>(en);
    g_physicsBind = reinterpret_cast<PhysicsBindFn>(GetProcAddress(en, kExpPhysicsBind));
    g_addForceAtPos = reinterpret_cast<AddForceAtPosFn>(GetProcAddress(en, kExpAddForceAtPos));
    g_getMass = reinterpret_cast<GetMassFn>(GetProcAddress(en, kExpGetMass));
    g_getPointVel = reinterpret_cast<Vec3RetFn>(GetProcAddress(en, kExpGetPointVel));
    g_worldToLocal = reinterpret_cast<Vec3RetFn>(GetProcAddress(en, kExpWorldToLocal));
    g_localToWorld = reinterpret_cast<Vec3RetFn>(GetProcAddress(en, kExpLocalToWorld));
    g_getPosition = reinterpret_cast<GetPositionFn>(GetProcAddress(en, kExpGetPosition));
    g_startPhysics = reinterpret_cast<StartPhysicsFn>(GetProcAddress(en, kExpStartPhysics));
    g_numJoints = reinterpret_cast<NumJointsFn>(GetProcAddress(en, kExpNumJoints));
    g_getJoint = reinterpret_cast<GetJointFn>(GetProcAddress(en, kExpGetJoint));
    g_jointBody = reinterpret_cast<JointBodyFn>(GetProcAddress(en, kExpJointBody));
    g_elemCount = reinterpret_cast<ElemCountFn>(GetProcAddress(en, kExpElemCount));
    g_elemParent = reinterpret_cast<ElemParentFn>(GetProcAddress(en, kExpElemParent));
    g_elemIsBone = reinterpret_cast<ElemIsBoneFn>(GetProcAddress(en, kExpElemIsBone));
    g_elemWorldPos = reinterpret_cast<ElemVecFn>(GetProcAddress(en, kExpElemWorldPos));
    g_elemName = reinterpret_cast<ElemNameFn>(GetProcAddress(en, kExpElemName));
    g_elemBox = reinterpret_cast<ElemVecFn>(GetProcAddress(en, kExpElemBox));
    g_getId = reinterpret_cast<GetIdFn>(GetProcAddress(en, kExpGetId));
    g_findById = reinterpret_cast<FindByIdFn>(GetProcAddress(en, kExpFindById));
    const char* missing = !g_physicsBind ? kExpPhysicsBind : !g_addForceAtPos ? kExpAddForceAtPos : !g_getMass ? kExpGetMass
                        : !g_getPointVel ? kExpGetPointVel : !g_worldToLocal ? kExpWorldToLocal
                        : !g_localToWorld ? kExpLocalToWorld : !g_getPosition ? kExpGetPosition
                        : !g_startPhysics ? kExpStartPhysics : !g_numJoints ? kExpNumJoints : !g_getJoint ? kExpGetJoint
                        : !g_jointBody ? kExpJointBody : !g_elemCount ? kExpElemCount : !g_elemParent ? kExpElemParent
                        : !g_elemIsBone ? kExpElemIsBone : !g_elemWorldPos ? kExpElemWorldPos : !g_elemName ? kExpElemName
                        : !g_elemBox ? kExpElemBox : !g_getId ? kExpGetId : !g_findById ? kExpFindById : nullptr;
    if (missing) {
        g_resolved = -1;
        // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=no
        LOG("bodygrab: [BODYGRAB] REFUSED - the engine does not export %s; body grab stays off this launch (loot by hand is "
            "unchanged)", missing);
        return false;
    }
    uintptr_t slot37 = 0;
    if (!ReadPtr(g_gd + g_rHumanAiVt + 8 * kIsDeadSlot, &slot37) || slot37 != g_gd + g_rIsDead) {
        g_resolved = -1;
        // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=no
        LOG("bodygrab: [BODYGRAB] REFUSED - HumanAI vftable gamedll+%llX slot %zu reads +%llX, expected IsDead +%llX: not the Steam "
            "gamedll 97046bba / the Epic build these offsets were read from. Body grab stays off this launch (loot by hand is "
            "unchanged).",
            static_cast<unsigned long long>(g_rHumanAiVt), kIsDeadSlot,
            static_cast<unsigned long long>(slot37 ? slot37 - g_gd : 0), static_cast<unsigned long long>(g_rIsDead));
        return false;
    }
    g_findRadius = reinterpret_cast<FindRadiusFn>(GetProcAddress(en, kExpFindRadius));
    g_getLevel = reinterpret_cast<GetLevelFn>(GetProcAddress(en, kExpGetLevel));
    g_bodyQuat = reinterpret_cast<GetVec4Fn>(GetProcAddress(en, kExpBodyQuat));
    g_angVel = reinterpret_cast<GetVec4Fn>(GetProcAddress(en, kExpAngVel));
    g_addTorque = reinterpret_cast<AddTorqueFn>(GetProcAddress(en, kExpAddTorque));
    g_jointType = reinterpret_cast<JointIntFn>(GetProcAddress(en, kExpJointType));
    g_amAxes = reinterpret_cast<JointIntFn>(GetProcAddress(en, kExpAmAxes));
    g_amGet = reinterpret_cast<JointGetFn>(GetProcAddress(en, kExpAmGet));
    g_amSet = reinterpret_cast<JointSetFn>(GetProcAddress(en, kExpAmSet));
    g_hiGet = reinterpret_cast<JointGetFn>(GetProcAddress(en, kExpHiGet));
    g_hiSet = reinterpret_cast<JointSetFn>(GetProcAddress(en, kExpHiSet));
    g_gravGet = reinterpret_cast<GravGetFn>(GetProcAddress(en, kExpGravGet));
    g_gravSet = reinterpret_cast<GravSetFn>(GetProcAddress(en, kExpGravSet));
    const char* stiffMissing = !g_jointType ? kExpJointType : !g_amAxes ? kExpAmAxes : !g_amGet ? kExpAmGet : !g_amSet ? kExpAmSet
                             : !g_hiGet ? kExpHiGet : !g_hiSet ? kExpHiSet : !g_gravGet ? kExpGravGet : !g_gravSet ? kExpGravSet
                             : nullptr;
    // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=yes
    LOG("bodygrab: [BODYGRAB] STIFF HOLD (live enemy) %s%s | enemy_grab_stiff_hold=%d, motor force %.1f, kept weight %.2f (logged "
        "unconditionally)",
        stiffMissing ? "*** OFF - the engine does not export " : "AVAILABLE - joint motors locked + gravity off while a live enemy is held",
        stiffMissing ? stiffMissing : "", EnemyGrabStiffHold ? 1 : 0, static_cast<double>(EnemyGrabStiffForce),
        static_cast<double>(EnemyGrabWeight));
    const char* holdMissing = !g_bodyQuat ? kExpBodyQuat : !g_angVel ? kExpAngVel : !g_addTorque ? kExpAddTorque : nullptr;
    // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=no
    LOG("bodygrab: [BODYGRAB] severed limbs %s - the level query around the hand runs while the grip is squeezed >= %.2f, at most "
        "every %llu ms per hand, radius %.2f m | rotation hold %s%s (cap %.0f rad/s^2)",
        (g_findRadius && g_getLevel) ? "LOOKED FOR" : "*** NOT looked for: an engine export is missing ***", kRelictSqueeze,
        static_cast<unsigned long long>(kRelictEveryMs), kRelictRadiusM,
        holdMissing ? "*** OFF - the engine does not export " : "ON - a held limb / corpse part turns with the hand", holdMissing ? holdMissing : "",
        kHoldMaxAngAccel);
    g_resolved = 1;
    // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=no
    LOG("bodygrab: [BODYGRAB] READY (logged unconditionally with body_grab=1) - engine exports resolved, HumanAI IsDead slot %zu = "
        "+%llX verified | reach %.2f m, hitbox margin %.2f m, spring %.2f Hz, damping %.2f, cap %.1f m/s^2 (severed limbs %.1f) | "
        "loot_by_hand=%d (%s) | one force per player update, after the frame's physics step - substeps not compensated | thread %lu",
        kIsDeadSlot, static_cast<unsigned long long>(g_rIsDead), BodyGrabReachM, BodyGrabHitboxMarginM, BodyGrabHz,
        BodyGrabDamping, BodyGrabMaxAccel, BodyGrabLimbMaxAccel,
        LootByHand ? 1 : 0, LootByHand ? "the gather and the press machine run" : "*** OFF - no grab can start: body grab needs it ***",
        GetCurrentThreadId());
    return true;
}

void Refuse(int h, void* ai, const char* why)
{
    g_rr[h].pending = false;   // a refused press is over - a queued restart is not retried after it
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] REFUSED - %s hand, body %p: %s (the grip stays held from the game until it opens)", HandName(h),
            ai, why);
    }
}

bool ReadU32(uintptr_t a, uint32_t* out)
{
    __try { *out = *reinterpret_cast<const uint32_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallVis(void* ai, uintptr_t fn, void** vis)
{
    __try { *vis = reinterpret_cast<VFn0>(fn)(ai); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// IPhysicsBind vfunc +160 = the current physics (any type) and its vfunc 0 = the type. False = a fault.
bool CurrentPhysics(void* ai, void** cur, int* type)
{
    __try {
        *cur = nullptr;
        *type = -1;
        void* bind = g_physicsBind(static_cast<char*>(ai) + kCtlObjOff);
        if (!bind) { return true; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        *cur = reinterpret_cast<VFn0>(*reinterpret_cast<const uintptr_t*>(vt + kBindCurOff))(bind);
        if (*cur) {
            const uintptr_t pvt = *reinterpret_cast<const uintptr_t*>(*cur);
            *type = static_cast<int>(reinterpret_cast<unsigned(__fastcall*)(void*)>(*reinterpret_cast<const uintptr_t*>(pvt))(*cur));
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int ByteOr(uintptr_t a)   // -1 = unreadable
{
    unsigned char b = 0;
    return ReadU8(a, &b) ? b : -1;
}

// U-65: the corpse's state at a "no ragdoll" refusal. Read-only (two vcalls: the type-free physics getter and the vis getter, both
// getters); runs only at that refusal, under the event cap - nothing per frame. -1 / null = unreadable or absent, said per field.
void NoRagdollDiag(int h, void* ai)
{
    if (!EventLine()) { return; }
    const uintptr_t a = reinterpret_cast<uintptr_t>(ai);
    void* cur = nullptr;
    int curType = -1;
    const bool okCur = CurrentPhysics(ai, &cur, &curType);
    // the main physics config, by the engine's own exported accessors' offsets
    uintptr_t ico = a + kCtlObjOff, co = 0, cfg = 0, cfgOwner = 0, phys = 0, params = 0;
    uint32_t pending = 0, working = 0;
    const bool okCo = ReadPtr(ico + kCoOff, &co) && co;
    const bool okCfg = okCo && ReadPtr(co + kCoMainCfgOff, &cfg) && cfg;
    const bool okPend = okCfg && ReadU32(cfg + kCfgPendingOff, &pending);
    const bool okOwner = okCfg && ReadPtr(cfg + kCfgOwnerOff, &cfgOwner);
    const bool okPhys = okCfg && ReadPtr(cfg + kCfgPhysOff, &phys);
    const bool okWork = okPhys && phys && ReadU32(phys + kPhysTypeOff, &working);
    const bool okPar = okCfg && ReadPtr(cfg + kCfgParamsOff, &params);
    const int plod = (okPar && params) ? ByteOr(params + kParamsPlodOff) : -1;
    const int deferred = (okPar && params) ? ByteOr(params + kParamsDeferOff) : -1;
    uintptr_t gso = 0;
    const bool okGso = okCo && ReadPtr(co + kCoGsoFlagsOff, &gso);
    // the vis: only through the getter this build was read from
    uintptr_t vt = 0, getter = 0, visOwner = 0;
    void* vis = nullptr;
    const bool okGetter = ReadPtr(a, &vt) && ReadPtr(vt + 8 * kVisGetSlot, &getter);
    const bool getterOk = okGetter && getter == g_gd + g_rVisGet;
    const bool okVis = getterOk && CallVis(ai, getter, &vis) && vis;
    const uintptr_t v = reinterpret_cast<uintptr_t>(vis);
    const bool okVisOwner = okVis && ReadPtr(v + kVisOwnerOff, &visOwner);
    // the three +613CC0 gates and UpdateInDead's veto
    uint32_t flags = 0;
    const bool okFlags = ReadU32(a + kGateFlagsOff, &flags);
    uintptr_t pa = 0, pb = 0, pc = 0, pd = 0, b1 = 0, b2 = 0;
    uint32_t b3 = 0;
    const bool okVeh = ReadPtr(a + kVehAOff, &pa) && ReadPtr(a + kVehBOff, &pb) && ReadPtr(a + kVehCOff, &pc) && ReadPtr(a + kVehDOff, &pd);
    int veto = -1;
    if (okVeh) {
        if (pa && pb) {
            veto = (ReadPtr(pb + 1568, &b1) && ReadPtr(pb + 1576, &b2) && ReadU32(pb + 2048, &b3)) ? ((b1 && b2 && !b3) ? 1 : 0) : -1;
        } else {
            veto = (pc && pd) ? 1 : 0;
        }
    }
    // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
    LOG("bodygrab: [BODYGRAB] NO-RAGDOLL DIAG (U-65, read-only) - %s hand, body %p | current physics %p type %d (2 = ragdoll, -1 = "
        "none or unread; %s) | main config %p (owner %s): pending type %d, physics %p working type %d, sleeping by PLOD %d, "
        "deferred mark %d | control object GSO flags 0x%llX (%s): active on module %d, setup-blocking bits %d | vis %p "
        "(getter slot %zu %s; vis+80 %s): skip %d, ragdoll latch %d, physics-off mark %d | gates: AI debug menu %d, movie-phase bit "
        "%d, in-movie %d | UpdateInDead veto %d (pair A %d/%d, pair B %d/%d) | thread %lu",
        HandName(h), ai, cur, curType, okCur ? "read" : "FAULTED", reinterpret_cast<void*>(cfg),
        !okCfg ? "NOT READ" : !okOwner ? "unread" : cfgOwner == co ? "== the control object" : "*** != the control object ***",
        okPend ? static_cast<int>(pending) : -1, reinterpret_cast<void*>(phys), okWork ? static_cast<int>(working) : -1, plod,
        deferred, static_cast<unsigned long long>(gso), okGso ? "read" : "NOT READ", okGso ? ((gso & kGsoActive) ? 1 : 0) : -1,
        okGso ? ((gso & kGsoNoSetup) ? 1 : 0) : -1, vis,
        kVisGetSlot, !okGetter ? "unreadable" : getterOk ? "matches this build" : "*** mismatch - not called ***",
        !okVisOwner ? "unread" : visOwner == a ? "== the body" : "*** != the body ***",
        okVis ? ByteOr(v + kVisSkipOff) : -1, okVis ? ByteOr(v + kVisLatchOff) : -1, okVis ? ByteOr(v + kVisOffMarkOff) : -1,
        ByteOr(g_gd + g_rGateMenu), okFlags ? ((flags & 0x200u) ? 1 : 0) : -1, ByteOr(a + kGateMovieOff), veto,
        pa ? 1 : 0, pb ? 1 : 0, pc ? 1 : 0, pd ? 1 : 0, GetCurrentThreadId());
}

// The main physics config by the game's own idiom: PhysicsBind(AI+24) vfunc +208. False = a fault.
bool ConfigOf(void* ai, void** cfg)
{
    __try {
        *cfg = nullptr;
        void* bind = g_physicsBind(static_cast<char*>(ai) + kCtlObjOff);
        if (!bind) { return true; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        *cfg = reinterpret_cast<VFn0>(*reinterpret_cast<const uintptr_t*>(vt + kBindCfgOff))(bind);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallStartPhysics(void* cfg, void** ret)
{
    __try { *ret = g_startPhysics(cfg); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// U-65 option 2: restart a released ragdoll from the corpse's own config, once per press. True = restarted (*pOut = the ragdoll
// when the engine created it at once, else null and the press is left pending for the tick). False = not applicable or failed -
// the caller refuses as before. COST: one PhysicsBind + vcall, two reads, one engine call, one ragdoll query - per press only.
// relict (a severed limb): its own single-body config (type 4) is restarted the same way.
bool ReRagdollOnce(int h, void* ai, const float pt[3], double now, void** pOut, bool relict)
{
    *pOut = nullptr;
    void* cfg = nullptr;
    uint32_t pending = 0;
    uintptr_t phys = 0;
    const char* why = nullptr;
    if (!ConfigOf(ai, &cfg)) { why = "the config getter faulted"; }
    else if (!cfg) { why = "no physics config"; }
    else if (!ReadU32(reinterpret_cast<uintptr_t>(cfg) + kCfgPendingOff, &pending) ||
             (pending != kRagdollType && !(relict && pending == kSingleType))) {
        why = relict ? "its config is neither a ragdoll nor a single-body config" : "its config is not a ragdoll config";
    } else if (!ReadPtr(reinterpret_cast<uintptr_t>(cfg) + kCfgPhysOff, &phys) || phys) {
        why = "its config already holds a physics object";
    }
    void* ret = nullptr;
    const bool called = !why && CallStartPhysics(cfg, &ret);
    if (!why && !called) { why = "StartPhysics faulted"; }
    void* p = nullptr;
    const bool okP = called && RagdollOf(ai, &p, relict);
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] RE-RAGDOLL - %s hand, body %p: %s | config %p, pending type %d | StartPhysics returned %p | "
            "ragdoll now %p - %s | thread %lu",
            HandName(h), ai, why ? why : "restarted from its own ragdoll config", cfg, static_cast<int>(pending), ret, p,
            !called ? "not restarted" : p ? "back, the grab goes on" : okP ? "not yet: queued by the engine, retried for 0.5 s"
                                                                      : "the ragdoll getter faulted",
            GetCurrentThreadId());
    }
    if (why || !okP) { return false; }
    *pOut = p;
    if (!p) {
        g_rr[h].pending = true;
        g_rr[h].ai = ai;
        for (int k = 0; k < 3; ++k) { g_rr[h].pt[k] = pt[k]; }
        g_rr[h].startS = now;
    }
    return true;
}

// bodyGone = the engine's id lookup no longer returns the body (review F3): its memory may be freed, so it is not read for the line.
// ---- the stiff live hold (enemy_grab_stiff_hold; RESEARCH-ENEMY-GRAB §10) - game thread only ----
struct StiffJoint { void* j; int type; int axes; float vel[3]; float fmax[3]; };
struct StiffState {
    bool on;                      // this hand owns a stiffened ragdoll (applied, not yet restored or dropped)
    bool shared;                  // final review C1: the other hand already stiffened this ragdoll - this hand holds, writes nothing
    bool pending;                 // final review I4: the release could not validate (player not published) - retried every update
    void* ai;
    void* gso;
    int id;
    uintptr_t aiVt;
    void* p;                      // the ragdoll it was applied to - restored only while it is still the AI's current one
    int nb;
    void* b[kMaxBodies];
    int grav[kMaxBodies];         // each body's gravity mode before (1 = on)
    int nj;
    StiffJoint j[kMaxStiffJoints];
};
StiffState g_st[2];

int SafeJointType(void* jt)
{
    __try { return g_jointType(jt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int SafeJointsOf(void* body, void* out[], int max)
{
    __try {
        const int nj = g_numJoints(body);
        int n = 0;
        for (int k = 0; k < nj && n < max; ++k) { out[n++] = g_getJoint(body, k); }
        return nj > max ? -2 : n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// Reads the joint's current motor values (all of them first), then locks it. 1 = locked, 0 = the read faulted (nothing written,
// axes = -1), -1 = a write faulted after the read (final review M5: the values are recorded, so the release restores it anyway).
bool ReadJoint(StiffJoint& s)
{
    __try {
        if (s.type == kJointAmotor) {
            const int n = g_amAxes(s.j);
            if (n < 1 || n > 3) { return false; }
            for (int a = 0; a < n; ++a) {
                s.vel[a] = g_amGet(s.j, kParamVel | (a << 8));
                s.fmax[a] = g_amGet(s.j, kParamFMax | (a << 8));
            }
            s.axes = n;
        } else {
            s.vel[0] = g_hiGet(s.j, kParamVel);
            s.fmax[0] = g_hiGet(s.j, kParamFMax);
            s.axes = 1;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteJoint(const StiffJoint& s, float fmax)
{
    __try {
        for (int a = 0; a < s.axes; ++a) {
            if (s.type == kJointAmotor) {
                g_amSet(s.j, kParamVel | (a << 8), 0.0f);
                g_amSet(s.j, kParamFMax | (a << 8), fmax);
            } else {
                g_hiSet(s.j, kParamVel, 0.0f);
                g_hiSet(s.j, kParamFMax, fmax);
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int LockJoint(StiffJoint& s, float fmax)
{
    s.axes = -1;
    if (!ReadJoint(s)) { s.axes = -1; return 0; }
    return WriteJoint(s, fmax) ? 1 : -1;
}
bool UnlockJoint(const StiffJoint& s)
{
    __try {
        if (s.type == kJointAmotor) {
            for (int a = 0; a < s.axes; ++a) {
                g_amSet(s.j, kParamVel | (a << 8), s.vel[a]);
                g_amSet(s.j, kParamFMax | (a << 8), s.fmax[a]);
            }
        } else {
            g_hiSet(s.j, kParamVel, s.vel[0]);
            g_hiSet(s.j, kParamFMax, s.fmax[0]);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SetGravity(void* body, int on, int* before)
{
    __try {
        if (before) { *before = g_gravGet(body); }
        g_gravSet(body, on);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool StillInEngine(void* gso, int id);
bool SafeMass(void* body, float* m)
{
    __try { *m = g_getMass(body); return std::isfinite(*m) && *m > 0.0f; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The ragdoll's bodies and joints as they are NOW (final review I2: a part severed during the hold is gone - never written).
// False = the walk faulted / overflowed (the caller then writes nothing).
bool WalkNow(void* p, void* bodies[], int* nb, void* joints[], int* nj, int maxJ)
{
    *nb = 0;
    *nj = 0;
    if (!ListBodies(p, bodies, nb) || *nb <= 0) { return false; }
    for (int i = 0; i < *nb; ++i) {
        void* js[16] = {};
        const int n = SafeJointsOf(bodies[i], js, 16);
        if (n < 0) { return false; }
        for (int k = 0; k < n; ++k) {
            if (!js[k]) { continue; }
            bool seen = false;
            for (int q = 0; q < *nj && !seen; ++q) { seen = joints[q] == js[k]; }
            if (seen) { continue; }
            if (*nj >= maxJ) { return false; }
            joints[(*nj)++] = js[k];
        }
    }
    return true;
}
bool InList(const void* x, void* const l[], int n)
{
    for (int i = 0; i < n; ++i) { if (l[i] == x) { return true; } }
    return false;
}

// The release, validated before ANY read of the AI (final review I3 / I4). Returns false = could not decide yet (pending).
// COST: once per grab end (+ once per update while pending): an id lookup, a vftable read, the ragdoll getter, one walk.
bool StiffRelease(int h, const char* when)
{
    StiffState& s = g_st[h];
    if (!s.on) { return true; }
    const int o = 1 - h;
    // C1: the other hand still holds this same ragdoll - hand the record over, write nothing
    if (g_st[o].shared && g_g[o].active && g_g[o].p == s.p) {
        g_st[o] = s;
        g_st[o].shared = false;
        if (EventLine()) {
            // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=yes
            LOG("bodygrab: [BODYGRAB] STIFF HANDED - %s hand let go (%s); the %s hand still holds ragdoll %p - it stays stiff and "
                "weightless, restored when that hand lets go", HandName(h), when, HandName(o), s.p);
        }
        s = StiffState{};
        return true;
    }
    if (!g_player) {
        if (!s.pending && EventLine()) {
            // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=yes
            LOG("bodygrab: [BODYGRAB] STIFF RELEASE WAITING - %s hand (%s): the player is not published, the AI cannot be "
                "re-validated - retried every update", HandName(h), when);
        }
        s.pending = true;
        return false;
    }
    const char* verdict = nullptr;
    uintptr_t vt = 0;
    void* cur = nullptr;
    if (!StillInEngine(s.gso, s.id)) { verdict = "the AI is no longer returned by the engine's id lookup - NOT touched"; }
    else if (!ReadPtr(reinterpret_cast<uintptr_t>(s.ai), &vt) || vt != s.aiVt) { verdict = "the AI's vftable changed - NOT touched"; }
    else if (!RagdollOf(s.ai, &cur, false) || cur != s.p) {
        verdict = "the ragdoll is no longer the AI's current one (the game replaced it) - NOT touched";
    }
    int unlocked = 0, gravBack = 0, woken = 0, goneJ = 0, goneB = 0;
    if (!verdict) {
        void* bodies[kMaxBodies] = {};
        void* joints[kMaxStiffJoints * 2] = {};
        int nb = 0, nj = 0;
        if (!WalkNow(s.p, bodies, &nb, joints, &nj, kMaxStiffJoints * 2)) {
            verdict = "the ragdoll's walk faulted - NOT touched";
        } else {
            for (int q = 0; q < s.nj; ++q) {
                if (s.j[q].type == kJointBall || s.j[q].axes <= 0) { continue; }
                if (!InList(s.j[q].j, joints, nj)) { ++goneJ; continue; }
                if (UnlockJoint(s.j[q])) { ++unlocked; }
            }
            for (int i = 0; i < s.nb; ++i) {
                if (s.grav[i] < 0) { continue; }
                if (!InList(s.b[i], bodies, nb)) { ++goneB; continue; }
                if (!SetGravity(s.b[i], s.grav[i], nullptr)) { continue; }
                ++gravBack;
                // M7: a body the engine put to rest while weightless would hang in the air - wake it with the force it needs
                float m = 0.0f, thr = -1.0f, at[3] = {};
                bool resting = false;
                if (SafeMass(s.b[i], &m)) { WakeState(s.b[i], m, &resting, &thr); }
                if (resting && thr > 0.0f && BodyPos(s.b[i], at)) {
                    const float f[3] = { 0.0f, -1.05f * thr, 0.0f };
                    if (AddForce(s.b[i], f, at)) { ++woken; }
                }
            }
            verdict = unlocked || gravBack ? "the same ragdoll is current - restored, it falls naturally"
                                           : "the same ragdoll is current, but nothing restored (all writes failed or parts gone)";
        }
    }
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] STIFF END - %s hand (%s)%s | %s | motors restored %d, gravity restored on %d of %d bodies, %d woken | "
            "gone since START: %d motors, %d bodies (not written)", HandName(h), when, s.pending ? " after waiting" : "", verdict,
            unlocked, gravBack, s.nb, woken, goneJ, goneB);
    }
    s = StiffState{};
    return true;
}

// At a live grab's START (g_g[h] filled). COST (once per grab): one body list, <= ~30 joint reads, <= ~13 motor writes, <= ~15
// gravity writes.
void StiffBegin(int h)
{
    StiffState& s = g_st[h];
    const Grab& g = g_g[h];
    if (s.on && !StiffRelease(h, "a new grab on this hand")) {   // a release still waiting: never overwrite its record
        if (EventLine()) {
            // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=yes
            LOG("bodygrab: [BODYGRAB] STIFF START skipped - %s hand: the previous hold's release is still waiting", HandName(h));
        }
        return;
    }
    s = StiffState{};
    if (!EnemyGrabStiffHold) { return; }
    void* const ai = g.ai;
    void* const p = g.p;
    const int o = 1 - h;
    if ((g_st[o].on || g_st[o].shared) && g_g[o].active && g_g[o].p == p) {   // C1: the other hand already holds it stiff
        s.shared = true;
        if (EventLine()) {
            // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=yes
            LOG("bodygrab: [BODYGRAB] STIFF SHARED - %s hand grabbed ragdoll %p, already stiff from the %s hand - nothing written",
                HandName(h), p, HandName(o));
        }
        return;
    }
    s.ai = ai;
    s.gso = g.gso;
    s.id = g.id;
    s.aiVt = g.aiVt;
    const char* why = nullptr;
    if (!g_jointType || !g_amAxes || !g_amGet || !g_amSet || !g_hiGet || !g_hiSet || !g_gravGet || !g_gravSet) {
        why = "an engine export is missing (see the STIFF HOLD line at READY)";
    }
    void* bodies[kMaxBodies] = {};
    int nb = 0;
    if (!why && (!ListBodies(p, bodies, &nb) || nb <= 0)) { why = nb == -2 ? "the body count is out of range" : "the body list faulted"; }
    // the joint walk: every body's basic joints, de-duplicated; the type of each
    int counts[16] = {}, other = 0, faults = 0;
    void* seenJ[kMaxStiffJoints * 2] = {};   // final review M6: every joint de-duplicated, whatever its type
    int nSeen = 0;
    bool overflow = false;
    for (int i = 0; !why && i < nb; ++i) {
        void* js[16] = {};
        const int n = SafeJointsOf(bodies[i], js, 16);
        if (n < 0) { ++faults; continue; }
        for (int k = 0; k < n; ++k) {
            if (!js[k] || InList(js[k], seenJ, nSeen)) { continue; }
            if (nSeen >= kMaxStiffJoints * 2) { overflow = true; continue; }
            seenJ[nSeen++] = js[k];
            const int t = SafeJointType(js[k]);
            if (t >= 0 && t < 16) { ++counts[t]; } else { ++other; }
            if (t != kJointAmotor && t != kJointHinge && t != kJointBall) { continue; }
            if (s.nj >= kMaxStiffJoints) { overflow = true; continue; }
            s.j[s.nj++] = StiffJoint{ js[k], t, t == kJointBall ? -1 : 0, {}, {} };   // a ball is tracked, never written
        }
    }
    if (!why && overflow) { why = "more joints than the record holds - nothing written"; }
    int unknownTypes = other;
    for (int t = 0; t < 16; ++t) { if (t != kJointBall && t != kJointHinge && t != kJointAmotor) { unknownTypes += counts[t]; } }
    // The safety gate (RESEARCH §10 10p - the type values are the Mac reading): only ball / hinge / angular motor, one motor per ball,
    // at least one motor. ragdoll_ai.phx intact = 9 ball + 9 motor + 4 hinge.
    if (!why && (faults || unknownTypes || counts[kJointAmotor] < 1 || counts[kJointAmotor] != counts[kJointBall])) {
        why = "the joint walk is not the ragdoll the stiffening was read for (types below) - nothing written";
    }
    int locked = 0, lockFail = 0, gravOff = 0;
    float oldF = -1.0f;
    if (!why) {
        for (int q = 0; q < s.nj; ++q) {
            if (s.j[q].type == kJointBall) { continue; }
            const int lk = LockJoint(s.j[q], EnemyGrabStiffForce);   // 0 = nothing written (axes -1); -1 = recorded, restored anyway
            if (lk == 1) { ++locked; } else { ++lockFail; }
            if (lk != 0 && oldF < 0.0f) { oldF = s.j[q].fmax[0]; }
        }
        s.nb = nb;
        for (int i = 0; i < nb; ++i) {
            s.b[i] = bodies[i];
            s.grav[i] = -1;
            if (SetGravity(bodies[i], 0, &s.grav[i])) { ++gravOff; }
        }
        s.on = true;
        s.p = p;
    }
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=enemy_grab_stiff_hold  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] STIFF START - %s hand, AI %p, ragdoll %p | %s | bodies %d, joints: ball %d, hinge %d, motor %d, other "
            "types %d, walk faults %d | motors locked %d (failed %d), max force %.2f -> %.2f | gravity off on %d of %d bodies",
            HandName(h), ai, p, why ? why : "APPLIED", nb, counts[kJointBall], counts[kJointHinge], counts[kJointAmotor], unknownTypes,
            faults, locked, lockFail, static_cast<double>(oldF), static_cast<double>(EnemyGrabStiffForce), gravOff, nb);
    }
    if (why) { s = StiffState{}; }
}

// Every held update, after Drive's own re-validation (same AI, same current ragdoll): pull each part still in the ragdoll down by
// enemy_grab_weight of its weight. COST: one body list (<= 15 vfunc calls) + one mass read and one force per part.
void StiffWeight(int h)
{
    StiffState& s = g_st[h];
    if (!s.on || !(EnemyGrabWeight > 0.0f)) { return; }
    void* bodies[kMaxBodies] = {};
    int nb = 0;
    if (!ListBodies(s.p, bodies, &nb) || nb <= 0) { return; }
    for (int i = 0; i < s.nb; ++i) {
        if (!InList(s.b[i], bodies, nb)) { continue; }
        float m = 0.0f, at[3] = {};
        if (!SafeMass(s.b[i], &m) || !BodyPos(s.b[i], at)) { continue; }
        const float f[3] = { 0.0f, bodygrab::HeldWeightForce(m, EnemyGrabWeight), 0.0f };
        if (f[1] != 0.0f) { AddForce(s.b[i], f, at); }
    }
}

void Finish(int h, const char* why, bool bodyGone = false)
{
    Grab& g = g_g[h];
    InterlockedExchange(&g_active[h], 0);
    if (!g.active) { return; }
    g.active = false;
    // the stiff hold: a sharing hand just stops sharing; the owner releases through the validated path (it re-checks the AI by id,
    // vftable and current ragdoll itself - `bodyGone` is not trusted either way, final review I3 / I4)
    g_st[h].shared = false;
    if (g_st[h].on) { StiffRelease(h, why ? why : "?"); }
    (void)bodyGone;
    if (g.burst == 1) {
        g.burst = -1;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HOLD BURST %ld CUT SHORT - the hold ended after %ld of %ld updates (%d unwritten)", g.bSet,
            g.bCount, kBurstSamples, g.bFill);
    }
    float wp[3] = {}, pv[3] = { -1.0f, -1.0f, -1.0f };
    const bool okV = !bodyGone && BodyVec(g_localToWorld, g.body, g.local, wp) && BodyVec(g_getPointVel, g.body, wp, pv)
                     && Finite3(pv);
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] END - %s hand: %s | held %.2f s | point velocity %.2f m/s (%s) - kept by the body, no velocity "
            "set | body %p", HandName(h), why ? why : "?", NowS() - g.startS, okV ? Len3(pv) : -1.0f,
            bodyGone ? "NOT READ - not returned by the id lookup" : okV ? "read" : "NOT READ - body gone", g.body);
    }
}

// One level query around `hand` for severed limbs; publishes the nearest (or none) for looting's press. Returns it (null = none or
// the query could not run), *distOut = its hitbox distance. Game thread only (the engine query and the shared result vector).
void* RelictQuery(int h, const float hand[3], float* distOut);
bool CallGetId(void* gso, int* out);
bool StillInEngine(void* gso, int id);
bool ReadHand(int h, float pos[3], double* t, float rot[4] = nullptr);

bool ReadBodyRot(void* body, float q[4], float w[3])
{
    __try {
        const float* a = g_bodyQuat(body);
        const float* b = g_angVel(body);
        if (!a || !b) { return false; }
        for (int i = 0; i < 4; ++i) { q[i] = a[i]; }
        for (int i = 0; i < 3; ++i) { w[i] = b[i]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadInertia(void* body, float I[9])
{
    __try {
        const float* f = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(body) + kBodyInertiaOff);
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { I[3 * r + c] = f[4 * r + c]; } }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AddTorqueTo(void* body, const float t[3])
{
    __try { g_addTorque(body, t); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 10-24 (2) (Jay: "Build fix 1"): a press the corpse refuses goes to a severed limb at the hand instead, in the same press - one
// fresh level query (the per-hand reading may not have landed yet: hitbox_dump_run1). True = a limb was within
// body_grab_hitbox_margin_m and its grab was started (Start logs its own START / REFUSED). COST: one query, refused presses only.
void Start(int h, void* ai, const float pt[3], double now, bool live = false);
bool LimbInstead(int h, void* corpse, const float pt[3], double now, const char* why)
{
    float d = -1.0f;
    void* rel = RelictQuery(h, pt, &d);
    const bool take = rel && d >= 0.0f && d <= BodyGrabHitboxMarginM;
    if (EventLine()) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] LIMB FALLBACK - %s hand, body %p refused (%s) | fresh level query: nearest severed limb %p at %.3f "
            "m (-1 = none), margin %.2f -> %s", HandName(h), corpse, why, rel, rel ? d : -1.0f, BodyGrabHitboxMarginM,
            take ? "taking the severed limb instead" : rel ? "too far - the press stays refused" : "none - the press stays refused");
    }
    if (!take) { return false; }
    Start(h, rel, pt, now);
    return true;
}

// live (plan 2026-10-31-open-hand-push-and-grab Task 4): a LIVE enemy the game has knocked down (pushgrab.cpp asked SetShouldFall and
// waited for its ragdoll) - no "must be dead" test and no restart of the ragdoll: its ragdoll must already be up, else refused.
void Start(int h, void* ai, const float pt[3], double now, bool live)
{
    Grab& g = g_g[h];
    const uintptr_t a = reinterpret_cast<uintptr_t>(ai);
    uintptr_t vt = 0, fn = 0, dfn = 0;
    if (!ai || !ReadPtr(a, &vt)) { Refuse(h, ai, "unreadable"); return; }
    // A severed limb (AIRelict) has no AI / death to check - and its vftable is 28 slots, so slots 37 / 179 are never read on it.
    const bool relict = vt == g_gd + g_rRelictVt;
    if (!relict) {
        if (!ReadPtr(vt + 8 * kTrigActSlot, &fn)) { Refuse(h, ai, "unreadable"); return; }
        if (fn != g_gd + g_rAiTrigAct) { Refuse(h, ai, "not an AI (a standalone container: loot only, no ragdoll)"); return; }
        if (!ReadPtr(vt + 8 * kIsDeadSlot, &dfn)) { Refuse(h, ai, "vftable unreadable"); return; }
        if (dfn != g_gd + g_rIsDead) {
            if (EventLine()) {
                // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
                LOG("bodygrab: [BODYGRAB] REFUSED - %s hand, body %p: not a HumanAI - IsDead slot %zu is +%llX, not +%llX (a base "
                    "AI answers 0 there), so its death cannot be read", HandName(h), ai, kIsDeadSlot,
                    static_cast<unsigned long long>(dfn - g_gd), static_cast<unsigned long long>(g_rIsDead));
            }
            return;
        }
    }
    // Review F3 (INV-663; Jay 10-31: "Drive + Start"): the press's body came from looting's gather (up to ~0.5 s old) or a queued
    // restart - the id the engine knows it by, and the lookup must return it NOW, before any call into it. Else it could never be
    // re-validated while held, so it is not grabbed (fails closed, named). Also the per-press positive control of Drive's check.
    void* gso = static_cast<char*>(ai) + kGsoOff;
    int id = 0;
    if (!CallGetId(gso, &id) || !StillInEngine(gso, id)) {
        g_rr[h].pending = false;
        if (EventLine()) {
            // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
            LOG("bodygrab: [BODYGRAB] REFUSED - %s hand, %s %p: the engine's id lookup does not return it (id %d, player %s) - it "
                "could not be re-validated while held (review F3; the grip stays held from the game until it opens)", HandName(h),
                relict ? "severed limb" : "body", ai, id, g_player ? "published" : "NOT published");
        }
        return;
    }
    if (!relict && !live) {
        bool dead = false;
        if (!CallIsDead(ai, &dead)) { Refuse(h, ai, "IsDead faulted"); return; }
        if (!dead) { Refuse(h, ai, "alive (IsDead reads false)"); return; }
    }
    // The visible skeleton: the bone segment under the hand (logged; the part when no hitbox is readable).
    SkelPick sk = {};
    sk.limb = sk.child = sk.bodyIdx = -1;
    sk.bodyD = -1.0f;
    SkelLimb(ai, pt, &sk);   // the AI is its own IModelObject
    void* p = nullptr;
    if (!RagdollOf(ai, &p, relict)) { g_rr[h].pending = false; Refuse(h, ai, "PhysicsBind / ragdoll getter faulted"); return; }
    if (!p && live) { Refuse(h, ai, "a live enemy whose ragdoll is not up (the game has not knocked it down)"); return; }
    if (!p) {
        // No physics = no hitboxes yet. 10-24 (2) (Jay: "the bone fix"): no bone-line gate here - a cut corpse keeps the bones of a
        // part cut off it (hitbox_dump_run1: refused at 18:01:56 against l_calf, a leg that was gone), and only a physics part
        // says what is still attached. The press already passed loot's box reach; the ragdoll is restarted and its own hitboxes
        // decide below, so a cut part - with no hitbox - never counts.
        if (g_rr[h].pending && g_rr[h].ai == ai) {
            if (now - g_rr[h].startS < kReRagdollS) { return; }   // still queued - BodyGrabTick calls Start again
            g_rr[h].pending = false;
            Refuse(h, ai, "no ragdoll (PhysicsBind vfunc +168 is null) - still none 0.5 s after the restart");
            if (!relict) { NoRagdollDiag(h, ai); LimbInstead(h, ai, pt, now, "no ragdoll after the restart"); }   // DIAG reads a dead AI's fields
            return;
        }
        if (!ReRagdollOnce(h, ai, pt, now, &p, relict)) {
            Refuse(h, ai, "no ragdoll (PhysicsBind vfunc +168 is null)");
            if (!relict) { NoRagdollDiag(h, ai); LimbInstead(h, ai, pt, now, "no ragdoll"); }
            return;
        }
        if (!p) { return; }   // queued by the engine - pending for the tick
    }
    g_rr[h].pending = false;
    void* body = nullptr;
    if (!NearestBody(p, pt, &body)) { Refuse(h, ai, "the nearest-body query faulted"); return; }
    if (!body) { Refuse(h, ai, "the ragdoll has no body near the hand (vfunc +464 is null)"); return; }
    LimbPick lp;
    void* limb = PickAlongLimbs(p, pt, body, &lp);   // kept for the log, and the fallback when no hitbox / skeleton is readable
    void* skel = sk.ok == 1 ? MapLimbToBody(p, &sk) : nullptr;
    // The hitboxes decide when any is readable: the part = the body owning the hitbox nearest the hand; the gate = the hand within
    // body_grab_hitbox_margin_m of that hitbox's surface. A severed limb is measured against its own shape only - it is a separate
    // object with its own physics, so a corpse's hitboxes never include it.
    HitPick hp;
    NearestHitbox(p, pt, &hp);
    DumpHitboxes(h, ai, relict, hp, pt,
                 hp.shapes <= 0 || !hp.body ? "no hitbox measured - the old pick decides"
                 : hp.d > BodyGrabHitboxMarginM ? "REFUSED - not on a hitbox" : "taken by its hitbox");
    if (hp.shapes > 0 && hp.body) {
        if (hp.d > BodyGrabHitboxMarginM) {
            if (EventLine()) {
                // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
                LOG("bodygrab: [BODYGRAB] REFUSED - %s hand, %s %p: hand not on a hitbox - the nearest (body index %d, %s %.3f %.3f "
                    "%.3f) is %.3f m outside it, past body_grab_hitbox_margin_m %.2f | %d hitboxes on %d bodies (the grip stays held "
                    "from the game until it opens)", HandName(h), relict ? "severed limb" : "body", ai, hp.idx, ShapeName(hp.shape),
                    hp.prm[0], hp.prm[1], hp.prm[2], hp.d, BodyGrabHitboxMarginM, hp.shapes, hp.bodies);
            }
            if (!relict) { LimbInstead(h, ai, pt, now, "hand not on a hitbox"); }
            return;
        }
        body = hp.body;
    } else if (skel) {
        body = skel;
    } else if (limb) {
        body = limb;
    }
    int idx = -1, count = -1;
    if (!BodyIndex(p, body, &idx, &count)) { Refuse(h, ai, "the body list faulted"); return; }
    float mass = 0.0f;
    if (!BodyMass(body, &mass) || !std::isfinite(mass) || mass <= 0.0f) { Refuse(h, ai, "no usable mass"); return; }
    // 10-24 (2) (Jay: grabbed parts should "latch to the palm"): the grabbed point is the point of the part's hitbox surface nearest
    // the hand (the hand itself when inside it) - that point is pulled INTO the hand, not held where it was up to the margin away.
    float palm[3] = { pt[0], pt[1], pt[2] };
    const bool onHitbox = hp.shapes > 0 && hp.body == body && hp.shape > 0;
    if (onHitbox) {
        bodygrab::ClosestOnShape(static_cast<bodygrab::Shape>(hp.shape), hp.c, hp.q, hp.prm, pt, palm);
        if (!Finite3(palm)) { palm[0] = pt[0]; palm[1] = pt[1]; palm[2] = pt[2]; }
    }
    const float palmGap[3] = { palm[0] - pt[0], palm[1] - pt[1], palm[2] - pt[2] };
    float local[3] = {}, bpos[3] = {};
    if (!BodyVec(g_worldToLocal, body, palm, local) || !Finite3(local)) { Refuse(h, ai, "PointWorldToLocal faulted"); return; }
    const bool havePos = BodyPos(body, bpos);
    bool resting = false;
    float thr = -1.0f;
    WakeState(body, mass, &resting, &thr);
    g = Grab{};
    g.active = true;
    g.ai = ai;
    g.gso = gso;
    g.id = id;
    g.aiVt = vt;
    g.p = p;
    g.body = body;
    for (int k = 0; k < 3; ++k) { g.local[k] = local[k]; }
    g.mass = mass;
    g.startS = now;
    g.winS = now;
    g.relict = relict;
    g.live = live;
    // the rotation hold: the limb's centre and rotation in the hand's frame, now
    // Corpses too since 10-30 (8) (Jay, the corpse shake: "Refer to how the shake was fixed for the limbs & apply the same logic"):
    // the held part's centre + rotation follow the hand, pushed once per physics step - corpses keep their own weight (CapAccel).
    const char* holdWhy = "OFF - an engine export is missing";
    if (g_bodyQuat && g_angVel && g_addTorque) {
        float hp0[3] = {}, hq[4] = {}, bq[4] = {}, bw[3] = {};
        double ht = 0.0;
        holdWhy = "OFF - no fresh hand pose";
        if (havePos && ReadHand(h, hp0, &ht, hq) && ht > 0.0 && now - ht <= kStaleS) {
            holdWhy = "OFF - the body's rotation / inertia did not read";
            const bool okRot = ReadBodyRot(body, bq, bw) && ReadInertia(body, g.inertia);
            const float qn = bq[0] * bq[0] + bq[1] * bq[1] + bq[2] * bq[2] + bq[3] * bq[3];
            const float hn = hq[0] * hq[0] + hq[1] * hq[1] + hq[2] * hq[2] + hq[3] * hq[3];
            bool okI = okRot;
            for (int r = 0; r < 3 && okI; ++r) { okI = std::isfinite(g.inertia[4 * r]) && g.inertia[4 * r] > 0.0f; }
            if (okI && qn > 0.9f && qn < 1.1f && hn > 0.9f && hn < 1.1f) {
                float hc[4] = {};
                bodygrab::QuatConj(hq, hc);
                bodygrab::QuatMul(hc, bq, g.qRel);
                // the centre's offset from the PALM POINT, in the hand's frame: the hold then puts that point in the hand
                const float d[3] = { bpos[0] - palm[0], bpos[1] - palm[1], bpos[2] - palm[2] };
                bodygrab::QuatRotate(hq, d, true, g.comLocal);
                g.hold = true;
                holdWhy = relict ? "ON - the limb turns with the hand" : "ON - the corpse part turns with the hand";
            }
        }
    }
    InterlockedExchange(&g_active[h], 1);
    if (EventLine()) {
        const float d[3] = { pt[0] - bpos[0], pt[1] - bpos[1], pt[2] - bpos[2] };
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
        LOG("bodygrab: [BODYGRAB] START - %s hand | target %s | AI %p, ragdoll %p, body %p = index %d of %d | mass %.2f kg | "
            "hand to the body's centre %.3f m (%s) | grab point local (%.3f, %.3f, %.3f) | resting flag %d, wake force %.1f N (-1 = unreadable) vs the "
            "cap %.1f N | taken by %s | hitbox: body index %d, %s (%.3f %.3f %.3f), hand %.3f m from its surface (<= 0 inside; "
            "margin %.2f) - %d hitboxes (%d sphere, %d capsule, %d box) on %d bodies, %d other, %d not on their body | skeleton: "
            "bone %s (element %d -> %d, %d bones of %d elements) %.3f m from the hand at t "
            "%.2f, part index %d centre %.3f m from that bone, box raw (%.2f %.2f %.2f / %.2f %.2f %.2f) | limb pick: segment %d-%d "
            "at t %.2f, hand %.3f m from it | the engine's centre pick index %d at %.3f m (%d bodies, %d joint links) | rotation "
            "hold %s, inertia diag (%.4f %.4f %.4f), centre in the hand's frame (%.3f %.3f %.3f) | palm point %s, %.3f m from the "
            "hand (pulled into it) | thread %lu",
            HandName(h), relict ? "a severed limb" : live ? "a live enemy (knocked down)" : "a corpse", ai, p, body, idx, count, mass, havePos ? Len3(d) : -1.0f,
            havePos ? "read" : "position NOT READ", local[0], local[1], local[2], resting ? 1 : 0, thr, mass * CapAccel(relict),
            (hp.shapes > 0 && hp.body) ? "its hitbox"
            : skel ? "the skeleton (no hitbox readable)" : limb ? "the limb pick (no hitbox, no skeleton)"
            : lp.n < 0 ? "the engine's (body list faulted)" : "the engine's (no hitbox, no skeleton, no joint links)",
            hp.idx, ShapeName(hp.shape), hp.prm[0], hp.prm[1], hp.prm[2], hp.d, BodyGrabHitboxMarginM, hp.shapes, hp.spheres,
            hp.capsules, hp.boxes, hp.bodies, hp.other, hp.unmatched,
            sk.ok == 1 ? (sk.name[0] ? sk.name : "unnamed") : sk.ok < 0 ? "FAULTED" : "none", sk.limb, sk.child, sk.bones, sk.n,
            sk.d, sk.t, sk.bodyIdx, sk.bodyD, sk.box[0], sk.box[1], sk.box[2], sk.box[3], sk.box[4], sk.box[5], lp.a, lp.b, lp.t,
            lp.d, lp.engIdx, lp.engD, lp.n, lp.links, holdWhy, g.inertia[0], g.inertia[4], g.inertia[8], g.comLocal[0],
            g.comLocal[1], g.comLocal[2], onHitbox ? "on the hitbox surface" : "the hand point (no hitbox)", Len3(palmGap),
            GetCurrentThreadId());
    }
    // [LOADBEARING: enemy_grab_stiff_hold] the B&S-like hold - a live enemy's ragdoll locked + weightless while held (RESEARCH §10)
    if (live) { StiffBegin(h); }
}

// Read the hand's newest published sample (and its rotation when `rot` is given). False = none consistent this call.
bool ReadHand(int h, float pos[3], double* t, float rot[4])
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_pubSeq[h];
        if (s0 & 1) { continue; }
        pos[0] = g_pubPos[h][0]; pos[1] = g_pubPos[h][1]; pos[2] = g_pubPos[h][2];
        if (rot) { for (int k = 0; k < 4; ++k) { rot[k] = g_pubRot[h][k]; } }
        *t = g_pubT[h];
        if (g_pubSeq[h] == s0) { return true; }
    }
    return false;
}

// The SEARCH ZONE of an unsearched corpse = every bone but an arm's or a leg's END (Jay 2026-10-27 after run 10: "If I wanted to drag
// it first, it would usually be by the arms/legs" -> "Alright, go ahead"). Run 10: 5 presses inside a searchable body's box dragged it
// because the nearest bone was not spine / pelvis (the old rule: neck, clavicle, upper arm, thigh, cloth bones all read "limb"). Now
// only a forearm / foretwist / hand / finger / calf / foot / toe bone under the hand drags; looting still never searches a body
// already searched (its state), so after the loot a grab anywhere drags. Name match is a substring, either side (l_ / r_).
bool IsTorsoBone(const char* name)
{
    static const char* const kEnds[] = { "forearm", "foretwist", "hand", "finger", "calf", "foot", "toe" };
    char low[64] = {};
    for (int i = 0; i < 63 && name[i]; ++i) { low[i] = static_cast<char>(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]); }
    for (const char* e : kEnds) {
        if (std::strstr(low, e)) { return false; }
    }
    return true;
}

// Game thread, per player update and hand: the bone under the hand of the body looting says is nearest. COST: nothing while no body
// is in reach or the hand holds one; else at most every kTorsoEveryMs one SkelLimb (~60-100 elements: IsElementABone each,
// GetElementWorldPos + GetElementParent per bone) - ~20 a second per hand near a body.
void TorsoTrack(int h)
{
    void* b = g_cand[h];
    if (!b || g_g[h].active) { return; }
    const ULONGLONG ms = GetTickCount64();
    if (ms - g_torLastRead[h] < kTorsoEveryMs) { return; }
    g_torLastRead[h] = ms;
    uintptr_t vt = 0, fn = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(b), &vt) || !ReadPtr(vt + 8 * kTrigActSlot, &fn) || fn != g_gd + g_rAiTrigAct) {
        return;   // not an AI (a standalone container) - no skeleton to read
    }
    float hand[3] = {};
    double t = 0.0;
    if (!ReadHand(h, hand, &t) || t <= 0.0 || NowS() - t > kStaleS) { return; }
    SkelPick sk = {};
    sk.limb = sk.child = sk.bodyIdx = -1;
    SkelLimb(b, hand, &sk);
    // The corpse's measure against a severed limb (looting compares the two): its nearest hitbox's surface while its ragdoll is up,
    // else none (-1). Not the gather's box - a hand anywhere inside a lying corpse's box reads 0 there - and not its skeleton,
    // which keeps the bones of a part cut off it (hitbox_dump_run1 14-16: l_thigh / l_calf / l_foot listed, no physics part).
    float hit = -1.0f;
    void* tp = nullptr;
    if (RagdollOf(b, &tp, false) && tp) {
        HitPick hp;
        NearestHitbox(tp, hand, &hp);
        if (hp.shapes > 0 && hp.body) { hit = hp.d > 0.0f ? hp.d : 0.0f; }
    }
    if (sk.ok != 1 && hit < 0.0f) { return; }
    const int torso = sk.ok == 1 ? (IsTorsoBone(sk.name) ? 1 : 0) : -1;
    InterlockedIncrement(&g_torSeq[h]);   // odd: writing
    g_torBody[h] = b;
    g_torIs[h] = torso;
    g_torHit[h] = hit;
    g_torMs[h] = ms;
    InterlockedIncrement(&g_torSeq[h]);   // even: settled
    if (sk.ok == 1 && InterlockedIncrement(&g_nTorsoReads) == 1) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] TORSO first reading (logged unconditionally) - %s hand, body %p: bone %s (element %d) %.3f m from the "
            "hand -> %s | refreshed at most every %llu ms, trusted for %llu ms | thread %lu",
            HandName(h), b, sk.name[0] ? sk.name : "unnamed", sk.limb, sk.d, torso ? "TORSO (a press may search)" : "a limb (a press grabs)",
            static_cast<unsigned long long>(kTorsoEveryMs), static_cast<unsigned long long>(kTorsoFreshMs), GetCurrentThreadId());
    }
}

bool IsRelictObj(void* obj)
{
    uintptr_t vt = 0;
    return obj && g_gd && ReadPtr(reinterpret_cast<uintptr_t>(obj), &vt) && vt == g_gd + g_rRelictVt;
}
bool LevelOf(void* player, void** level)
{
    __try { *level = g_getLevel(static_cast<char*>(player) + kGsoOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool QueryRadius(void* level, const float c[3], float r)
{
    __try { return g_findRadius(level, &g_relVec, c, r, nullptr, false, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallGetId(void* gso, int* out)
{
    __try { *out = g_getId(gso); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallFindById(void* gso, int id, void** out)
{
    __try { *out = g_findById(gso, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Review F3 (INV-663), handcoll's StillInEngine: is `gso` still the live object the engine knows by `id`? Asked through the
// PLAYER's IGSObject (the lookup reads the caller's level only), so the body's own memory is never read - a destroyed object is
// skipped by the engine's id tree the moment it is flagged, before it is freed. A fault or any other answer = no (fails closed).
bool StillInEngine(void* gso, int id)
{
    void* pl = g_player;
    void* found = nullptr;
    return pl && gso && CallFindById(static_cast<char*>(pl) + kGsoOff, id, &found) && found == gso;
}
// The hand's distance to a severed limb: its own hitbox's surface when readable, else its skeleton when it has bones, else its
// nearest ragdoll body centre (-1 = unreadable).
float RelictDist(void* obj, const float hand[3])
{
    // its own hitbox first (inside = 0), the same measure the corpse is compared by; then its skeleton; then its body centre
    void* hp0 = nullptr;
    if (RagdollOf(obj, &hp0, true) && hp0) {
        HitPick hp;
        NearestHitbox(hp0, hand, &hp);
        if (hp.shapes > 0 && hp.body) { return hp.d > 0.0f ? hp.d : 0.0f; }
    }
    SkelPick sk = {};
    sk.limb = sk.child = sk.bodyIdx = -1;
    SkelLimb(obj, hand, &sk);
    if (sk.ok == 1) { return sk.d; }
    void* p = nullptr;
    void* bodies[kMaxBodies] = {};
    int n = 0;
    if (!RagdollOf(obj, &p, true) || !p || !ListBodies(p, bodies, &n) || n <= 0) { return -1.0f; }
    float best = -1.0f;
    for (int i = 0; i < n; ++i) {
        float c[3] = {};
        if (!bodies[i] || !BodyPos(bodies[i], c) || !Finite3(c)) { continue; }
        const float dv[3] = { hand[0] - c[0], hand[1] - c[1], hand[2] - c[2] };
        const float d = Len3(dv);
        if (best < 0.0f || d < best) { best = d; }
    }
    return best;
}
// Game thread, per player update and hand: the severed limb nearest the hand, published for looting's press. COST: nothing while
// the grip is under kRelictSqueeze, the hand holds a body, or the exports are missing; else at most every kRelictEveryMs one
// engine level query (radius kRelictRadiusM) + a vftable read per hit + RelictDist per severed limb found.
void RelictTrack(int h)
{
    if (!g_findRadius || !g_getLevel || g_g[h].active) { return; }
    const bool asked = InterlockedExchange(&g_relReq[h], 0) != 0;   // a press: query now, whatever the squeeze / the rate
    const LONG bits = g_squeezeBits[h];
    float sq = 0.0f;
    std::memcpy(&sq, &bits, sizeof(sq));
    if (!asked && !(sq >= kRelictSqueeze)) { return; }
    const ULONGLONG ms = GetTickCount64();
    if (!asked && ms - g_relLastQuery[h] < kRelictEveryMs) { return; }
    g_relLastQuery[h] = ms;
    float hand[3] = {};
    double t = 0.0;
    if (!ReadHand(h, hand, &t) || t <= 0.0 || NowS() - t > kStaleS) { return; }
    float d = -1.0f;
    RelictQuery(h, hand, &d);
}
void* RelictQuery(int h, const float hand[3], float* distOut)
{
    *distOut = -1.0f;
    void* pl = g_player;
    void* level = nullptr;
    if (!g_findRadius || !g_getLevel || !pl) { return nullptr; }
    if (!LevelOf(pl, &level) || !level || !QueryRadius(level, hand, kRelictRadiusM)) { return nullptr; }
    const ULONGLONG ms = GetTickCount64();
    float sq = 0.0f;
    const LONG bits = g_squeezeBits[h];
    std::memcpy(&sq, &bits, sizeof(sq));
    const LONG q = InterlockedIncrement(&g_nRelictQueries);
    const int hits = static_cast<int>(g_relVec.count);
    const int n = hits < kMaxRelictHits ? hits : kMaxRelictHits;
    void* best = nullptr;
    float bd = 1.0e30f;
    int nRel = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t e = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(g_relVec.data) + 8 * static_cast<uintptr_t>(i), &e) || !e) { continue; }
        void* obj = reinterpret_cast<void*>(e - kCtlObjOff);   // the entry is the object's IControlObject (+24 in an AIRelict)
        if (!IsRelictObj(obj)) { continue; }
        ++nRel;
        const float d = RelictDist(obj, hand);
        if (d >= 0.0f && d < bd) { bd = d; best = obj; }
    }
    InterlockedIncrement(&g_relSeq[h]);   // odd: writing
    g_relObj[h] = best;
    g_relDist[h] = best ? bd : -1.0f;
    g_relMs[h] = ms;
    InterlockedIncrement(&g_relSeq[h]);   // even: settled
    if (q == 1 || (nRel > 0 && InterlockedIncrement(&g_nRelictsSeen) == 1)) {
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] RELICT %s (logged unconditionally) - %s hand, squeeze %.2f | the level query returned %d objects "
            "within %.2f m, %d severed limbs; nearest %p at %.3f m (-1 = none) | thread %lu",
            q == 1 ? "first query" : "first severed limb seen", HandName(h), sq, hits, kRelictRadiusM, nRel, best,
            best ? bd : -1.0f, GetCurrentThreadId());
    }
    *distOut = best ? bd : -1.0f;
    return best;
}

// AFW list item 5 (runs BG14, BG15): the NPCs near the player join the rebuild's mask + held-pose map through their own table, so a
// running zombie is moved in the rebuilt eye instead of being carried as static world. This half learns their draw ranges: four new
// AI objects per tick, each read once per 30 s, the table kept until the level changes (a learned range stays: zombies share meshes).
// The AI object is the model (BG15: 353 elements; its BodyVis reads none). Needs body_grab on (Resolve) and a published hand.
// COST: game thread, every 250 ms: one radius query + a pointer walk; <= 4 element walks; RTTI only for a primary vtable not seen.
constexpr int   kNpcMax = 4096;      // = the bootstrap's npcmap::kN
constexpr int   kNpcHash = 16384;    // power of two, > 2 x kNpcMax
constexpr int   kNpcHits = 256;
constexpr int   kNpcPerTick = 4;
constexpr int   kNpcPerModel = 512;
constexpr int   kNpcSeen = 2048;     // power of two
constexpr float kNpcRadiusM = 30.0f;
unsigned  g_npcTab[kNpcMax][3] = {};
int       g_npcN = 0;
long      g_npcGen = 0;
SRWLOCK   g_npcLock = SRWLOCK_INIT;
unsigned short g_npcSet[kNpcHash] = {};   // index + 1 into g_npcTab
uintptr_t g_npcSeenAi[kNpcSeen] = {};
uintptr_t g_npcVt[64] = {};
int       g_npcNVt = 0;
void*     g_npcLevel = nullptr;
ULONGLONG g_npcNext = 0, g_npcForget = 0, g_npcLine = 0;
bool      g_npcFirst = false;
unsigned  g_npcBuf[kNpcPerModel][3];

unsigned NpcKey(unsigned a, unsigned b, unsigned c) { return (a * 2654435761u) ^ (b * 40503u) ^ (c * 2246822519u); }

void NpcLearn(const unsigned (*use)[3], int nu)   // the caller holds g_npcLock
{
    for (int r = 0; r < nu && g_npcN < kNpcMax; ++r) {
        unsigned i = NpcKey(use[r][0], use[r][1], use[r][2]) & (kNpcHash - 1);
        bool have = false;
        for (; g_npcSet[i]; i = (i + 1) & (kNpcHash - 1)) {
            const unsigned* e = g_npcTab[g_npcSet[i] - 1];
            if (e[0] == use[r][0] && e[1] == use[r][1] && e[2] == use[r][2]) { have = true; break; }
        }
        if (have) { continue; }
        g_npcTab[g_npcN][0] = use[r][0]; g_npcTab[g_npcN][1] = use[r][1]; g_npcTab[g_npcN][2] = use[r][2];
        g_npcSet[i] = static_cast<unsigned short>(++g_npcN);
        ++g_npcGen;
    }
}

void NpcTick()
{
    if (!AfwRbFgMask || !g_findRadius || !g_getLevel || !g_player) { return; }
    const ULONGLONG now = GetTickCount64();
    if (now < g_npcNext) { return; }
    g_npcNext = now + 250;
    float at[3] = {};
    double t = 0.0;
    void* level = nullptr;
    const bool okHand = ReadHand(1, at, &t) && t > 0.0;
    const bool okQ = okHand && LevelOf(g_player, &level) && level && QueryRadius(level, at, kNpcRadiusM);
    if (!okQ) {
        // [PROBE: AFWNPC] gate=afw_rb_fg_mask + body_grab  site=HEADSET  log-only=yes
        if (!g_npcFirst) { g_npcFirst = true; LOG("bodygrab: [AFWNPC] FIRST (logged unconditionally): no query - hand %d, level %p. afw_rb_fg_mask=1", okHand ? 1 : 0, level); }
        return;
    }
    if (level != g_npcLevel) {   // a new level: its meshes sit elsewhere in the vertex pool
        g_npcLevel = level;
        AcquireSRWLockExclusive(&g_npcLock);
        g_npcN = 0; ++g_npcGen;
        memset(g_npcSet, 0, sizeof(g_npcSet));
        ReleaseSRWLockExclusive(&g_npcLock);
        memset(g_npcSeenAi, 0, sizeof(g_npcSeenAi));
    }
    if (now >= g_npcForget) { g_npcForget = now + 30000; memset(g_npcSeenAi, 0, sizeof(g_npcSeenAi)); }
    const int hits = static_cast<int>(g_relVec.count);
    const int n = hits < kNpcHits ? hits : kNpcHits;
    int nAi = 0, read = 0, none = 0, most = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t e = 0, vt = 0, s37 = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(g_relVec.data) + 8 * static_cast<uintptr_t>(i), &e) || !e) { continue; }
        const uintptr_t a = e - kCtlObjOff;
        if (!ReadPtr(a, &vt) || !ReadPtr(vt + 8 * kIsDeadSlot, &s37) || s37 != g_gd + g_rIsDead) { continue; }
        ++nAi;
        unsigned s = static_cast<unsigned>((a >> 4) * 2654435761u) & (kNpcSeen - 1);
        bool seen = false;
        for (int k = 0; k < 8 && g_npcSeenAi[s]; ++k, s = (s + 1) & (kNpcSeen - 1)) { if (g_npcSeenAi[s] == a) { seen = true; break; } }
        if (seen || read >= kNpcPerTick) { continue; }
        g_npcSeenAi[s] = a;
        ++read;
        int el = -1;
        const int nu = AfwNpcCollect(reinterpret_cast<void*>(a), g_npcBuf, kNpcPerModel, &el);
        if (nu == 0) { ++none; }
        if (nu > most) { most = nu; }
        bool newVt = true;
        for (int k = 0; k < g_npcNVt; ++k) { if (g_npcVt[k] == vt) { newVt = false; break; } }
        if (newVt && g_npcNVt < 64) {
            g_npcVt[g_npcNVt++] = vt;
#if !defined(DLVR_BETA_LEAN)   // BETA lean: the log line only, compiled out 2026-11-07 (Jay, tester package: "[AFWNPC] 5 s lines")
            char cls[64] = {};
            if (!RttiClassNameOf(a, cls, sizeof(cls))) { strcpy_s(cls, "?"); }
            // [PROBE: AFWNPC] gate=afw_rb_fg_mask + body_grab  site=HEADSET  log-only=yes
            LOG("bodygrab: [AFWNPC] CLASS %s at %p (once per class): %d elements, %d ranges (at most %d). afw_rb_fg_mask=1", cls,
                reinterpret_cast<void*>(a), el, nu, kNpcPerModel);
#endif
        }
        AcquireSRWLockExclusive(&g_npcLock);
        NpcLearn(g_npcBuf, nu);
        ReleaseSRWLockExclusive(&g_npcLock);
    }
    if (!g_npcFirst || now >= g_npcLine) {
        // BETA lean (2026-11-07, Jay, tester package): the FIRST line still prints; the 5 s repeats are compiled out.
#if defined(DLVR_BETA_LEAN)
        if (!g_npcFirst)
#endif
        // [PROBE: AFWNPC] gate=afw_rb_fg_mask + body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [AFWNPC] %s: query %d objects in %.0f m, %d AI | read this tick %d, none %d, most on one %d | ranges learned %d of %d. "
            "afw_rb_fg_mask=1", g_npcFirst ? "5 s" : "FIRST (logged unconditionally)", hits, kNpcRadiusM, nAi, read, none, most, g_npcN, kNpcMax);
        g_npcFirst = true;
        g_npcLine = now + 5000;
    }
}

// [ANIMLOD] (diag_anim_lod, INV-682): the NPC census for boneblend's anim-rate probe - once a second, every HumanAI within
// kAnimLodRadiusM of the right hand, by the same IsDead vtable test NpcTick uses; its model = *(AI + 8) (ElemArray's read). Every
// player update it calls AnimLodTick, which installs the detours once and writes the 5 s report. With body_grab off or the
// exports unresolved the census is empty and says why; the CPU / distance lines still run.
// COST: off = one bool test. On: once a second one radius query + a pointer walk (<= 4096 hits, 3 SEH reads each); per update the
// report's seqlock test.
constexpr float kAnimLodRadiusM = 300.0f;
constexpr int   kAnimLodHits = 4096;
void AnimLodNpcTick()
{
    if (!DiagAnimLod) { return; }
    static ULONGLONG next = 0;
    static uintptr_t buf[512];
    static int n = 0, hits = 0, nAi = 0;
    static uintptr_t cl = 0;
    static const char* why = "not queried yet";
    const ULONGLONG now = GetTickCount64();
    if (now < next) { AnimLodTick(nullptr, n, cl, hits, nAi, why); return; }
    next = now + 1000;
    n = hits = nAi = 0;
    if (!BodyGrab) { why = "body_grab=0 - no radius query"; AnimLodTick(buf, 0, cl, 0, 0, why); return; }
    if (!Resolve() || !g_findRadius || !g_getLevel || !g_player) { why = "bodygrab exports not resolved"; AnimLodTick(buf, 0, cl, 0, 0, why); return; }
    float at[3] = {};
    double t = 0.0;
    void* level = nullptr;
    if (!(ReadHand(1, at, &t) && t > 0.0)) { why = "no published right hand"; AnimLodTick(buf, 0, cl, 0, 0, why); return; }
    if (!(LevelOf(g_player, &level) && level)) { why = "no level"; AnimLodTick(buf, 0, cl, 0, 0, why); return; }
    uintptr_t c = 0;
    if (ReadPtr(reinterpret_cast<uintptr_t>(level) + 8, &c)) { cl = c; }   // ILevel + 8 = CLevel (ILevel::SetLevelUnImportanceFactor's read)
    if (!QueryRadius(level, at, kAnimLodRadiusM)) { why = "radius query failed"; AnimLodTick(buf, 0, cl, 0, 0, why); return; }
    hits = static_cast<int>(g_relVec.count);
    const int lim = hits < kAnimLodHits ? hits : kAnimLodHits;
    for (int i = 0; i < lim; ++i) {
        uintptr_t e = 0, vt = 0, s37 = 0, m = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(g_relVec.data) + 8 * static_cast<uintptr_t>(i), &e) || !e) { continue; }
        const uintptr_t a = e - kCtlObjOff;
        if (!ReadPtr(a, &vt) || !ReadPtr(vt + 8 * kIsDeadSlot, &s37) || s37 != g_gd + g_rIsDead) { continue; }
        ++nAi;
        if (n < 512 && ReadPtr(a + 8, &m) && m) { buf[n++] = m; }
    }
    why = n ? "HumanAI within 300 m of the right hand" : "query ran, 0 HumanAI";
    AnimLodTick(buf, n, cl, hits, nAi, why);
}

void WindowLine(int h, double now)
{
    Grab& g = g_g[h];
    if (now - g.winS < 1.0) { return; }
    // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
    LOG("bodygrab: [BODYGRAB] 1 s %s | updates %ld, forces %ld (stale hand %ld) | error mean %.3f max %.3f m | force mean %.1f max "
        "%.1f N (cap %.1f) | capped %.0f%% | dropped while resting %ld | mass %.2f kg | rotation hold %s: angle error mean %.1f max "
        "%.1f deg, spin mean %.2f rad/s, torque capped %.0f%% | pushes %ld, skipped %ld (no physics step since the last push)",
        HandName(h), g.ticks, g.forces, g.stale, g.forces ? g.errSum / g.forces : 0.0, g.errMax, g.forces ? g.fSum / g.forces : 0.0,
        g.fMax, g.mass * CapAccel(g.relict), g.forces ? 100.0 * g.capped / g.forces : 0.0, g.wakeDropped, g.mass,
        g.hold ? "on" : "off", g.forces ? 57.29578 * g.angSum / g.forces : 0.0, 57.29578 * g.angMax,
        g.forces ? g.wSum / g.forces : 0.0, g.forces ? 100.0 * g.angCapped / g.forces : 0.0, g.pushes, g.skips);
    g.winS = now;
    g.ticks = g.forces = g.capped = g.stale = g.wakeDropped = g.angCapped = g.pushes = g.skips = 0;
    g.errSum = g.fSum = g.angSum = g.wSum = 0.0;
    g.errMax = g.fMax = g.angMax = 0.0f;
}

// HOLD BURST (Jay 10-24 (2): the shaking fix) - log-only: every player update of a held limb for kBurstSamples, starting
// kBurstStartS into the hold, for the first kMaxBursts holds: update interval, angle error, the limb's spin, the torque (and whether
// the spring part was capped), the hand's own rotation speed - 10 updates a line. The shake's period in updates says whether it is
// the one-frame delay (~2 updates a cycle) or the spring (~4 Hz); the hand's speed says whether the hand itself jitters.
void HoldBurst(int h, double now, float err, float spin, float tq, bool capped)
{
    Grab& g = g_g[h];
    if (kMaxBursts <= 0 || g.burst < 0) { return; }   // OFF (see kMaxBursts)
    if (g.burst == 0) {
        if (now - g.startS < kBurstStartS) { return; }
        const LONG set = InterlockedIncrement(&g_nBursts);
        if (set > kMaxBursts) { g.burst = -1; return; }
        g.burst = 1;
        g.bSet = set;
        g.bCount = 0;
        g.bFill = 0;
        g.bPrevS = now;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HOLD BURST %ld/%ld BEGIN (logged unconditionally) - %s hand, severed limb %p, %.2f s into the hold | "
            "turn %.2f Hz, damping %.2f, spring cap %.0f rad/s^2 (damping uncapped) | mass %.2f kg, inertia diag (%.4f %.4f %.4f) | "
            "%ld pushes (one per physics step since 10-24 (2)), 10 a line: dt ms / angle error deg / limb spin rad/s / torque N m "
            "(C = spring capped) / hand turn rad/s",
            set, kMaxBursts, HandName(h), g.ai, now - g.startS, BodyGrabLimbTurnHz, BodyGrabLimbTurnDamping, kHoldMaxAngAccel,
            g.mass, g.inertia[0], g.inertia[4], g.inertia[8], kBurstSamples);
    }
    const int i = g.bFill++;
    g.bDt[i] = static_cast<float>((now - g.bPrevS) * 1000.0);
    g.bPrevS = now;
    g.bErr[i] = 57.29578f * err;
    g.bSpin[i] = spin;
    g.bTq[i] = tq;
    g.bCap[i] = capped;
    g.bHandW[i] = g.handW;
    ++g.bCount;
    const bool last = g.bCount >= kBurstSamples;
    if (g.bFill == 10 || last) {
        char line[768];
        int len = 0;
        for (int k = 0; k < g.bFill && len < 700; ++k) {
            const int n = std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " %.1f/%.1f/%.2f/%.2f%s/%.2f;", g.bDt[k],
                                        g.bErr[k], g.bSpin[k], g.bTq[k], g.bCap[k] ? "C" : "", g.bHandW[k]);
            if (n > 0) { len += n; }
        }
        line[len < 767 ? len : 767] = 0;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HOLD BURST %ld updates %ld-%ld:%s", g.bSet, g.bCount - g.bFill + 1, g.bCount, line);
        g.bFill = 0;
    }
    if (last) {
        g.burst = -1;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] HOLD BURST %ld END", g.bSet);
    }
}

// One player update for one held body. COST: 1 FindObjectByID (the engine's level-tree walk, O(log n), no allocation - review F3)
// + 3 guarded reads + 1 IsDead call + PhysicsBind + 1 vcall (ragdoll) + the body count +
// up to ~15 body-at-index vcalls (the same-body check) + PointLocalToWorld + GetPointVel + 1 byte / 2 pointer reads (rest state) +
// AddForceAtPos - all direct calls, no RTTI walk, no lookup by name.
void Drive(int h, double now)
{
    Grab& g = g_g[h];
    ++g.ticks;
    // Review F3 (INV-663): the engine's id lookup first - a destroyed body is dropped without one read of its memory.
    if (!StillInEngine(g.gso, g.id)) {
        Finish(h, g_player ? "body gone (no longer returned by the engine's id lookup - despawned, not read)"
                           : "the player is not published - cannot re-validate", true);
        return;
    }
    // Review Focus 1: re-validate before touching the body - same vftable, still dead, the same ragdoll, the body still in it.
    uintptr_t vt = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(g.ai), &vt) || vt != g.aiVt) { Finish(h, "body gone (the AI's vftable changed)"); return; }
    if (!g.relict && !g.live) {   // a severed limb has no IsDead (its vftable is 28 slots) - the vftable + ragdoll checks cover it;
        bool dead = false;        // a live grab's enemy stays alive - its ragdoll ending (it gets up) ends the grab below
        if (!CallIsDead(g.ai, &dead)) { Finish(h, "body gone (IsDead faulted)"); return; }
        if (!dead) { Finish(h, "body gone (IsDead reads false)"); return; }
    }
    void* p = nullptr;
    if (!RagdollOf(g.ai, &p, g.relict) || p != g.p) { Finish(h, "body gone (the ragdoll changed or is gone)"); return; }
    if (g.live && g_st[h].on && g_st[h].p == p) { StiffWeight(h); }   // the held live enemy keeps enemy_grab_weight of its weight
    int idx = -1, count = -1;
    if (!BodyIndex(p, g.body, &idx, &count) || idx < 0) { Finish(h, "body gone (no longer one of the ragdoll's bodies)"); return; }
    // Review Focus 2: the hand. No force from a stale sample; END after 0.5 s without one.
    float hand[3] = {}, handRot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    double t = 0.0;
    if (!ReadHand(h, hand, &t, handRot) || t <= 0.0) { ++g.stale; return; }
    if (now - t > kNoPoseS) { Finish(h, "no pose for 0.5 s"); return; }
    if (now - t > kStaleS) { ++g.stale; g.havePrev = false; return; }
    if (g.havePrev && t > g.prevT + 0.001) {
        const float inv = static_cast<float>(1.0 / (t - g.prevT));
        for (int k = 0; k < 3; ++k) {
            const float v = (hand[k] - g.prevPos[k]) * inv;
            g.handVel[k] = 0.5f * g.handVel[k] + 0.5f * v;   // one-pole smoothing: the pose steps once per display frame (D-269)
        }
    }
    if (!g.havePrev || t > g.prevT + 0.001) {
        g.prevT = t;
        for (int k = 0; k < 3; ++k) { g.prevPos[k] = hand[k]; }
        g.havePrev = true;
    }
    // wp = where the force goes and what is pulled; goal = where it is pulled to. The point pull: the grab point to the hand. The
    // rotation hold (a severed limb or a corpse part): the CENTRE to the hand's pose times the START offset, plus a torque to the hand's rotation.
    float wp[3] = {}, pv[3] = {}, goal[3] = { hand[0], hand[1], hand[2] };
    float bq[4] = {}, bw[3] = {}, qT[4] = {};
    if (g.hold) {
        if (!BodyPos(g.body, wp) || !Finite3(wp) || !BodyVec(g_getPointVel, g.body, wp, pv) || !Finite3(pv) ||
            !ReadBodyRot(g.body, bq, bw)) {
            Finish(h, "body gone (centre / rotation reads faulted)");
            return;
        }
        float off[3] = {};
        bodygrab::QuatRotate(handRot, g.comLocal, false, off);
        for (int k = 0; k < 3; ++k) { goal[k] = hand[k] + off[k]; }
        bodygrab::QuatMul(handRot, g.qRel, qT);
        // once per physics step: no step since the last push (centre and rotation exactly as then) = no new push, else the step
        // receives it 2-5 times from the same stale spin (limb_shake_run1). A body at rest never changes: after kPushFallbackS it is
        // pushed anyway so it still wakes.
        bool same = g.lastPushS > 0.0 && now - g.lastPushS < kPushFallbackS;
        for (int k = 0; k < 3 && same; ++k) { same = wp[k] == g.lastP[k]; }
        for (int k = 0; k < 4 && same; ++k) { same = bq[k] == g.lastQ[k]; }
        if (same) {
            ++g.skips;
            WindowLine(h, now);
            return;
        }
        for (int k = 0; k < 3; ++k) { g.lastP[k] = wp[k]; }
        for (int k = 0; k < 4; ++k) { g.lastQ[k] = bq[k]; }
        g.lastPushS = now;
        ++g.pushes;
    } else if (!BodyVec(g_localToWorld, g.body, g.local, wp) || !BodyVec(g_getPointVel, g.body, wp, pv) || !Finite3(wp) ||
               !Finite3(pv)) {
        Finish(h, "body gone (point reads faulted)");
        return;
    }
    bodygrab::SpringParams sp;
    sp.hz = BodyGrabHz;
    sp.damping = BodyGrabDamping;
    sp.maxAccel = CapAccel(g.relict);
    const bodygrab::SpringOut o = bodygrab::SpringForce(g.mass, sp, goal, g.handVel, wp, pv);
    if (!Finite3(o.force)) { ++g.stale; return; }
    bool resting = false;
    float thr = -1.0f;
    WakeState(g.body, g.mass, &resting, &thr);
    const float fl = Len3(o.force);
    if (!AddForce(g.body, o.force, wp)) { Finish(h, "body gone (AddForceAtPos faulted)"); return; }
    if (g.hold) {
        const bodygrab::AngularOut ao = bodygrab::AngularSpring(g.inertia, bq, bw, qT, BodyGrabLimbTurnHz, BodyGrabLimbTurnDamping,
                                                                kHoldMaxAngAccel);
        if (Finite3(ao.torque) && !AddTorqueTo(g.body, ao.torque)) { Finish(h, "body gone (AddTorque faulted)"); return; }
        g.angSum += ao.errAngle;
        if (ao.errAngle > g.angMax) { g.angMax = ao.errAngle; }
        if (ao.capped) { ++g.angCapped; }
        g.wSum += Len3(bw);
        // the hand's own rotation speed: is the shake the hand's (tracking jitter) or the limb's?
        if (g.rotPrevT > 0.0 && t > g.rotPrevT + 0.001) {
            float pc[4] = {}, d[4] = {}, rv[3] = {};
            bodygrab::QuatConj(g.prevHandRot, pc);
            bodygrab::QuatMul(handRot, pc, d);
            bodygrab::QuatToRotVec(d, rv);
            g.handW = Len3(rv) / static_cast<float>(t - g.rotPrevT);
        }
        if (g.rotPrevT <= 0.0 || t > g.rotPrevT + 0.001) {
            for (int k = 0; k < 4; ++k) { g.prevHandRot[k] = handRot[k]; }
            g.rotPrevT = t;
        }
        HoldBurst(h, now, ao.errAngle, Len3(bw), Len3(ao.torque), ao.capped);
    }
    ++g.forces;
    if (o.capped) { ++g.capped; }
    if (resting && thr >= 0.0f && fl <= thr) { ++g.wakeDropped; }
    g.errSum += o.errLen;
    g.fSum += fl;
    if (o.errLen > g.errMax) { g.errMax = o.errLen; }
    if (fl > g.fMax) { g.fMax = fl; }
    if (!g_firstDrive) {
        g_firstDrive = true;
        // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=yes
        LOG("bodygrab: [BODYGRAB] first drive sample (logged unconditionally) - %s hand | hand (%.3f, %.3f, %.3f) | point (%.3f, %.3f, "
            "%.3f) | error %.3f m | force (%.1f, %.1f, %.1f) N, %s | point velocity %.2f m/s | resting %d (wake force %.1f N) | hand "
            "sample %.1f ms old | thread %lu",
            HandName(h), hand[0], hand[1], hand[2], wp[0], wp[1], wp[2], o.errLen, o.force[0], o.force[1], o.force[2],
            o.capped ? "CAPPED" : "under the cap", Len3(pv), resting ? 1 : 0, thr, (now - t) * 1000.0, GetCurrentThreadId());
    }
    WindowLine(h, now);
}

}  // namespace

bool BodyGrabBegin(int hand, void* ai, const float handWorld[3])
{
    if (!BodyGrab || hand < 0 || hand > 1 || !ai || !handWorld || !Finite3(handWorld)) {
        if (EventLine()) {
            // [PROBE: BODYGRAB] gate=body_grab  site=HEADSET  log-only=no
            LOG("bodygrab: [BODYGRAB] REFUSED at the press - %s (hand %d, body %p)",
                !BodyGrab ? "body_grab=0" : (hand < 0 || hand > 1) ? "bad hand index" : !ai ? "no body" : "the hand point is not finite",
                hand, ai);
        }
        return false;
    }
    g_cmdAi[hand] = ai;
    for (int k = 0; k < 3; ++k) { g_cmdPt[hand][k] = handWorld[k]; }
    InterlockedExchange(&g_cmd[hand], 1);
    return true;
}

bool BodyGrabBeginLive(int hand, void* ai, const float handWorld[3])
{
    if (!BodyGrab || hand < 0 || hand > 1 || !ai || !handWorld || !Finite3(handWorld)) { return false; }
    g_cmdAi[hand] = ai;
    for (int k = 0; k < 3; ++k) { g_cmdPt[hand][k] = handWorld[k]; }
    InterlockedExchange(&g_cmd[hand], 3);
    return true;
}

void BodyGrabEnd(int hand, const char* why)
{
    if (hand < 0 || hand > 1) { return; }
    g_cmdWhy[hand] = why;
    InterlockedExchange(&g_cmd[hand], 2);
    InterlockedExchange(&g_active[hand], 0);   // the weapon and the attack blocks let go now, not on the next update
}

int NpcRanges(unsigned* out, int maxTriples, long* gen)
{
    AcquireSRWLockShared(&g_npcLock);
    int n = g_npcN;
    if (n > maxTriples) { n = maxTriples; }
    if (out && n > 0) { memcpy(out, g_npcTab, sizeof(unsigned) * 3 * static_cast<size_t>(n)); }
    if (gen) { *gen = g_npcGen; }
    ReleaseSRWLockShared(&g_npcLock);
    return n;
}

bool BodyGrabHandActive(int hand)
{
    return hand >= 0 && hand <= 1 && g_active[hand] != 0;
}

// handcoll.cpp SHAPECHECK (plan 2026-10-30-live-enemy-hand-collision Task 3): the AI's ragdoll hitboxes exactly as body grab measures
// them (NearestHitbox's walk, every geom it could measure). Read-only. Game thread. COST: one ragdoll body list + ~12 reads per geom.
int BodyGrabRagdollHitboxes(void* ai, BodyGrabHitbox* out, int cap)
{
    if (!ai || !out || cap <= 0) { return -4; }
    if (!BodyGrab) { return -1; }
    if (!Resolve()) { return -2; }
    void* p = nullptr;
    if (!RagdollOf(ai, &p, false) || !p) { return -3; }
    static HitPick s_hp;   // game thread only (its recs are ~2.5 KB)
    const float farPt[3] = { 1.0e6f, 1.0e6f, 1.0e6f };
    NearestHitbox(p, farPt, &s_hp);
    int n = 0;
    for (int i = 0; i < s_hp.nrec && n < cap; ++i) {
        const HitPick::Rec& r = s_hp.recs[i];
        if (!r.ok) { continue; }
        BodyGrabHitbox& o = out[n++];
        o.body = r.bi;
        o.shape = r.shape;
        for (int k = 0; k < 3; ++k) { o.c[k] = r.c[k]; o.prm[k] = r.prm[k]; }
        for (int k = 0; k < 4; ++k) { o.q[k] = r.q[k]; }
    }
    return n;
}

void BodyGrabPublishSqueeze(int hand, float squeeze)
{
    if (hand < 0 || hand > 1) { return; }
    LONG bits = 0;
    std::memcpy(&bits, &squeeze, sizeof(bits));
    InterlockedExchange(&g_squeezeBits[hand], BodyGrab ? bits : 0);
    // when this squeeze reached the search's threshold (pad thread only writes it) - a reading older than this is not this press's
    const bool over = BodyGrab && squeeze >= kRelictSqueeze;
    if (!over) { g_sqStartMs[hand] = 0; } else if (g_sqStartMs[hand] == 0) { g_sqStartMs[hand] = GetTickCount64(); }
}

void BodyGrabPublishPlayer(void* player)
{
    g_player = player;
}

void* BodyGrabRelictOf(int hand, float* distM)
{
    if (distM) { *distM = -1.0f; }
    if (!BodyGrab || hand < 0 || hand > 1) { return nullptr; }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_relSeq[hand];
        if (s0 & 1) { continue; }
        void* o = g_relObj[hand];
        const float d = g_relDist[hand];
        const ULONGLONG ms = g_relMs[hand];
        if (g_relSeq[hand] != s0) { continue; }
        if (!o || GetTickCount64() - ms > kRelictFreshMs) { return nullptr; }
        if (distM) { *distM = d; }
        return o;
    }
    return nullptr;
}

bool BodyGrabIsRelict(void* obj)
{
    return IsRelictObj(obj);
}

// The camouflage gesture (camogesture.cpp). Game thread: what hand `hand` holds now - the corpse AI or the severed limb (relict) the
// held grab started on (re-validated by Drive every update, before CamoGestureTick runs in the same tail). A LIVE knocked-down enemy
// (BodyGrabBeginLive) is not a corpse: nullptr. COST: three reads.
void* BodyGrabHeldObject(int hand, bool* relict)
{
    if (relict) { *relict = false; }
    if (hand < 0 || hand > 1 || !g_g[hand].active || g_g[hand].live) { return nullptr; }
    if (relict) { *relict = g_g[hand].relict; }
    return g_g[hand].ai;
}

// Any thread: `obj`'s engine object id (its IGSObject at +40, as a corpse and a relict both carry it). False = body grab's engine
// exports not resolved yet, or a fault. COST: one guarded engine call.
bool BodyGrabObjectId(void* obj, int* id)
{
    if (!obj || !id || !g_getId) { return false; }
    return CallGetId(static_cast<char*>(obj) + kGsoOff, id);
}

// Game thread: is `obj` still the live object the engine knows by `id`? The engine's own id lookup through the player (review F3,
// StillInEngine) - `obj`'s memory is never read. False = gone, never published player, or a fault.
bool BodyGrabObjectAlive(void* obj, int id)
{
    if (!obj || !g_findById) { return false; }
    return StillInEngine(static_cast<char*>(obj) + kGsoOff, id);
}

// Any thread: the PlayerDI the HUD hook last published (NearbyHook), nullptr before the first.
void* BodyGrabPlayer()
{
    return g_player;
}

void BodyGrabPublishCandidate(int hand, void* body)
{
    if (hand < 0 || hand > 1) { return; }
    g_cand[hand] = BodyGrab ? body : nullptr;
}

int BodyGrabTorsoOf(int hand, void* body)
{
    if (!BodyGrab || hand < 0 || hand > 1 || !body) { return -1; }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_torSeq[hand];
        if (s0 & 1) { continue; }
        void* b = g_torBody[hand];
        const int is = g_torIs[hand];
        const ULONGLONG ms = g_torMs[hand];
        if (g_torSeq[hand] != s0) { continue; }
        if (b != body || GetTickCount64() - ms > kTorsoFreshMs) { return -1; }
        return is;
    }
    return -1;
}

float BodyGrabHitOf(int hand, void* body)
{
    if (!BodyGrab || hand < 0 || hand > 1 || !body) { return -1.0f; }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_torSeq[hand];
        if (s0 & 1) { continue; }
        void* b = g_torBody[hand];
        const float d = g_torHit[hand];
        const ULONGLONG ms = g_torMs[hand];
        if (g_torSeq[hand] != s0) { continue; }
        if (b != body || GetTickCount64() - ms > kTorsoFreshMs) { return -1.0f; }
        return d;
    }
    return -1.0f;
}

void BodyGrabRequestRelict(int hand)
{
    if (!BodyGrab || hand < 0 || hand > 1) { return; }
    g_relReqMs[hand] = GetTickCount64();
    InterlockedExchange(&g_relReq[hand], 1);
}

bool BodyGrabRelictPending(int hand)
{
    if (!BodyGrab || hand < 0 || hand > 1 || !g_findRadius || !g_getLevel || !g_player) { return false; }
    const ULONGLONG sq = g_sqStartMs[hand], rq = g_relReqMs[hand];
    const ULONGLONG since = sq > rq ? sq : rq;   // the newer of: the squeeze reached the search, a press asked for a fresh query
    if (since == 0) { return false; }   // neither: the search is not due
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG s0 = g_relSeq[hand];
        if (s0 & 1) { continue; }
        const ULONGLONG ms = g_relMs[hand];
        if (g_relSeq[hand] != s0) { continue; }
        return ms < since;
    }
    return true;   // mid-write three times: treat the reading as not yet in
}

void BodyGrabPublishHand(int hand, const float world[3], const float rot[4], double tS)
{
    if (hand < 0 || hand > 1) { return; }
    InterlockedIncrement(&g_pubSeq[hand]);   // odd: writing
    g_pubPos[hand][0] = world[0];
    g_pubPos[hand][1] = world[1];
    g_pubPos[hand][2] = world[2];
    for (int k = 0; k < 4; ++k) { g_pubRot[hand][k] = rot[k]; }
    g_pubT[hand] = tS;
    InterlockedIncrement(&g_pubSeq[hand]);   // even: settled
}

// COST per player update: off = one bool test. On, nothing held = two interlocked exchanges; each held body = Drive's cost above;
// once per launch the ID CHECK self-test (one GetID + one FindObjectByID).
void BodyGrabTick()
{
    AnimLodNpcTick();   // [ANIMLOD]: diag_anim_lod off -> one bool test
    if (!BodyGrab) {
        for (int h = 0; h < 2; ++h) {
            g_rr[h].pending = false;
            if (g_g[h].active) { Finish(h, "body_grab switched off"); }
        }
        return;
    }
    if (!Resolve()) { return; }   // READY / REFUSED once - READY names loot_by_hand=0 if it is off
    NpcTick();                    // AFW item 5: afw_rb_fg_mask off -> one bool test
    for (int h = 0; h < 2; ++h) {   // final review I4: a stiff-hold release that could not validate is retried here
        if (g_st[h].on && g_st[h].pending && !g_g[h].active) { StiffRelease(h, "retried"); }
    }
    if (!LootByHand) {
        for (int h = 0; h < 2; ++h) {
            g_rr[h].pending = false;
            if (g_g[h].active) { Finish(h, "loot_by_hand switched off"); }
        }
        return;
    }
    void* pl = g_player;
    if (pl && !g_idSelfTestSaid) {
        g_idSelfTestSaid = true;
        void* pgso = static_cast<char*>(pl) + kGsoOff;
        int pid = 0;
        void* found = nullptr;
        const bool okId = CallGetId(pgso, &pid), okFind = okId && CallFindById(pgso, pid, &found);
        // [PROBE: BODYGRAB] gate=body_grab  site=FLAT  log-only=yes
        LOG("bodygrab: [BODYGRAB] ID CHECK self-test (logged unconditionally once the player is published): player id %d -> the "
            "lookup returns %s (review F3: every press and every held update is re-validated this way before the body is read)",
            pid, !okId ? "NOTHING - GetID faulted" : !okFind ? "NOTHING - FindObjectByID faulted" : found == pgso ? "the player (OK)"
            : found ? "ANOTHER object - bodies whose id another object shares will be refused" : "null");
    }
    const double now = NowS();
    for (int h = 0; h < 2; ++h) {
        TorsoTrack(h);
        RelictTrack(h);
        const LONG c = InterlockedExchange(&g_cmd[h], 0);
        if (c == 2) {
            g_rr[h].pending = false;
            Finish(h, g_cmdWhy[h]);
        } else if (c == 1 || c == 3) {
            if (g_g[h].active) { Finish(h, "replaced by a new grab"); }
            g_rr[h].pending = false;   // a new press: its own single restart
            float pt[3] = { g_cmdPt[h][0], g_cmdPt[h][1], g_cmdPt[h][2] };
            Start(h, g_cmdAi[h], pt, now, c == 3);   // 3 = BodyGrabBeginLive (a knocked-down live enemy)
        } else if (g_rr[h].pending && !g_g[h].active) {
            // a queued restart: try the grab again at the hand's current point (the press point if no fresh sample)
            float pt[3] = { g_rr[h].pt[0], g_rr[h].pt[1], g_rr[h].pt[2] };
            float cur[3] = {};
            double t = 0.0;
            if (ReadHand(h, cur, &t) && t > 0.0 && now - t <= kStaleS && Finite3(cur)) {
                pt[0] = cur[0]; pt[1] = cur[1]; pt[2] = cur[2];
            }
            Start(h, g_rr[h].ai, pt, now);
        }
        if (g_g[h].active) { Drive(h, now); }
    }
}

}  // namespace dlvr
