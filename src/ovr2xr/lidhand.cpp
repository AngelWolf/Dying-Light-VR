// lidhand.cpp - see lidhand.h. Spec docs/superpowers/specs/2026-11-18-hand-open-lids-design.md (Jay 2026-11-18: "not affect the
// players view or positioning at all", "physically grab the top of the container and open it/close it", "the item to physically
// appear in the hand until the player stows it", close "Visual only", "Top lids first", option "A"). Trace
// docs/traces/20261118_1711_hand_open_lids.md. Facts (OUR gamedll 97046bba / engine e9fbfc2c): RESEARCH-NATIVE-LOOTING §12-§13.
//
// WHAT IT DOES:
//   - NearbyHook (looting.cpp, HUD trigger update) hands us the game's gathered triggers: ComplexContainers whose mesh + lid bone
//     match lid_hinge_table.inc, and ContainerItems, are published (seqlock); a game pick on one of them is replaced by the game's
//     own NULL pick (the caller's CallSetter) - no prompt, X has no target, so the game's interaction (walk-to, look turn, the
//     player clip - §12) never starts for these containers.
//   - LidHandTick (game thread): a grip edge with the hand in reach of a published lid's box (top half; state 2 or 1 - never 10,
//     locked) owns that lid; its angle follows the hand about the hinge (relative to the grab); SetElementLocalMatrix(lid bone,
//     R(axis, deg) . rest) each tick while it is off rest, the local read back first (an overwrite is counted, logged once).
//     First crossing of lid_open_frac x open in state 2 -> one +B9B930(player, container, info): the game's own open (points,
//     state 1, SpawnItems, its own lid clip). Option A: the hand lets go there and our write for that lid stops until the next grab.
//   - Items: a grip edge in reach of a ContainerItem whose container is open past the open point -> HandCarryTakeAs onto R_HandHolder /
//     L_HandHolder, the grip socket in the palm (trace 20261120_1900) (home = its current parent element = the hook the game attached
//     it to). Grip open in the shoulder zone with the item's
//     action == 1 -> give back, then +B9B930(player, item, info) = the game's own take; anywhere else -> give back. The container
//     leaving state 1 while carried -> forget (the game destroys its placed items, §13.5).
//   - LidHandPoll (scePad poll) withholds L1 / R1 while a hand owns a lid / item or is next to one with the grip squeezed.
// THREADS: LidHandOnNearby on the HUD update (presumed the game thread - looting.cpp's note); LidHandTick on the game thread;
// LidHandPoll on the pad thread. They share a seqlocked list and a few interlocked longs only.
#include "lidhand.h"

#include "boneblend.h"      // RigGripWorldRead (game world grip pose)
#include "camhook.h"        // BoneProbeCapturedModel (the player's FPP body model)
#include "controllers.h"    // GetControllerSqueeze, GetControllerGripPose
#include "handcarry.h"      // the item on the hand (INV-613 engine attach)
#include "headpose.h"       // GetHeadPose (the shoulder zone)
#include "holster.h"        // HolsterZoneLocal, HolsterCenterX / Y / Z, HolsterRadius - the right-shoulder zone
#include "holster_logic.h"  // holster::kExitScale
#include "lidhand_logic.h"
#include "looting.h"        // LootReachM (the same reach as loot by hand)
#include "looting_logic.h"  // looting::BoxDist
#include "promptquad.h"     // PromptQuadTick - the in-world prompt, ticked first in LidHandTick (trace 20261123_1356)
#include "rtti_vtable.h"    // RttiFindVtable
#include "throwable_logic.h"  // throwable::HandHistory - a carriable's let-go velocity (trace 20261126_1500)
#include "twohand.h"        // RigTwoHandHaptic - the buzz strength (trace 20261118_1905: reuse, no new key)

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>   // free - the inventory enumeration's buffer (trace 20261120_2210)
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_LID_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#else
#define DLVR_LID_GD_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: lid_by_hand] required-for=opening top-lid containers by hand + items held until stowed (LidByHand) - spec
//      2026-11-18-hand-open-lids-design. tree=INERT at 0: LidHandOnNearby / LidHandTick / LidHandPoll return at their first test,
//      nothing is read or written. Needs vr_grip=1, NOT loot_by_hand (trace 20261119_1240, by code: the shared GetNearbyTriggers
//      detour is installed by the always-on camouflage poll GripCamoPoll -> TryInstall, which has no loot gate, and NearbyHook calls
//      LidHandOnNearby before its loot_by_hand return; LidHandPoll owns its press BEFORE LootPoll). HEADSET: the grip
//      poses + squeeze, the rig's game-world grip (RigGripWorldRead), the player body model (BoneProbeCapturedModel). SHIPS ON
//      from 1.06 (Jay 2026-11-21: "Ship on."; trace 20261121_2237). Shipped OFF in 1.04 / 1.05 (Jay 2026-11-18: "Ship it OFF";
//      trace 20261118_2045): it withheld the game's prompt on every lid
//      container incl. a locked one's lockpick (fixed 2026-11-19, trace 20261119_1135: a locked one keeps the game's prompt), and
//      roomscale pushes the player off the chest. Jay tests it ON on his disk. A carried item has the game's loose-item collision
//      category 2048 from the take to the give-back (trace 20261120_1634: the player's headroom check saw it at the shoulder and the
//      game crouched the player); needs the engine export IControlObject::PhysicsBind, named on the READY line when missing.
bool  LidByHand = true;
// [DEP: lid_open_frac] required-for=where the game's own open fires, as a fraction of the lid's open angle (LidOpenFrac). Tier D
//      (my call 0.5, spec §4.4). tree=INERT at lid_by_hand=0.
float LidOpenFrac = 0.5f;

namespace {

namespace lh = lidhand;
#include "lid_hinge_table.inc"   // kLidHinges - tools/lid_hinge_table.py (OUR data, controls in the tool)
constexpr int kRows = static_cast<int>(sizeof(kLidHinges) / sizeof(kLidHinges[0]));
// CLIP-FOLLOW rows (trace 20261121_2145; plan 2026-11-21-clip-follow-containers): the van's rear doors and basket B's lift-off cover -
// not one fixed hinge, so the bone is written at the game's OWN clip frame nearest the hand. Row ids kRows .. kRows + kClipRows - 1; for
// such a row LidState::deg holds the FRAME POSITION t (frames), not degrees. Every shared path reads a row through the accessors below.
#include "lid_clip_table.inc"    // kLidClips / kLidClipFrames - tools/lid_clip_table.py (OUR data, controls in the tool)
bool IsClipRow(int row) { return row >= kRows && row < kRows + kClipRows; }
const LidClipRow& ClipRow(int row) { return kLidClips[row - kRows]; }
const char* RowObject(int row) { return IsClipRow(row) ? ClipRow(row).object : kLidHinges[row].object; }
const float* RowEdge(int row) { return IsClipRow(row) ? ClipRow(row).edge : kLidHinges[row].edge; }
const float* RowPivot(int row) { return IsClipRow(row) ? ClipRow(row).pivot : kLidHinges[row].pivot; }
const float* RowAxis(int row) { return IsClipRow(row) ? ClipRow(row).axis : kLidHinges[row].axis; }
// the open point's base (LidOpenFrac of it fires the game's open): hinge = its open angle; clip = the table frame of the open pose
float RowOpenBase(int row) { return IsClipRow(row) ? static_cast<float>(ClipRow(row).openFrame) : kLidHinges[row].openDeg; }
// the far end of the row's range (the clamp, the ease target): hinge = its open angle; clip = the LAST frame (the game's open jump's pose)
float RowEnd(int row) { return IsClipRow(row) ? static_cast<float>(ClipRow(row).nFrames - 1) : kLidHinges[row].openDeg; }
const float (*ClipFramesOf(int row))[12] { return &kLidClipFrames[ClipRow(row).first]; }
const char* RowUnit(int row) { return IsClipRow(row) ? "frame" : "deg"; }
// Clip rows switched OFF (trace 20261121_2235, Jay RUN CF3: "still a little rough ... disable it just for the basket for now"): skipped
// by LidIdentity, so the object is not a lid to lidhand and the game opens it its own way (prompt + X). The row and its code stay.
// 2026-11-22 (78): the basket is back ON as a FREE LIFT (trace 20261122_1830) - the list is empty (nullptr = no entry).
constexpr const char* kClipRowsOff[] = { nullptr };
bool ClipRowOff(int row)
{
    if (!IsClipRow(row)) { return false; }
    for (const char* o : kClipRowsOff) { if (o && strcmp(ClipRow(row).object, o) == 0) { return true; } }
    return false;
}
// THE FREE LIFT rows (trace 20261122_1830, Jay: basket B "1" -> "Yes"): a clip row with a rim circle (tools/lid_rim_fit.py) is NOT
// clip-followed - the cover rides the hand's drawn holder freely; let go lifted > lh::kFreeLiftOpenM = the game's own open (its leaning
// end pose), else back on closed. Grab zone = the ring. -1 = not a free row.
#include "lid_rim_table.inc"     // kLidRims - tools/lid_rim_fit.py (OUR data, controls in the tool)
int FreeRim(int row)
{
    if (!IsClipRow(row)) { return -1; }
    for (int i = 0; i < kLidRimRows; ++i) { if (strcmp(kLidRims[i].object, ClipRow(row).object) == 0) { return i; } }
    return -1;
}
// The SMOOTHED clip rows (trace 20261121_2225): the basket cover only - rim grab point, stickiness, eased frame. Resolved once by name.
bool ClipSmoothed(int row)
{
    static int s_row = -2;
    if (s_row == -2) {
        s_row = -1;
        for (int i = 0; i < kClipRows; ++i) {
            if (strcmp(kLidClips[i].object, "basket_cover_removing") == 0) { s_row = kRows + i; }
        }
    }
    return row == s_row;
}
// grip-loot containers (trace 20261121_2145, Jay: "just holding grip on the item to loot it should be enough"): the game's open IS the
// loot (containers.loot: "pick_up_money", Trigger_Open) - a grip press with the hand on it calls the game's own execute; no bone moves.
constexpr const char* kGripLootMeshes[] = { "purse_medi_a", "purse_medi_b" };
// Jay's approved per-item grips (trace 20261120_2057): included in THIS TU only - boneblend's finger writer reads rows through
// LidHandPoseData, never a pointer compare (memory const-table-pointers-across-tus).
#include "lid_item_poses.inc"   // kLidPoses - tools/lid_item_poses.py (controls C1-C4 in the tool)
struct LidPoseNames {
    const char* n[kLidPoseCount];
    LidPoseNames() { for (int i = 0; i < kLidPoseCount; ++i) { n[i] = kLidPoses[i].mesh; } }
};
const LidPoseNames kLidPoseNames;
volatile LONG g_poseRow[2] = { -1, -1 };   // tick -> finger pass: the carried item's pose row per side, -1 = none (trace 20261120_2057)

// ---- OUR gamedll 97046bba (RESEARCH §12d / §12e / §13) ----
constexpr uintptr_t kExecuteRva = 0xB9B930;   // sub_180B9B930(player, trigger, info) - the game's execute (§12d)
// sub_180EF4340(obj, key, 0, jump, force) - play the object's interaction-data stage for a key; jump = straight to its END pose
// (ida/lk5_out.txt, lk6_out.txt; trace 20261119_1310). No EGL buildrva row: on EGL the head check below refuses the jump (named).
constexpr uintptr_t kPlayStageRva = 0xEF4340;
constexpr size_t    kStageKeySlotOff = 1416;   // vfunc slot 177 (+EF7220): the stage key for the CURRENT state (6 = Open in state 1)
constexpr float     kOpenEaseS = 0.30f;        // our write carries the lid from the hand to the open angle (my call, a constant)
constexpr size_t    kStateOff = 824;           // PersistentTriggerObjectDI state (ComplexContainer slot 195 +A57BF0 returns it)
constexpr size_t    kItemOwnerOff = 1016;      // ContainerItem -> its ComplexContainer (CreateItem +A6E720 writes it, §12e)
constexpr size_t    kActionSlotOff = 1432;     // vfunc: the trigger's action for a use info (ContainerItem +A69FD0: 1 or 23)
constexpr size_t    kCtlObjOff = 24;           // IControlObject (INV-402)
// IGSObject: ComplexContainer +0x28 (OUR gamedll 97046bba RTTI hierarchy, ida/ccr11_out.txt: GameObject / IGSObject at mdisp 40) - the
// same +40 as PlayerDI (handcarry.cpp kGsoOff). Its record = *(IGSObject+32): record+72 = the level, record+80 = the id (INV-663);
// level+3452 = the level's next id (engine e9fbfc2c AssignNewID +252090 / SetLevel +1347D0). Trace 20261125_1610 v2.
constexpr size_t    kGsoOff = 40;
constexpr size_t    kRecLevelOff = 72, kLevelNextIdOff = 3452;
constexpr int       kStateOpened = 1, kStateClosed = 2, kStateLocked = 10;
constexpr float     kGrip = 0.5f;              // ovr2xr.cpp's vr_grip threshold
constexpr int       kMaxPub = 16;
constexpr int       kMaxLids = 8;
constexpr float     kDropDistM = 6.0f;         // a lid this far from the hand stops being written (my call)
// The lid grab zone (trace 20261118_2010, Jay "Yes"): the hand within this of the lid's FAR EDGE (kLidHinges[].edge, a segment that
// rides the lid). Was 0.35 m round the whole box + a top band (trace 20261118_1905) - RUN LH3 grabbed the basket 0.267 m outside its
// box, ~1.0 m from the hinge. My call, a constant (no new key).
constexpr float     kLidEdgeReachM = 0.20f;
// A clip row's frame may move at most this many frames per tick toward the hand (trace 20261121_2145; my call, a constant): keeps the
// basket cover on its own stretch of a turning path and stops a hand that tears away from snapping the door to the far end.
constexpr float     kClipWindowFrames = 6.0f;
// The basket cover's follow (trace 20261121_2225, Jay CF2: "extremely rough"): a far frame must be this much nearer per frame of
// distance to win (a near-tie between two stretches of its looping path no longer flips per tick), and the written frame eases toward
// the chosen one with this time constant. My calls, constants; the van doors (felt fine) keep the plain follow.
constexpr float     kClipStickMPerFrame = 0.003f;
constexpr float     kClipEaseS = 0.05f;

// The use info +B9B930 takes, built as the game builds it (+D2D7A0 / +BC9510; §12e): 40 bytes.
struct UseInfo {
    void*         actor;
    int           slot;          // 0 = primary
    float         f0;            // 0.1
    float         f1;            // 2.6
    unsigned char flags;         // 0
    unsigned char pad[3];
    float         v[4];          // xmmword_181928640 = (1.5708, 1.5708, 1, 1)
};
static_assert(sizeof(UseInfo) == 40, "UseInfo layout (+D2D7A0 / +BC9510 build it on the stack: 8 + 4 + 4 + 4 + 4 + 16)");

struct StrBase { char* p; unsigned size; unsigned cap; };   // ttl::string_base<char> as the game passes it (ptr / size / cap)

using SetLocalFn = void(__fastcall*)(void* model, int elem, const float* m);
using GetLocalFn = const float*(__fastcall*)(void* model, int elem);
using GetIdFn = int(__fastcall*)(void* model, const char* name);
using GetWorldElemFn = const float*(__fastcall*)(void* model, int elem);
using GetParentFn = int(__fastcall*)(void* model, int elem);
using GetMeshNameFn = StrBase*(__fastcall*)(void* model, StrBase* out);
using GetXformFn = const float*(__fastcall*)(void* co);
using GetParentMeshFn = int(__fastcall*)(void* co);
using GetExtentsFn = const float*(__fastcall*)(void* co);
using ExecuteFn = void(__fastcall*)(void* player, void* trigger, UseInfo* info);
using ActionFn = int(__fastcall*)(void* trigger, UseInfo* info);
using PlayStageFn = char(__fastcall*)(void* obj, int key, int a3, char jump, char force);
using StageKeyFn = int(__fastcall*)(void* obj);
// The carried item's collision CATEGORY (trace docs/traces/20261120_1634_carried_item_category.md, design 2026-11-20-carried-item-
// headroom-design; Jay: "the character model crouches" over the shoulder). The player's stand-up check (+CDD890) is a capsule round
// the body that ignores only the player; a carried ContainerItem keeps the engine default category 1 and is seen -> the game's
// auto-crouch. While carried it gets 2048 = what the game gives DroppedItem / QuestLootableItem (+6E3F80 / +DFDF60), outside the
// query mask 0xFFC24307. ENGINE e9fbfc2c CPhysicsBind (vt +831BB8): slot 8 (+64) sets the category (+832, copied onto every geom
// +136), slot 11 (+88) returns it (ida/cd59_eng_out.txt).
// The carried item placed from the DRAWN hand every tick (trace 20261120_2000): the holder element rides the ANIMATION's hand (INV-27 /
// INV-191 - a two-handed rifle's foregrip for the left), so the item's root-level elements are written like ItemCtlPlace writes weapons
// (SetElementWorldMatrix - INV-178: item models are double-buffered, the API never a raw write).
using SetWorldFn = void(__fastcall*)(void* model, int elem, const float* m);
using ElemNumFn  = int(__fastcall*)(void* model);
constexpr int kMaxPlaced = 8;   // root-level elements written per carried item (loot meshes carry one or two)
using PhysicsBindFn = void*(__fastcall*)(void* co);
using BindGetFn = int(__fastcall*)(void* bind);
using BindSetFn = void(__fastcall*)(void* bind, int value);
constexpr size_t kBindSetCategoryOff = 64;   // CPhysicsBind slot 8  (+333E30)
constexpr size_t kBindGetCategoryOff = 88;   // CPhysicsBind slot 11 (+333F80)
constexpr int    kLooseItemCategory = 2048;  // the game's own loose-item category (DroppedItem / QuestLootableItem)

// ---- WORLD ITEMS by hand (plan 2026-11-20-world-items-by-hand, trace 20261120_2210, INV-758, RESEARCH-NATIVE-LOOTING §14) ----
// Physics OFF for a held DroppedItem = the game's own bone attach (+ECF2E0): PhysicsBind slot +256 (type 0, an empty string_base, 1).
// Its OnDetached (+6E94C0) restarts physics by itself at the release.
using BindPhysTypeFn = void*(__fastcall*)(void* bind, int type, StrBase* name, char flag);
constexpr size_t kBindPhysTypeOff = 256;     // CPhysicsBind slot 32 (+334270 -> inner vfunc +1248)
constexpr size_t kDropFlyingOff   = 1376;    // DroppedItem byte: flying to a target point (its CanExecute +6E2430 refuses then)
constexpr size_t kDropSpeedOff    = 1412;    // DroppedItem float: > 0 = the first physics step computes a flight (+6E6D70; INV-577)
// The game's drop of ONE inventory entry: PlayerDI::DropItem (Mac name) +B9EEF0(player, hand, InventoryItem*, bool, const vec3& dir,
// bool, bool removeFromInventory, int count, int group) -> the new DroppedItem (in front of the player, physics on, the plain drop's
// [+1412] = 2.5). Called as +BB9EC0 calls it: (player, -1, entry, 0, zero, 0, 1, count, 0). No EGL buildrva row: the head check refuses.
constexpr uintptr_t kDropItemRva = 0xB9EEF0;
using DropItemFn = void*(__fastcall*)(void* player, int hand, void* entry, unsigned char a4, const float* dir, unsigned char a6,
                                      char remove, int count, int group);
// The player's items, INVKEEP's verified enumeration (invkeep.cpp EnumGroup): [[gamedll +1C16358] + 1920] + 2360 = the container,
// its vfunc +120(container, out vector, category) for the six groups 4 1 8 3 0 9; an item's stack count = the first dword at +64 (INV-755).
constexpr uintptr_t kPlayerGlobalRva = 0x1C16358;
constexpr int kInvCats[6] = { 4, 1, 8, 3, 0, 9 };
constexpr int kMaxInv = 512;
// 24 bytes, as the game's (INV-760, trace 20261121_1614): every enumeration target writes a DWORD at +16 (51) - a 16-byte struct had
// those 4 bytes land on InvSnapshot's saved r15 (RUN CL1's crash). grow / pad are never read.
struct InvVec { void** d; uint32_t n; uint32_t cap; uint32_t grow; uint32_t pad; };
static_assert(sizeof(InvVec) == 24, "the game's inventory out vector is 24 bytes (INV-760)");
using InvEnumFn = void(__fastcall*)(void* cont, InvVec* out, int cat);
using SetXformFn = void(__fastcall*)(void* co, const float* m);   // IControlObject::SetWorldXform

// ---- CARRIED OBJECTS by hand (plan 2026-11-26-carried-objects-by-hand, trace 20261126_1500; facts ENGINE_ARCHITECTURE "WORLD SWITCHES
// + CARRIED OBJECTS", ghidra-project/ida/woc1..woc6, OUR gamedll 97046bba). Gas canisters / crates / bricks: the game's own take (action
// 29) is NEVER called - no ObjectsCarryController, no RT throw, the other hand keeps its weapon (Jay: "Keep its weapon and use it"). ----
// The take actions are lh::IsCarryAction (1 or 29 - RUN CO2: 1 when the object's own CanExecuteTake +1632 passes; woc1 +AA1820 /
// +B33350 / +B35B30)
constexpr size_t kPhysNameVfuncOff  = 528;   // obj vfunc (obj, StrBase* out, type) -> out: the physics script for a type (+B35D80 uses 4)
constexpr int    kPhysTypeDynamic   = 4;     // the spawner's / ResetPhysics' body type
constexpr size_t kSetLinVelVfuncOff = 432;   // ObjectDI slot 54 +EBAD60 SetPhysicsLinearVel(vec3) (throwable.cpp's kSetLinVelVfunc)
constexpr size_t kGetLinVelVfuncOff = 440;   // ObjectDI slot 55 +EBACF0 GetPhysicsLinearVel(out) -> out (zero with no body)
constexpr size_t kSpawnVfuncOff     = 1560;  // HeavyObjectsSpawner slot 195 +B35D80: create + place the HeavyObject it hands out
constexpr size_t kSpawnHasOff = 880, kSpawnObjOff = 888;   // the spawner's handed-out object (smart pointer: +880 set -> +888)
constexpr size_t kCreditHeavyOff = 912, kCreditExplodingOff = 1344;   // the game's throw +D5A8E0 (exact class): the thrower
constexpr size_t kPlayerHandleOff = 88;      // what the game passes with the player (+D5A8E0: player + 88); kGsoOff (40) is above
constexpr uintptr_t kSmartSetRva = 0x8E2D0;  // sub_18008E2D0(field, handle, obj) - the game's smart-pointer set (+D5A8E0 :1989)
constexpr uintptr_t kIgnoreRva = 0xBACA30;   // (player, obj co, seconds): the player ignores the object (+D5A8E0, DropItem +B9EEF0)
constexpr double kCarryVelWindowMs = 60.0;   // the let-go velocity: the fastest >= 30 ms span in the last 60 ms (my call)
constexpr size_t kExplodedFlagOff = 1008;    // ExplodingObject byte: set 1 by its explosion +A9E720 (woc4 :39; +1020 -> 2 / 3 there too)
using NameForTypeFn  = StrBase*(__fastcall*)(void* obj, StrBase* out, int type);
using VecSetFn       = void(__fastcall*)(void* obj, const float* v);
using VecGetFn       = float*(__fastcall*)(void* obj, float* out);
using SpawnFn        = void(__fastcall*)(void* spawner);
using SmartSetFn     = void(__fastcall*)(void* field, void* handle, void* obj);
using IgnoreFn       = void(__fastcall*)(void* player, void* co, float seconds);
using ReplIntervalFn = void(__fastcall*)(void* gso, float a, float b);

SetLocalFn      g_setLocal = nullptr;
GetLocalFn      g_getLocal = nullptr;
GetIdFn         g_getId = nullptr;
GetWorldElemFn  g_getWorldElem = nullptr;
GetParentFn     g_getParent = nullptr;
GetMeshNameFn   g_getMeshName = nullptr;
GetXformFn      g_getXform = nullptr;
GetParentMeshFn g_getParentMesh = nullptr;
GetExtentsFn    g_getExtents = nullptr;
ExecuteFn       g_execute = nullptr;
PlayStageFn     g_playStage = nullptr;   // null = the open jump is off (head check failed) - the open behaves as before
PhysicsBindFn   g_physBind = nullptr;    // null = the carried item keeps its category (named on the READY line); all else as before
SetWorldFn      g_setWorld = nullptr;    // null (or g_elemNum null) = the carried item rides the holder as before (named on READY)
ElemNumFn       g_elemNum = nullptr;
uintptr_t       g_vtContainer = 0, g_vtItem = 0;
// world items (trace 20261120_2210): 0 = that class not found by RTTI (named at READY) - only that kind is not taken by hand
uintptr_t       g_vtDropped = 0, g_vtFlare = 0, g_vtSpawner = 0, g_vtSpawnerR = 0, g_vtSpawnerC = 0;
DropItemFn      g_dropItem = nullptr;    // null = its head bytes do not match (EGL) - a let-go chest / placed item goes home instead
SetXformFn      g_setXform = nullptr;    // null = the dropped item stays where the game's drop put it (named at READY)
// carried objects (trace 20261126_1500): 0 / null = that class / call not found (named on the CARRIED OBJECTS line) - only it is off
uintptr_t       g_vtExploding = 0, g_vtHeavy = 0, g_vtHeavySpawner = 0;
SmartSetFn      g_smartSet = nullptr;    // null = no throw credit (head bytes differ - EGL has no row)
IgnoreFn        g_ignorePlayer = nullptr;   // null = no 1 s player ignore at the let-go
ReplIntervalFn  g_replInterval = nullptr;   // IGSObject::ReplSetLocalReplicationInterval (engine export)
volatile LONG   g_nThrow = 0;
volatile LONG   g_ready = 0;      // 0 not tried, 1 ready, -1 refused (named once)
void* volatile  g_player = nullptr;   // the PlayerDI NearbyHook was called with (the execute's actor)
// THE INSTANCE CHECK (trace 20261125_1610 v2): the object's own id and its destroying / destroyed flags. Engine exports (dumpbin
// e9fbfc2c): GetID +252080 = *(*(this+32)+80); IsDestroying +252250; IsDestroyed +252280 - the delete +2384D0 sets the first before
// the second, both before the free (INV-663 addendum). Any null = the check is OFF (named at READY), remembered lids trusted as before.
using ObjIdFn = int(__fastcall*)(const void* gso);
using ObjFlagFn = bool(__fastcall*)(const void* gso);
ObjIdFn         g_objId = nullptr;
ObjFlagFn       g_objDying = nullptr, g_objDead = nullptr;
volatile LONG   g_nStaleSlot = 0, g_nStaleId = 0;   // run totals: slots dropped (game thread), cache entries dropped (HUD thread)

// ---- published by LidHandOnNearby (HUD) -> LidHandTick (game) ----
volatile LONG g_pubSeq = 0;
int   g_pubLidN = 0, g_pubItemN = 0;
void* g_pubLid[kMaxPub] = {};
int   g_pubLidRow[kMaxPub] = {};
int   g_pubLidBone[kMaxPub] = {};
float g_pubLidBox[kMaxPub][6] = {};
void* g_pubItem[kMaxPub] = {};
float g_pubItemBox[kMaxPub][6] = {};
int   g_pubItemKind[kMaxPub] = {};   // lh::ItemKind as int (trace 20261120_2210)
int   g_pubLootN = 0;                // grip-loot containers (purses, trace 20261121_2145) - same seqlock
void* g_pubLoot[kMaxPub] = {};
float g_pubLootBox[kMaxPub][6] = {};

// identity cache (HUD thread only): object -> its doors' rows / bones (n = 0: not a lid), checked once per object pointer. A 2-door
// object (wardrobe / cabinet, trace 20261121_1549) = two rows of the same mesh, each its own lid. loot = a grip-loot mesh (a purse).
constexpr int kMaxDoors = 2;
// gid / gidOk (trace 20261125_1610 v2): the object's IGSObject id when the entry was made - a later hit on the same address with another
// live id is a different object built into a freed block, and is identified afresh.
struct IdCache { void* obj; int n; int row[kMaxDoors]; int bone[kMaxDoors]; bool loot; int gid; bool gidOk; };
IdCache g_idc[64] = {};
int     g_idcNext = 0;

// ---- per hand / per lid (game thread), and what the pad thread reads ----
struct LidState {
    void* obj = nullptr; int row = -1; int bone = -1;
    float deg = 0.0f;           // our last written angle (a CLIP row: the frame position t - trace 20261121_2145)
    bool  writing = false;      // write every tick (off rest, owned, or released off rest)
    bool  gameClip = false;     // the game's own clip took it at the open (option A) - no write until the next grab
    bool  overwriteSaid = false;
    // the open (trace 20261119_1310): the game's pose jumped to OPEN, our write eases from the hand's angle to it, then stops
    bool  easing = false;
    float easeFrom = 0.0f;
    LONGLONG easeT0 = 0;        // QPC
    bool  checkHold = false;    // the first tick after the ease: log the game's own pose once
    // a 2-door object's door after that object's game open (trace 20261121_1549, Jay: the other door "Stays shut"): written every tick
    // even at rest, so the game's jumped open pose never shows on a door the hand did not open
    bool  hold = false;
    // the object's IGSObject id this slot belongs to (trace 20261125_1610 v2) - checked against the object itself every tick
    int   gid = -1;
    bool  gidOk = false;
};
LidState g_lids[kMaxLids];
struct HandState {
    int   lid = -1;             // index into g_lids while this hand owns a lid
    float lidAtGrab = 0.0f, grabW[3] = {};
    float clipG[3] = {};        // a CLIP row (trace 20261121_2145): the hand's point in the bone's own frame at the grab
    float clipT = 0.0f;         // the basket cover (trace 20261121_2225): the eased frame position written ...
    LONGLONG clipQpc = 0;       // ... and the QPC of its last tick (0 = the first tick after the grab)
    bool  prevDown = false;
    bool  prevZone = false;     // the hand was in a lid's / an item's grab zone last tick (the entry buzz's edge)
    HandCarry carry;            // the item while carried
    void* item = nullptr;
    void* itemOwner = nullptr;
    int   savedCat = 0;         // the item's own collision category, read at the take (restored at the give-back)
    bool  catSet = false;       // we set kLooseItemCategory on the carried item and owe it the restore
    // the drawn-hand placement (trace 20261120_2000): the item's root-level elements and their locals, latched at the take
    int   placeN = 0;
    int   placeElem[kMaxPlaced] = {};
    float placeLocal[kMaxPlaced][12] = {};
    bool  placeSaidFail = false;
    // Jay's approved grip for the carried mesh (trace 20261120_2057): its row, and the item frame in the drawn socket frame (identity =
    // today's placement when the mesh has no row)
    int   poseRow = -1;
    float rel[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    // what is carried (trace 20261120_2210): a chest item (home = its hook), a placed spawner or a dropped item (home = a world matrix)
    lidhand::ItemKind kind = lidhand::ItemKind::Chest;
    bool  physOff = false;      // we switched a DroppedItem's physics off at the take (its OnDetached restarts it)
    throwable::HandHistory hist;   // a carriable's hand positions (game world, QPC ms) from the take - its let-go velocity
    // THE FREE LIFT (trace 20261122_1830): the owned lid rides the drawn holder - the cover's world in the holder's frame at the grab,
    // and the rim centre's world at the row's rest (frame 0) for the let-go rule
    bool  free = false;
    float freeRel[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float freeRestC[3] = {};
    bool  freeSaidNoHolder = false;
};
HandState g_hand[2];            // 0 = right, 1 = left (RigGripWorldRead's sides)
volatile LONG g_busy[2] = {};   // tick -> pad: this hand owns a lid / item, or is next to one
volatile LONG g_padOwned[2] = {};   // pad -> tick: an earlier poller owns this press
volatile LONG g_zone[2] = {};       // tick -> pad: this hand is in a lid's edge / an item's zone, or owns one (the hip yield, trace 20261119_1215)
void* volatile g_carried[2] = {};   // tick -> HUD: the items carried (their pick is nulled)
volatile LONG g_nGrab = 0, g_nOpen = 0, g_nTake = 0, g_nStow = 0, g_nGiveBack = 0, g_nForgot = 0, g_nOverwrite = 0, g_nNull = 0;
volatile LONG g_nDrop = 0;   // let-go drops with physics (trace 20261120_2210)
volatile LONG g_nCarryNull = 0;   // carriable picks withheld (X has no target) - trace 20261126_1815

// ---- SEH-guarded game access (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadI32(uintptr_t a, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// THE INSTANCE CHECK (trace 20261125_1610 v2): obj's IGSObject id and whether it is being / has been destroyed, read from obj itself
// (it may be a freed block - the same SEH-guarded exposure IsVt already has every tick). False = the check is off or a read faulted.
// COST: three export calls (2-3 reads each).
bool ObjIdentity(void* obj, int* id, bool* dying)
{
    if (!obj || !g_objId || !g_objDying || !g_objDead) { return false; }
    __try {
        const void* g = static_cast<const char*>(obj) + kGsoOff;
        *id = g_objId(g);
        *dying = g_objDying(g) || g_objDead(g);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The level and its next id through a PLAYER's IGSObject record (player+40 -> +32 record, +72 level, level+3452). False = not read.
// COST: three guarded reads.
bool LevelStamp(void* player, uintptr_t* level, int* next)
{
    uintptr_t rec = 0, lv = 0;
    int n = 0;
    if (!player || !ReadPtr(reinterpret_cast<uintptr_t>(player) + kGsoOff + 32, &rec) || !rec || !ReadPtr(rec + kRecLevelOff, &lv) ||
        !lv || !ReadI32(lv + kLevelNextIdOff, &n)) {
        return false;
    }
    *level = lv;
    *next = n;
    return true;
}
bool SafeBox(void* obj, float out[6])
{
    __try {
        const float* e = g_getExtents(static_cast<char*>(obj) + kCtlObjOff);
        if (!e) { return false; }
        for (int k = 0; k < 6; ++k) { out[k] = e[k]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeMeshName(void* obj, char* out, size_t n)
{
    // The returned string is NOT freed (the engine's heap is not ours to free into); called once per object (the identity cache).
    __try {
        StrBase s{};
        g_getMeshName(obj, &s);
        if (!s.p) { return false; }
        strncpy_s(out, n, s.p, _TRUNCATE);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeElemId(void* obj, const char* name, int* out)
{
    __try { *out = g_getId(obj, name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeLocal(void* obj, int elem, float out[12])
{
    __try {
        const float* m = g_getLocal(obj, elem);
        if (!m) { return false; }
        memcpy(out, m, 12 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetLocal(void* obj, int elem, const float m[12])
{
    __try { g_setLocal(obj, elem, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The lid bone's PARENT frame in world: its parent element's world, or the object's world xform when it is a root-level element.
bool SafeParentWorld(void* obj, int elem, float out[12])
{
    __try {
        const int p = g_getParent(obj, elem);
        const float* m = p >= 0 ? g_getWorldElem(obj, p) : g_getXform(static_cast<char*>(obj) + kCtlObjOff);
        if (!m) { return false; }
        memcpy(out, m, 12 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeParentMeshElem(void* obj, int* out)
{
    __try { *out = g_getParentMesh(static_cast<char*>(obj) + kCtlObjOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeExecute(void* player, void* trig)
{
    UseInfo info{};
    info.actor = player;
    info.slot = 0;
    info.f0 = 0.1f;
    info.f1 = 2.6f;
    info.flags = 0;
    info.v[0] = 1.5707964f; info.v[1] = 1.5707964f; info.v[2] = 1.0f; info.v[3] = 1.0f;
    __try { g_execute(player, trig, &info); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The open jump (trace 20261119_1310): the key the state setter itself uses (vfunc +1416), then +EF4340 with jump + force -> the
// container's own Open stage at its END pose. *key = -1 when the read faulted.
bool SafeOpenJump(void* obj, int* key)
{
    *key = -1;
    if (!g_playStage) { return false; }
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const StageKeyFn kf = *reinterpret_cast<const StageKeyFn*>(vt + kStageKeySlotOff);
        *key = kf(obj);
        if (*key <= 0) { return false; }
        g_playStage(obj, *key, 0, 1, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeElemNum(void* obj, int* out)
{
    if (!g_elemNum || !obj) { return false; }
    __try { *out = g_elemNum(obj); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeElemParent(void* obj, int elem, int* out)
{
    __try { *out = g_getParent(obj, elem); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetWorld(void* obj, int elem, const float m[12])
{
    if (!g_setWorld || !obj) { return false; }
    __try { g_setWorld(obj, elem, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a 3x4 (rows; translation in [3] [7] [11]) times a 3x4: out = a . b
void Mul34(const float a[12], const float b[12], float out[12])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = a[4 * r + 0] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c];
            if (c == 3) { v += a[4 * r + 3]; }
            out[4 * r + c] = v;
        }
    }
}

// The carried item's collision category through its IPhysicsBind (see kLooseItemCategory). SEH-guarded; false = not read / not set.
bool SafeGetCategory(void* obj, int* out)
{
    if (!g_physBind || !obj) { return false; }
    __try {
        void* bind = g_physBind(static_cast<char*>(obj) + kCtlObjOff);
        if (!bind) { return false; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        *out = (*reinterpret_cast<const BindGetFn*>(vt + kBindGetCategoryOff))(bind);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetCategory(void* obj, int cat)
{
    if (!g_physBind || !obj) { return false; }
    __try {
        void* bind = g_physBind(static_cast<char*>(obj) + kCtlObjOff);
        if (!bind) { return false; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        (*reinterpret_cast<const BindSetFn*>(vt + kBindSetCategoryOff))(bind, cat);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeAction(void* trig, void* player, int* out)
{
    UseInfo info{};
    info.actor = player;
    info.f0 = 0.1f;
    info.f1 = 2.6f;
    info.v[0] = 1.5707964f; info.v[1] = 1.5707964f; info.v[2] = 1.0f; info.v[3] = 1.0f;
    __try {
        uintptr_t vt = 0, fn = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(trig), &vt) || !ReadPtr(vt + kActionSlotOff, &fn) || !fn) { return false; }
        *out = reinterpret_cast<ActionFn>(fn)(trig, &info);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The trigger's own CanExecute(action, &info) (vfunc +1048, RESEARCH-NATIVE-LOOTING §12d; ExplodingObject +AA1950 / HeavyObject +B33470 /
// HeavyObjectsSpawner +B35C10 - woc1): enabled, active on module, a pile with none out, the player able (final review findings 1 / 2).
using CanExecFn = bool(__fastcall*)(void* trigger, int action, UseInfo* info);
constexpr size_t kCanExecSlotOff = 1048;
bool SafeCanExecute(void* trig, void* player, int action, bool* out)
{
    UseInfo info{};
    info.actor = player;
    info.f0 = 0.1f;
    info.f1 = 2.6f;
    info.v[0] = 1.5707964f; info.v[1] = 1.5707964f; info.v[2] = 1.0f; info.v[3] = 1.0f;
    __try {
        uintptr_t vt = 0, fn = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(trig), &vt) || !ReadPtr(vt + kCanExecSlotOff, &fn) || !fn) { return false; }
        *out = reinterpret_cast<CanExecFn>(fn)(trig, action, &info);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// ---- world items (trace 20261120_2210) ----
// Physics OFF on a held DroppedItem: PhysicsBind slot +256 (type 0, empty name, 1) - the game's own +ECF2E0 call before its bone attach.
bool SafePhysicsOff(void* obj)
{
    if (!g_physBind || !obj) { return false; }
    __try {
        void* bind = g_physBind(static_cast<char*>(obj) + kCtlObjOff);
        if (!bind) { return false; }
        StrBase none{};
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
        (*reinterpret_cast<const BindPhysTypeFn*>(vt + kBindPhysTypeOff))(bind, 0, &none, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeReadByte(uintptr_t a, unsigned char* out)
{
    __try { *out = *reinterpret_cast<const volatile unsigned char*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// [obj+1412] = 0.0f: the first physics step then computes no flight (target zero, not `again`) - the item falls (RESEARCH §14 (6b)).
bool SafeZeroDropSpeed(void* obj, float* was)
{
    __try {
        float* p = reinterpret_cast<float*>(static_cast<char*>(obj) + kDropSpeedOff);
        *was = *p;
        *p = 0.0f;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetXform(void* obj, const float m[12])
{
    if (!g_setXform || !obj) { return false; }
    __try { g_setXform(static_cast<char*>(obj) + kCtlObjOff, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeGetXform(void* obj, float out[12])
{
    if (!g_getXform || !obj) { return false; }
    __try {
        const float* m = g_getXform(static_cast<char*>(obj) + kCtlObjOff);
        if (!m) { return false; }
        memcpy(out, m, 12 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// ---- carried objects (trace 20261126_1500) ----
bool IsVt(void* obj, uintptr_t vt);   // below
// Physics ON at the let-go, the spawner's own idiom (+B35D80): the object's physics script for type 4 (vfunc +528 into a string_base the
// game allocates - freed with the same CRT free as the inventory buffer), then PhysicsBind slot +256 (4, name, 1). These classes' detach
// restarts nothing (their OnDetached = the engine base, +114B0 = ret - woc5 / woc6), so this is the only restart.
bool SafePhysicsOn(void* obj)
{
    if (!g_physBind || !obj) { return false; }
    StrBase name{};
    bool ok = false;
    __try {
        const uintptr_t ovt = *reinterpret_cast<const uintptr_t*>(obj);
        StrBase* nm = (*reinterpret_cast<const NameForTypeFn*>(ovt + kPhysNameVfuncOff))(obj, &name, kPhysTypeDynamic);
        void* bind = g_physBind(static_cast<char*>(obj) + kCtlObjOff);
        if (bind && nm) {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(bind);
            (*reinterpret_cast<const BindPhysTypeFn*>(vt + kBindPhysTypeOff))(bind, kPhysTypeDynamic, nm, 1);
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    free(name.p);
    return ok;
}
// The game's throw credit (+D5A8E0): by EXACT class, as the game tests it - HeavyObject +912, ExplodingObject +1344 (a derived
// GasTankObject / QuestExplodingObject is not credited by the game either) = the player, then its replication interval (100, 300).
// *field = the offset written (0 = not one of the two exact classes).
// The outcome by its real reason (review minor 6, trace 20261126_1800): no export / not one of the two exact classes / a fault.
lh::CreditResult SafeCredit(void* obj, void* player, int* field)
{
    *field = 0;
    if (!g_smartSet) { return lh::CreditResult::NoExport; }
    if (!obj || !player) { return lh::CreditResult::NotCalled; }
    const size_t off = IsVt(obj, g_vtHeavy) ? kCreditHeavyOff : IsVt(obj, g_vtExploding) ? kCreditExplodingOff : 0;
    if (!off) { return lh::CreditResult::NotExactClass; }
    __try {
        g_smartSet(static_cast<char*>(obj) + off, static_cast<char*>(player) + kPlayerHandleOff, player);
        if (g_replInterval) { g_replInterval(static_cast<char*>(obj) + kGsoOff, 100.0f, 300.0f); }
        *field = static_cast<int>(off);
        return lh::CreditResult::Given;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return lh::CreditResult::Faulted; }
}
bool SafeIgnorePlayer(void* player, void* obj)
{
    if (!g_ignorePlayer || !player || !obj) { return false; }
    __try { g_ignorePlayer(player, static_cast<char*>(obj) + kCtlObjOff, 1.0f); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeSetLinVel(void* obj, const float v[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        (*reinterpret_cast<const VecSetFn*>(vt + kSetLinVelVfuncOff))(obj, v);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeGetLinVel(void* obj, float out[3])
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        float tmp[4] = {};
        const float* p = (*reinterpret_cast<const VecGetFn*>(vt + kGetLinVelVfuncOff))(obj, tmp);
        if (!p) { return false; }
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A pile (HeavyObjectsSpawner): its own spawn (vfunc +1560 = +B35D80, the call its execute makes) once, then the object it handed out.
// nullptr = refused (one already out: +880 && +888 before the call; nothing at +888 after it; a fault).
void* SafeSpawnFrom(void* spawner)
{
    __try {
        uintptr_t has = 0, cur = 0;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(spawner) + kSpawnHasOff, &has) ||
            !ReadPtr(reinterpret_cast<uintptr_t>(spawner) + kSpawnObjOff, &cur)) { return nullptr; }
        if (has && cur) { return nullptr; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(spawner);
        (*reinterpret_cast<const SpawnFn*>(vt + kSpawnVfuncOff))(spawner);
        if (!ReadPtr(reinterpret_cast<uintptr_t>(spawner) + kSpawnHasOff, &has) || !has ||
            !ReadPtr(reinterpret_cast<uintptr_t>(spawner) + kSpawnObjOff, &cur) || !cur) { return nullptr; }
        return reinterpret_cast<void*>(cur);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// +8E2D0's first 16 bytes (set-aside gamedll 97046bba, read 2026-11-26 heads.py; control: the same read of +B9B930 == ExecHeadOk):
// mov [rsp+8],rbx / mov r9,[rcx] / xor ebx,ebx / mov r11,r8 / test r9,r9. Mismatch = no credit, named.
bool SmartSetHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x8B, 0x09, 0x33, 0xDB, 0x4D, 0x8B, 0xD8, 0x4D, 0x85, 0xC9 };
    __try {
        for (int i = 0; i < 16; ++i) { if (p[i] != kHead[i]) { return false; } }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// +BACA30's first 16 bytes (same read): test rdx,rdx / jz rel32 / push rdi / push r14 / sub rsp,48h - the rel32 (5-8) not compared.
bool IgnoreHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[16] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x8C, 0x01, 0x00, 0x00, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48 };
    __try {
        for (int i = 0; i < 16; ++i) {
            if (i >= 5 && i <= 8) { continue; }
            if (p[i] != kHead[i]) { return false; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A gas canister / crate / pile the game would carry RIGHT NOW (trace 20261126_1500): one of the three exact classes, the game's own
// action test (+1432) answers a take action (1 or 29, RUN CO2), AND its own CanExecute(that action) agrees (final review findings 1 / 2: the action test does NOT see a pile
// with its object out, a disabled object or one inactive on its module - CanExecute does, woc1 +B35C10 / +AA1950 / +B33470).
// HUD thread (NearbyHook); two vfunc calls per candidate of these classes per gather.
bool CarriableNow(void* obj, void* player)
{
    if (!(IsVt(obj, g_vtExploding) || IsVt(obj, g_vtHeavy) || IsVt(obj, g_vtHeavySpawner)) || !player) { return false; }
    int action = -1;
    bool can = false;
    const bool actOk = SafeAction(obj, player, &action);
    // RUN CO2: the take action is 1 (CanExecuteTake passed) or 29 - CanExecute is asked with the action the game returned
    const bool canOk = actOk && lh::IsCarryAction(action) && SafeCanExecute(obj, player, action, &can);
    const bool yes = canOk && lh::CarriableGrabbable(action, can);
    // RUN CO1 diagnostic (Jay: "Yes, go ahead" - grip did not take a canister that showed the game's X prompt): the first 12 candidates
    // of the three classes with every test's value - so a refusal names itself. COST: one interlocked increment per candidate after 12.
    static LONG s_n = 0;
    const LONG n = InterlockedIncrement(&s_n);
    if (n <= 12) {
        char mesh[96] = "(name not read)";
        if (g_getMeshName) { SafeMeshName(obj, mesh, sizeof(mesh)); }
        // [PROBE: CARRYHAND] gate=lid_by_hand  site=FLAT  log-only=yes
        LOG("lidhand: [CARRYHAND] CANDIDATE %ld of 12: %s %p mesh '%s' | action %s %d (want 1 or 29) | CanExecute(that action) %s | -> %s", n,
            IsVt(obj, g_vtExploding) ? "ExplodingObject" : IsVt(obj, g_vtHeavy) ? "HeavyObject" : "HeavyObjectsSpawner", obj, mesh,
            actOk ? "read" : "*** NOT READ (fault / no vtable slot) ***", action,
            !canOk ? (actOk && lh::IsCarryAction(action) ? "*** NOT READ ***" : "not asked (not a take action)") : (can ? "true" : "FALSE"),
            yes ? "PUBLISHED (grabbable)" : "*** NOT published ***");
    }
    return yes;
}
// QPC ticks -> ms (the rig's grip read carries its QPC)
double QpcMs(long long qpc)
{
    static LARGE_INTEGER s_f = {};
    if (!s_f.QuadPart) { QueryPerformanceFrequency(&s_f); }
    return s_f.QuadPart ? static_cast<double>(qpc) * 1000.0 / static_cast<double>(s_f.QuadPart) : 0.0;
}
void* SafeDropItem(void* player, void* entry, int count)
{
    if (!g_dropItem || !player || !entry || count < 1) { return nullptr; }
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    __try { return g_dropItem(player, -1, entry, 0, zero, 0, 1, count, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// +B9EEF0's first 15 bytes (set-aside gamedll 97046bba, read 2026-11-20 (59); control: the same read of +B9B930 == ExecHeadOk's kHead):
// mov [rsp+18h],rbx / push rbp / push rsi / push rdi / sub rsp,0F0h. Mismatch = the drop is off, named at READY.
bool DropHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[15] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };
    __try {
        for (int i = 0; i < 15; ++i) {
            if (p[i] != kHead[i]) { return false; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The player's inventory entries + their stack counts (INVKEEP's enumeration, read only). False = no inventory / a fault.
// + each entry's ItemID (trace 20261121_1627; lh::kNoId when it did not read).
struct InvSnap { int n = 0; void* p[kMaxInv]; int count[kMaxInv]; int id[kMaxInv]; };
// An item's ItemID - the game's own stack-match key (InventoryMain add +CD4D60, ida/cd71_ours_out.txt): *(DWORD*)vfunc+432 of the
// item's DESC OBJECT. The desc object: InventoryItem +88; ContainerItem +840 (its header +816, take +A6A280); ItemSpawner +920 (+896,
// take +70EF80). lh::kNoId when the call or the read faults / returns null.
constexpr size_t kInvDescObjOff = 88, kChestDescObjOff = 840, kSpawnerDescObjOff = 920;
using ItemIdFn = const int*(__fastcall*)(void* descObj);
int SafeItemId(void* descObj)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(descObj);
        const int* p = (*reinterpret_cast<const ItemIdFn*>(vt + 432))(descObj);
        return p ? *p : lh::kNoId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return lh::kNoId; }
}
bool InvEnumOne(void* cont, int cat, InvVec* v)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(cont);
        (*reinterpret_cast<const InvEnumFn*>(vt + 120))(cont, v, cat);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InvCountOf(void* item, int* out)
{
    __try { *out = *reinterpret_cast<const int*>(static_cast<char*>(item) + 64); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InvSnapshot(InvSnap& s)
{
    s.n = 0;
    uintptr_t g = 0, inv = 0, cont = 0;
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!gd || !ReadPtr(reinterpret_cast<uintptr_t>(gd) + DLVR_LID_GD_RVA(kPlayerGlobalRva), &g) || !g || !ReadPtr(g + 1920, &inv) || !inv ||
        !ReadPtr(inv + 2360, &cont) || !cont) {
        return false;
    }
    for (int c = 0; c < 6; ++c) {
        InvVec v = {};
        if (!InvEnumOne(reinterpret_cast<void*>(cont), kInvCats[c], &v)) { return false; }
        for (uint32_t i = 0; i < v.n && s.n < kMaxInv; ++i) {
            void* it = v.d ? v.d[i] : nullptr;
            int cnt = 0;
            if (it && InvCountOf(it, &cnt)) {
                s.p[s.n] = it; s.count[s.n] = cnt; s.id[s.n] = SafeItemId(static_cast<char*>(it) + kInvDescObjOff); ++s.n;
            }
        }
        free(v.d);   // the game's realloc'd buffer - one ucrtbase heap (invkeep.cpp:217)
    }
    return true;
}
bool IsVt(void* obj, uintptr_t vt)
{
    uintptr_t v = 0;
    return obj && vt && ReadPtr(reinterpret_cast<uintptr_t>(obj), &v) && v == vt;
}
int State(void* obj)
{
    int s = -1;
    return ReadI32(reinterpret_cast<uintptr_t>(obj) + kStateOff, &s) ? s : -1;
}

void MulRot(const float m[12], const float v[3], float out[3])
{
    for (int r = 0; r < 3; ++r) { out[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2]; }
}
void MulPoint(const float m[12], const float v[3], float out[3])
{
    MulRot(m, v, out);
    for (int r = 0; r < 3; ++r) { out[r] += m[r * 4 + 3]; }
}

// +B9B930's first 16 bytes (set-aside gamedll 97046bba, 2026-11-18; control +B9BBB0 read looting's kNearbyBytes): test rdx,rdx /
// jz rel32 / push rbp / push rsi / push rdi / sub rsp,40h. The jz's rel32 (bytes 5-8) is not compared (another build may move its
// target); everything else must match, or the execute is never called (called, not patched).
bool ExecHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[16] = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x02, 0x02, 0x00, 0x00, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    __try {
        for (int i = 0; i < 16; ++i) {
            if (i >= 5 && i <= 8) { continue; }
            if (p[i] != kHead[i]) { return false; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// +EF4340's first 14 bytes (set-aside gamedll 97046bba, read 2026-11-19 with scratchpad rvabytes.py; the same read of +B9B930 matched
// kHead above = the control): mov [rsp+20h],rbx / push rbp / push r14 / push r15 / sub rsp,30h. Mismatch = the jump is off, named.
bool PlayStageHeadOk(const unsigned char* p)
{
    static const unsigned char kHead[14] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
    __try {
        for (int i = 0; i < 14; ++i) {
            if (p[i] != kHead[i]) { return false; }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Resolve once: the engine exports by name, the execute, the two vtables by RTTI name. Refuses by name.
bool Ready()
{
    if (g_ready != 0) { return g_ready == 1; }
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    if (!en || !gd || !HandCarryReady()) { return false; }   // not mapped yet / hand-carry not resolved yet: retried
    g_setLocal = reinterpret_cast<SetLocalFn>(GetProcAddress(en, "?SetElementLocalMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    g_getLocal = reinterpret_cast<GetLocalFn>(GetProcAddress(en, "?GetElementLocalMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_getId = reinterpret_cast<GetIdFn>(GetProcAddress(en, "?GetElementID@IModelObject@@QEBAHPEBD@Z"));
    g_getWorldElem = reinterpret_cast<GetWorldElemFn>(GetProcAddress(en, "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z"));
    g_getParent = reinterpret_cast<GetParentFn>(GetProcAddress(en, "?GetElementParent@IModelObject@@QEBAHH@Z"));
    g_getMeshName = reinterpret_cast<GetMeshNameFn>(GetProcAddress(en, "?GetMeshName@IModelObject@@QEBA?AV?$string_base@D@ttl@@XZ"));
    g_getXform = reinterpret_cast<GetXformFn>(GetProcAddress(en, "?GetWorldXform@IControlObject@@QEBAAEBVmtx34@@XZ"));
    g_getParentMesh = reinterpret_cast<GetParentMeshFn>(GetProcAddress(en, "?GetParentMeshElement@IControlObject@@QEBAHXZ"));
    g_getExtents = reinterpret_cast<GetExtentsFn>(GetProcAddress(en, "?GetExtentsInWorld@IControlObject@@QEBAAEBVextents@@XZ"));
    // not in `missing`: without it only the carried item's category is left alone (named below)
    g_physBind = reinterpret_cast<PhysicsBindFn>(GetProcAddress(en, "?PhysicsBind@IControlObject@@QEAAPEAVIPhysicsBind@@XZ"));
    // not in `missing` either: without them the carried item rides the holder (the animation's hand) as before - named below.
    // The same export strings ItemCtlPlace / ResolveAccessors resolve (boneblend.cpp ItemCtlPlace, the element-count lookup).
    g_setWorld = reinterpret_cast<SetWorldFn>(GetProcAddress(en, "?SetElementWorldMatrix@IModelObject@@QEAAXHAEBVmtx34@@@Z"));
    g_elemNum = reinterpret_cast<ElemNumFn>(GetProcAddress(en, "?GetElementsNumber@IModelObject@@QEBAHXZ"));
    const uintptr_t gdb = reinterpret_cast<uintptr_t>(gd);
    int s1 = -1, s2 = -1;
    g_vtContainer = RttiFindVtable(gdb, ".?AVComplexContainer@@", 0, &s1);
    g_vtItem = RttiFindVtable(gdb, ".?AVContainerItem@@", 0, &s2);
    g_execute = reinterpret_cast<ExecuteFn>(gdb + DLVR_LID_GD_RVA(kExecuteRva));
    const bool headOk = ExecHeadOk(reinterpret_cast<const unsigned char*>(g_execute));
    {
        void* ps = reinterpret_cast<void*>(gdb + DLVR_LID_GD_RVA(kPlayStageRva));
        g_playStage = PlayStageHeadOk(static_cast<const unsigned char*>(ps)) ? reinterpret_cast<PlayStageFn>(ps) : nullptr;
    }
    const char* missing = !g_setLocal ? "SetElementLocalMatrix" : !g_getLocal ? "GetElementLocalMatrix" : !g_getId ? "GetElementID"
                        : !g_getWorldElem ? "GetElementWorldMatrix" : !g_getParent ? "GetElementParent"
                        : !g_getMeshName ? "GetMeshName" : !g_getXform ? "GetWorldXform" : !g_getParentMesh ? "GetParentMeshElement"
                        : !g_getExtents ? "GetExtentsInWorld" : !g_vtContainer ? "ComplexContainer vtable (RTTI)"
                        : !g_vtItem ? "ContainerItem vtable (RTTI)" : !headOk ? "the execute's code" : nullptr;
    if (missing) {
        InterlockedExchange(&g_ready, -1);
        // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
        LOG("lidhand: [LIDHAND] REFUSED - %s not found (RTTI stages container %d item %d) - lids open the game's way", missing, s1, s2);
        return false;
    }
    // WORLD ITEMS (trace 20261120_2210): each class by RTTI; a missing one disables only that kind (named). Not in `missing`.
    {
        int t[5] = { -1, -1, -1, -1, -1 };
        g_vtDropped = RttiFindVtable(gdb, ".?AVDroppedItem@@", 0, &t[0]);
        g_vtFlare = RttiFindVtable(gdb, ".?AVFlare@@", 0, &t[1]);
        g_vtSpawner = RttiFindVtable(gdb, ".?AVItemSpawner@@", 0, &t[2]);
        g_vtSpawnerR = RttiFindVtable(gdb, ".?AVItemSpawnerRandom@@", 0, &t[3]);
        g_vtSpawnerC = RttiFindVtable(gdb, ".?AVChallengeItemSpawner@@", 0, &t[4]);
        void* dp = reinterpret_cast<void*>(gdb + DLVR_LID_GD_RVA(kDropItemRva));
        g_dropItem = DropHeadOk(static_cast<const unsigned char*>(dp)) ? reinterpret_cast<DropItemFn>(dp) : nullptr;
        g_setXform = reinterpret_cast<SetXformFn>(GetProcAddress(en, "?SetWorldXform@IControlObject@@QEAAXAEBVmtx34@@@Z"));
        auto rv = [gdb](uintptr_t v) { return static_cast<unsigned long long>(v ? v - gdb : 0); };
        // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
        LOG("lidhand: [LIDHAND] WORLD ITEMS (logged unconditionally) - DroppedItem vt +%llX (want +153F4E0), Flare +%llX (want +1792720), "
            "ItemSpawner +%llX (want +1546CB0), ItemSpawnerRandom +%llX (want +15485A8), ChallengeItemSpawner +%llX (want +171EA00) "
            "[0 = not found, that kind is not taken; RTTI stages %d %d %d %d %d] | DropItem +%llX %s | SetWorldXform %s",
            rv(g_vtDropped), rv(g_vtFlare), rv(g_vtSpawner), rv(g_vtSpawnerR), rv(g_vtSpawnerC), t[0], t[1], t[2], t[3], t[4],
            static_cast<unsigned long long>(DLVR_LID_GD_RVA(kDropItemRva)),
            g_dropItem ? "ON (a let-go chest / placed item is dropped with physics)"
                       : "*** OFF - its code does not match the Steam bytes (EGL has no row): a let-go chest / placed item goes home ***",
            g_setXform ? "found" : "*** MISSING - a dropped item stays where the game's drop puts it ***");
    }
    // CARRIED OBJECTS (trace 20261126_1500): each class / call on its own; a missing one disables only itself (named). Not in `missing`.
    {
        int t[3] = { -1, -1, -1 };
        g_vtExploding = RttiFindVtable(gdb, ".?AVExplodingObject@@", 0, &t[0]);
        g_vtHeavy = RttiFindVtable(gdb, ".?AVHeavyObject@@", 0, &t[1]);
        g_vtHeavySpawner = RttiFindVtable(gdb, ".?AVHeavyObjectsSpawner@@", 0, &t[2]);
        void* ss = reinterpret_cast<void*>(gdb + DLVR_LID_GD_RVA(kSmartSetRva));
        g_smartSet = SmartSetHeadOk(static_cast<const unsigned char*>(ss)) ? reinterpret_cast<SmartSetFn>(ss) : nullptr;
        void* ig = reinterpret_cast<void*>(gdb + DLVR_LID_GD_RVA(kIgnoreRva));
        g_ignorePlayer = IgnoreHeadOk(static_cast<const unsigned char*>(ig)) ? reinterpret_cast<IgnoreFn>(ig) : nullptr;
        g_replInterval = reinterpret_cast<ReplIntervalFn>(GetProcAddress(en, "?ReplSetLocalReplicationInterval@IGSObject@@QEAAXMM@Z"));
        auto rv = [gdb](uintptr_t v) { return static_cast<unsigned long long>(v ? v - gdb : 0); };
        // [PROBE: CARRYHAND] gate=lid_by_hand  site=FLAT  log-only=no
        LOG("lidhand: [CARRYHAND] CARRIED OBJECTS (logged unconditionally) - ExplodingObject vt +%llX (want +1671E80), HeavyObject +%llX "
            "(want +16EA498), HeavyObjectsSpawner +%llX (want +16EBA28) [0 = not found, that class is not taken by hand; RTTI stages %d %d "
            "%d] | throw credit +%llX %s | player ignore +%llX %s | ReplSetLocalReplicationInterval %s | physics ON + velocity: vfuncs "
            "+%zu / +%zu / +%zu (no address) | velocity = the hand's 1:1, capped %.1f m/s, fastest >= 30 ms span in the last %.0f ms",
            rv(g_vtExploding), rv(g_vtHeavy), rv(g_vtHeavySpawner), t[0], t[1], t[2],
            static_cast<unsigned long long>(DLVR_LID_GD_RVA(kSmartSetRva)),
            g_smartSet ? "ON" : "*** OFF - its code does not match the Steam bytes: a thrown object is credited to nobody ***",
            static_cast<unsigned long long>(DLVR_LID_GD_RVA(kIgnoreRva)),
            g_ignorePlayer ? "ON" : "*** OFF - its code does not match: the object may hit the player as it leaves the hand ***",
            g_replInterval ? "found" : "*** MISSING - not called ***", kPhysNameVfuncOff, kSetLinVelVfuncOff, kGetLinVelVfuncOff,
            lh::kCarryMaxSpeed, kCarryVelWindowMs);
    }
    // THE INSTANCE CHECK's exports (trace 20261125_1610 v2) - written BEFORE g_ready = 1, read only after it. NOT in `missing`: without
    // them the check is OFF and remembered lids are trusted by address as before (named on the INSTANCE CHECK line below).
    g_objId = reinterpret_cast<ObjIdFn>(GetProcAddress(en, "?GetID@IGSObject@@QEBAHXZ"));
    g_objDying = reinterpret_cast<ObjFlagFn>(GetProcAddress(en, "?IsDestroying@IGSObject@@QEBA_NXZ"));
    g_objDead = reinterpret_cast<ObjFlagFn>(GetProcAddress(en, "?IsDestroyed@IGSObject@@QEBA_NXZ"));
    InterlockedExchange(&g_ready, 1);
    // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
    LOG("lidhand: [LIDHAND] READY - ComplexContainer vt +%llX (want +165A950 Steam), ContainerItem vt +%llX (want +165BA58), execute "
        "+%llX, %d lid rows + %d clip rows (%d frames) + %d grip-loot meshes (lid_open_frac %.2f) | open jump +%llX %s | carried-item "
        "category %s | drawn-hand placement %s",
        static_cast<unsigned long long>(g_vtContainer - gdb),
        static_cast<unsigned long long>(g_vtItem - gdb), static_cast<unsigned long long>(DLVR_LID_GD_RVA(kExecuteRva)), kRows,
        kClipRows, kLidClipFrameCount, static_cast<int>(sizeof(kGripLootMeshes) / sizeof(kGripLootMeshes[0])),
        LidOpenFrac, static_cast<unsigned long long>(DLVR_LID_GD_RVA(kPlayStageRva)),
        g_playStage ? "ON (the lid's pose goes straight to open at the open point, ours eases it there)"
                    : "OFF - its code does not match the Steam bytes (EGL has no row): the open plays the game's clip from its start",
        g_physBind ? "ON (2048 while in the hand - the headroom check no longer sees it)"
                   : "OFF - IControlObject::PhysicsBind not exported by this engine: the item keeps its own category (may crouch)",
        (g_setWorld && g_elemNum) ? "ON (the carried item follows the DRAWN hand's grip socket each tick, like the weapons)"
                                  : "OFF - SetElementWorldMatrix / GetElementsNumber not exported: the item rides the animation's holder");
    {
        const uintptr_t enb = reinterpret_cast<uintptr_t>(en);
        auto er = [enb](const void* p) { return static_cast<unsigned long long>(p ? reinterpret_cast<uintptr_t>(p) - enb : 0); };
        // [PROBE: LIDHAND] gate=lid_by_hand  site=FLAT  log-only=no
        LOG("lidhand: [LIDHAND] INSTANCE CHECK (logged unconditionally) - %s | GetID +%llX (want +252080), IsDestroying +%llX (want "
            "+252250), IsDestroyed +%llX (want +252280) [0 = not exported]",
            (g_objId && g_objDying && g_objDead)
                ? "ON - every remembered lid is checked against the object itself each tick (its id + destroying / destroyed flags); a "
                  "container rebuilt at the same address is identified afresh (trace 20261125_1610)"
                : "*** OFF - an export is missing: remembered lids are trusted by address, as before ***",
            er(reinterpret_cast<const void*>(g_objId)), er(reinterpret_cast<const void*>(g_objDying)),
            er(reinterpret_cast<const void*>(g_objDead)));
    }
    return true;
}

// HUD thread: is `obj` a lid container? -> its doors' rows + bones (the count returned; 0 = not a lid), cached per object pointer.
// Hinge rows first, then CLIP rows (trace 20261121_2145). *loot = a grip-loot mesh (a purse) with no row.
// gid / gidOk = obj's IGSObject id read NOW (it is live: the game's gather just returned it, this call) - trace 20261125_1610 v2.
int LidIdentity(void* obj, int* rows, int* bones, bool* loot, int gid, bool gidOk)
{
    *loot = false;
    for (IdCache& c : g_idc) {
        if (c.obj != obj) { continue; }
        if (lh::CacheHitStale(c.gidOk, c.gid, gidOk, gid)) {
            // THE TESTER's SLEEP (evidence/tester_20261124_223836_lidhand): a different container built into a freed block at the same
            // address - its rows / bones must come from ITS mesh, not from the object that lived here before.
            const LONG n = InterlockedIncrement(&g_nStaleId);
            if (n <= 16) {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] STALE IDENTITY - object %p: cached as %s (id %d), the live object here has id %d -> identified "
                    "afresh from its own mesh (%ld of 16 logged)", obj, c.n > 0 ? RowObject(c.row[0]) : c.loot ? "a grip-loot mesh"
                    : "not a lid", c.gid, gid, n);
            }
            c = IdCache{};
            break;
        }
        for (int d = 0; d < c.n; ++d) { rows[d] = c.row[d]; bones[d] = c.bone[d]; }
        *loot = c.loot;
        return c.n;
    }
    char mesh[96] = {};
    int r = -1, b = -1;   // the FIRST door found (the logs below name it, as before)
    int nd = 0, drow[kMaxDoors] = { -1, -1 }, dbone[kMaxDoors] = { -1, -1 };
    const bool haveName = SafeMeshName(obj, mesh, sizeof(mesh));
    // The BASE name (trace docs/traces/20261118_1849_lid_mesh_name.md; RUN LH1 matched nothing): GetMeshName returns the FILE name,
    // "furniture_chest_anm.msh" (OUR dynamic_objects_containers.scr), so the folder and the extension go before the compare.
    char base[96] = {};
    if (haveName) {
        const char* s = mesh;
        for (const char* q = mesh; *q; ++q) { if (*q == '/' || *q == '\\') { s = q + 1; } }
        strncpy_s(base, sizeof(base), s, _TRUNCATE);
        char* dot = strrchr(base, '.');
        if (dot) { *dot = 0; }
    }
    // EVERY row of this mesh (trace 20261121_1549: a 2-door object has one row per door); a row whose bone the object lacks is skipped
    int rowByName = -1;
    for (int i = 0; haveName && i < kRows + kClipRows; ++i) {
        if (ClipRowOff(i)) { continue; }   // switched off (trace 20261121_2235) - matches nothing
        const char* rmesh = IsClipRow(i) ? ClipRow(i).mesh : kLidHinges[i].mesh;
        const char* rbone = IsClipRow(i) ? ClipRow(i).bone : kLidHinges[i].bone;
        if (_stricmp(base, rmesh) == 0) {
            int id = -1;
            if (SafeElemId(obj, rbone, &id) && id >= 0) {
                if (r < 0) { r = i; b = id; }
                if (nd < kMaxDoors) { drow[nd] = i; dbone[nd] = id; ++nd; }
            } else if (rowByName < 0) {
                rowByName = i;
            }
        }
    }
    bool isLoot = false;
    for (const char* m : kGripLootMeshes) { isLoot = isLoot || (r < 0 && rowByName < 0 && haveName && _stricmp(base, m) == 0); }
    static LONG s_firstCc = 0;
    if (InterlockedIncrement(&s_firstCc) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST ComplexContainer gathered (logged unconditionally): object %p mesh '%s' (base '%s') -> %s", obj,
            haveName ? mesh : "(name not read)", base, r >= 0 ? RowObject(r) : rowByName >= 0 ? "a lid row, but its lid bone is missing"
            : isLoot ? "a grip-loot mesh" : "not a lid row");
    }
    if (isLoot) {
        static LONG s_lootSaid = 0;
        if (InterlockedIncrement(&s_lootSaid) <= 4) {
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] FIRST grip-loot container seen: object %p mesh '%s' - a grip press with the hand on it calls the "
                "game's open (the loot) (%ld of 4 logged)", obj, mesh, s_lootSaid);
        }
    } else if (r < 0 && rowByName >= 0) {
        static LONG s_noBone[kRows + kClipRows] = {};
        if (InterlockedIncrement(&s_noBone[rowByName]) == 1) {
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] ROW MATCHED BUT NO LID BONE - object %p mesh '%s' = row %s, bone '%s' not found in it - not a lid",
                obj, mesh, RowObject(rowByName), IsClipRow(rowByName) ? ClipRow(rowByName).bone : kLidHinges[rowByName].bone);
        }
    } else if (r < 0) {
        static char s_said[16][64] = {};
        static int  s_nSaid = 0;
        bool known = false;
        for (int k = 0; k < s_nSaid && !known; ++k) { known = _stricmp(s_said[k], base) == 0; }
        if (!known && s_nSaid < 16) {
            strncpy_s(s_said[s_nSaid++], sizeof(s_said[0]), base, _TRUNCATE);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] NOT A LID ROW - a ComplexContainer with mesh '%s' (base '%s') matches no row (%d of 16 names logged)",
                haveName ? mesh : "(name not read)", base, s_nSaid);
        }
    }
    g_idc[g_idcNext] = { obj, nd, { drow[0], drow[1] }, { dbone[0], dbone[1] }, isLoot, gid, gidOk };
    g_idcNext = (g_idcNext + 1) % static_cast<int>(sizeof(g_idc) / sizeof(g_idc[0]));
    for (int d = 0; d < nd; ++d) {
        static LONG s_saidRow[kRows + kClipRows] = {};
        const int rr = drow[d];
        if (InterlockedIncrement(&s_saidRow[rr]) == 1) {
            if (IsClipRow(rr)) {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] FIRST %s seen: object %p mesh %s, bone %s = element %d (clip, %d frames, open pose at frame %d)"
                    " | door %d of %d", RowObject(rr), obj, mesh, ClipRow(rr).bone, dbone[d], ClipRow(rr).nFrames, ClipRow(rr).openFrame,
                    d + 1, nd);
            } else {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] FIRST %s seen: object %p mesh %s, lid bone %s = element %d (hinge axis %.3f %.3f %.3f, open %.2f deg)"
                    " | door %d of %d", kLidHinges[rr].object, obj, mesh, kLidHinges[rr].bone, dbone[d], kLidHinges[rr].axis[0],
                    kLidHinges[rr].axis[1], kLidHinges[rr].axis[2], kLidHinges[rr].openDeg, d + 1, nd);
            }
        }
    }
    for (int d = 0; d < nd; ++d) { rows[d] = drow[d]; bones[d] = dbone[d]; }
    *loot = isLoot;
    return nd;
}

bool HandWorldPos(int side, float out[3])
{
    float rot[9];
    long long qpc = 0;
    return RigGripWorldRead(side, out, rot, &qpc);
}

// The right hand at the right shoulder: bownock.cpp's HandInShoulder (the holster's zone, its 1.2x exit). The left hand: the same
// zone mirrored to the left shoulder (my call - the left holster's own flag belongs to its poll).
bool InShoulder(int side)
{
    static bool s_in[2] = {};
    XrPosef head{}, grip{};
    if (!GetHeadPose(head) || !GetControllerGripPose(side == 0 ? kRightHand : kLeftHand, grip)) { s_in[side] = false; return false; }
    const float hp[3] = { head.position.x, head.position.y, head.position.z };
    const float hq[4] = { head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w };
    const float gp[3] = { grip.position.x, grip.position.y, grip.position.z };
    float lv[3];
    // mirrorX for the left hand (final review I1, trace 20261120_1510): the direction-free frame is aligned to the centre tested below.
    HolsterZoneLocal(kHolsterShoulder, kHolsterCallerBow, hp, hq, gp, lv, side == 1);
    const float cx = side == 0 ? HolsterCenterX : -HolsterCenterX;
    const float dx = lv[0] - cx, dy = lv[1] - HolsterCenterY, dz = lv[2] - HolsterCenterZ;
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float exitR = HolsterRadius * holster::kExitScale;
    if (s_in[side]) { if (d2 > exitR * exitR) { s_in[side] = false; } }
    else if (d2 < HolsterRadius * HolsterRadius) { s_in[side] = true; }
    return s_in[side];
}

// keyed by (object, row): the two doors of one object are two lids (trace 20261121_1549)
int FindLidSlot(void* obj, int row)
{
    for (int i = 0; i < kMaxLids; ++i) { if (g_lids[i].obj == obj && g_lids[i].row == row) { return i; } }
    return -1;
}
int NewLidSlot(void* obj, int row, int bone)
{
    int i = FindLidSlot(obj, row);
    if (i >= 0) { return i; }
    for (int k = 0; k < kMaxLids; ++k) {
        if (!g_lids[k].obj || !g_lids[k].writing) {
            if (g_hand[0].lid == k || g_hand[1].lid == k) { continue; }
            g_lids[k] = LidState{};
            g_lids[k].obj = obj; g_lids[k].row = row; g_lids[k].bone = bone;
            // its id (trace 20261125_1610 v2) - obj comes from this tick's snapshot of the game's gather, so it is the live object;
            // a dying one is not given an id (the next tick's check drops it)
            int id = -1;
            bool dying = false;
            if (ObjIdentity(obj, &id, &dying) && !dying) { g_lids[k].gid = id; g_lids[k].gidOk = true; }
            return k;
        }
    }
    return -1;
}

// The lid's current angle from its own local (our write or the game's clip), about the row's axis.
bool LidNowDeg(const LidState& L, float* deg)
{
    float cur[12];
    if (!SafeLocal(L.obj, L.bone, cur)) { return false; }
    if (IsClipRow(L.row)) {   // a clip row: the frame position nearest the bone's current local (trace 20261121_2145)
        *deg = lh::ClipTFromLocal(ClipFramesOf(L.row), ClipRow(L.row).nFrames, cur);
        return true;
    }
    *deg = lh::LidAngleFromLocal(kLidHinges[L.row].rest, cur, kLidHinges[L.row].axis);
    return true;
}

// The hinge in world: pivot = parent world x the rest translation, axis = parent world rotation x the row's axis. A clip row: its rest
// pivot and first-20-deg axis (the GRAB log and the 2-door leaf tie-break only - never the write).
bool HingeWorld(const LidState& L, float pivotW[3], float axisW[3])
{
    float pw[12];
    if (!SafeParentWorld(L.obj, L.bone, pw)) { return false; }
    MulPoint(pw, RowPivot(L.row), pivotW);
    MulRot(pw, RowAxis(L.row), axisW);
    return true;
}

// The row's local at `deg` (a hinge: R(axis, deg) . rest; a clip row: the clip frame at t = deg).
void RowLocal(int row, float deg, float out[12])
{
    if (IsClipRow(row)) { lh::ClipLocalAt(ClipFramesOf(row), ClipRow(row).nFrames, deg, out); return; }
    lh::LidLocal(kLidHinges[row].rest, kLidHinges[row].axis, deg, out);
}

void WriteLid(LidState& L, float deg)
{
    float now[12], want[12];
    // read back first: anything that changed the local since our last write is counted (§13.3 ruling)
    if (SafeLocal(L.obj, L.bone, now) && L.writing) {
        float last[12];
        RowLocal(L.row, L.deg, last);
        float worst = 0.0f;
        for (int k = 0; k < 12; ++k) { worst = std::fmax(worst, std::fabs(now[k] - last[k])); }
        if (worst > 1.0e-4f) {
            InterlockedIncrement(&g_nOverwrite);
            if (!L.overwriteSaid) {
                L.overwriteSaid = true;
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] OVERWRITE - %s %p: the lid's local differs from our last write by %.4f (something else writes "
                    "it between ticks; logged once per lid, counted)", RowObject(L.row), L.obj, worst);
            }
        }
    }
    RowLocal(L.row, deg, want);
    if (SafeSetLocal(L.obj, L.bone, want)) { L.deg = deg; L.writing = deg > 0.05f || L.hold; }
}

// ---- THE FREE LIFT (trace 20261122_1830) ----
// At the grab: the cover's world now, in the drawn holder's frame (freeRel), and the rim centre's world at the row's rest (frame 0).
// False = no drawn hand pose / the local unread (the grab is refused and named by the caller).
bool FreeGrab(int side, LidState& L, const float pw[12])
{
    HandState& H = g_hand[side];
    const int ri = FreeRim(L.row);
    float D[12], cur[12];
    if (ri < 0 || !RigDrawnHolderRead(side, D) || !SafeLocal(L.obj, L.bone, cur)) { return false; }
    float Wb[12], Di[12], L0[12], W0[12];
    lh::Compose34(pw, cur, Wb);
    lh::InvRigid34(D, Di);
    lh::Compose34(Di, Wb, H.freeRel);
    RowLocal(L.row, 0.0f, L0);
    lh::Compose34(pw, L0, W0);
    MulPoint(W0, kLidRims[ri].centre, H.freeRestC);
    H.free = true;
    H.freeSaidNoHolder = false;
    return true;
}

// The let-go: CLOSED and lifted past lh::kFreeLiftOpenM -> the game's own execute + open jump, our write of the clip's LAST frame (the
// game's leaning end pose) once, then the game's own pose; CLOSED and not lifted -> frame 0 (rest) written once; already OPENED -> its
// last frame once. alive = the container still is one (else nothing is touched).
void FreeRelease(int side, int lidIdx, const char* why, bool alive)
{
    HandState& H = g_hand[side];
    LidState& L = g_lids[lidIdx];
    const char* sn = side == 0 ? "right" : "left";   // (kSideName is defined further down)
    H.lid = -1;
    H.free = false;
    if (!alive) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FREE LIFT RELEASE %s hand: %s - the container is gone, nothing written", sn, RowObject(L.row));
        L = LidState{};
        return;
    }
    const int ri = FreeRim(L.row);
    float cur[12], pw[12], Wb[12], nowC[3] = {};
    const bool read = ri >= 0 && SafeLocal(L.obj, L.bone, cur) && SafeParentWorld(L.obj, L.bone, pw);
    if (read) {
        lh::Compose34(pw, cur, Wb);
        MulPoint(Wb, kLidRims[ri].centre, nowC);
    }
    const float dx = nowC[0] - H.freeRestC[0], dy = nowC[1] - H.freeRestC[1], dz = nowC[2] - H.freeRestC[2];
    const float moved = read ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.0f;
    const int st0 = State(L.obj);
    L.writing = false;   // our free pose is not a clip frame: no OVERWRITE comparison against it in WriteLid
    L.easing = false;
    if (st0 == kStateClosed && read && lh::FreeLiftOpens(H.freeRestC, nowC)) {
        const bool ok = SafeExecute(g_player, L.obj);
        const int st = State(L.obj);
        InterlockedIncrement(&g_nOpen);
        int key = -1;
        const bool jumped = ok && st == kStateOpened && SafeOpenJump(L.obj, &key);
        WriteLid(L, RowEnd(L.row));
        L.writing = false;
        L.gameClip = true;
        L.checkHold = true;
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FREE LIFT OPEN %s hand: %s %p (%s) - lifted %.3f m (> %.2f): the game's own execute %s -> state %d (want 1) "
            "| open jump (stage key %d): %s | the clip's last frame (%.0f) written once, then the game's pose", sn,
            RowObject(L.row), L.obj, why, moved, lh::kFreeLiftOpenM, ok ? "called" : "*** FAULTED ***", st, key,
            jumped ? "JUMPED to the end pose" : "*** not jumped - the game's clip plays from its start ***", RowEnd(L.row));
    } else if (st0 == kStateOpened) {
        WriteLid(L, RowEnd(L.row));
        L.writing = false;
        L.gameClip = true;
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FREE LIFT RELEASE %s hand: %s %p (%s) - already open, back to its leaning pose (moved %.3f m)",
            sn, RowObject(L.row), L.obj, why, moved);
    } else {
        WriteLid(L, 0.0f);
        L.writing = false;
        L.gameClip = false;
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FREE LIFT BACK ON %s hand: %s %p (%s) - moved %.3f m (open past %.2f; -1 = unread), state %d: the rest "
            "pose written, still closed", sn, RowObject(L.row), L.obj, why, moved, lh::kFreeLiftOpenM, st0);
    }
}

void Publish(void* const* lids, const int* rows, const int* bones, float (*lboxes)[6], int nl, void* const* items,
             float (*iboxes)[6], const int* ikinds, int ni, void* const* loots, float (*lootBoxes)[6], int nloot)
{
    InterlockedIncrement(&g_pubSeq);   // odd: writing
    for (int i = 0; i < nloot; ++i) {
        g_pubLoot[i] = loots[i];
        memcpy(g_pubLootBox[i], lootBoxes[i], sizeof(g_pubLootBox[i]));
    }
    g_pubLootN = nloot;
    for (int i = 0; i < nl; ++i) {
        g_pubLid[i] = lids[i]; g_pubLidRow[i] = rows[i]; g_pubLidBone[i] = bones[i];
        memcpy(g_pubLidBox[i], lboxes[i], sizeof(g_pubLidBox[i]));
    }
    for (int i = 0; i < ni; ++i) {
        g_pubItem[i] = items[i]; g_pubItemKind[i] = ikinds[i];
        memcpy(g_pubItemBox[i], iboxes[i], sizeof(g_pubItemBox[i]));
    }
    g_pubLidN = nl;
    g_pubItemN = ni;
    InterlockedIncrement(&g_pubSeq);   // even: settled
}

struct Snapshot {
    int nl = 0, ni = 0;
    void* lid[kMaxPub]; int row[kMaxPub]; int bone[kMaxPub]; float lbox[kMaxPub][6];
    void* item[kMaxPub]; float ibox[kMaxPub][6]; int ikind[kMaxPub];
    int nloot = 0; void* loot[kMaxPub]; float lootBox[kMaxPub][6];   // grip-loot containers (trace 20261121_2145)
};
bool ReadSnapshot(Snapshot& s)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG s0 = g_pubSeq;
        if (s0 & 1) { continue; }
        s.nl = g_pubLidN; s.ni = g_pubItemN;
        for (int i = 0; i < s.nl; ++i) {
            s.lid[i] = g_pubLid[i]; s.row[i] = g_pubLidRow[i]; s.bone[i] = g_pubLidBone[i];
            memcpy(s.lbox[i], g_pubLidBox[i], sizeof(s.lbox[i]));
        }
        for (int i = 0; i < s.ni; ++i) {
            s.item[i] = g_pubItem[i]; s.ikind[i] = g_pubItemKind[i];
            memcpy(s.ibox[i], g_pubItemBox[i], sizeof(s.ibox[i]));
        }
        s.nloot = g_pubLootN;
        for (int i = 0; i < s.nloot; ++i) {
            s.loot[i] = g_pubLoot[i];
            memcpy(s.lootBox[i], g_pubLootBox[i], sizeof(s.lootBox[i]));
        }
        if (g_pubSeq == s0) { return true; }
    }
    return false;
}

const char* kSideName[2] = { "right", "left" };
const char* KindName(lh::ItemKind k)
{
    return k == lh::ItemKind::Chest ? "chest" : k == lh::ItemKind::Spawner ? "placed"
         : k == lh::ItemKind::Carriable ? "carriable" : "dropped";
}

// A chest / placed item let go away from the shoulder, ALREADY HOME (spec 5.4, trace 20261120_2210): the game's take, the entry it added
// or raised (InvDelta over two INVKEEP-path snapshots), the game's drop of exactly that count, the new DroppedItem moved to `Wd` (the
// game's own drop-at-a-matrix idiom, Mac ActorDI::DropInventoryItem) and its flight speed zeroed so it falls. Every outcome is named in
// `out`. An event - never per frame. Game thread.
void TakeAndDrop(void* item, lh::ItemKind kind, const float* Wd, int action, char* out, size_t n)
{
    static InvSnap s_before, s_after;   // ~8 KB each - static (game thread only), not on the stack
    // The carried item's ItemID, read BEFORE the take (trace 20261121_1627): the entry to drop is the one of THIS item (RUN CL1b: the
    // first changed entry was the equipped grappling hook's).
    const int want = SafeItemId(static_cast<char*>(item) + (kind == lh::ItemKind::Spawner ? kSpawnerDescObjOff : kChestDescObjOff));
    const bool snapB = InvSnapshot(s_before);
    const bool took = SafeExecute(g_player, item);
    const bool snapA = InvSnapshot(s_after);
    if (!took) { sprintf_s(out, n, " the game's take (+B9B930, action %d) *** FAULTED *** - nothing dropped", action); return; }
    if (!snapB || !snapA) {
        sprintf_s(out, n, " the game's take (action %d) called; *** DROP SKIPPED - the inventory was not read (%s) - the item is in the "
                  "inventory ***", action, snapB ? "after" : "before");
        return;
    }
    if (want == lh::kNoId) {
        sprintf_s(out, n, " the game's take (action %d) called; *** DROP SKIPPED - the item's identity (ItemID, desc object +%zu) did not "
                  "read *** - taken as a normal pickup", action, kind == lh::ItemKind::Spawner ? kSpawnerDescObjOff : kChestDescObjOff);
        return;
    }
    int count = 0;
    const int idx = lh::InvDeltaMatch(s_before.p, s_before.count, s_before.n, s_after.p, s_after.count, s_after.id, s_after.n, want, &count);
    if (idx < 0) {
        sprintf_s(out, n, " the game's take (action %d) called; DROP SKIPPED - no entry of ItemID %d was added or raised (%d -> %d entries: "
                  "ammo / cash / a collectable is taken with no entry) - taken as a normal pickup", action, want, s_before.n, s_after.n);
        return;
    }
    void* entry = s_after.p[idx];
    if (!g_dropItem) {
        sprintf_s(out, n, " the game's take (action %d) called; entry %p +%d; *** DROP OFF - DropItem's code does not match (see WORLD "
                  "ITEMS at READY) - the item is in the inventory ***", action, entry, count);
        return;
    }
    void* dropped = SafeDropItem(g_player, entry, count);
    if (!dropped) {
        sprintf_s(out, n, " the game's take (action %d) called; entry %p +%d; *** DROP FAILED - DropItem returned null / faulted - the item "
                  "is in the inventory ***", action, entry, count);
        return;
    }
    const bool moved = Wd && SafeSetXform(dropped, Wd);
    float back[12] = {};
    const bool readBack = moved && SafeGetXform(dropped, back);
    const float dx = readBack ? back[3] - Wd[3] : 0.0f, dy = readBack ? back[7] - Wd[7] : 0.0f, dz = readBack ? back[11] - Wd[11] : 0.0f;
    float was = -1.0f;
    const bool zeroed = SafeZeroDropSpeed(dropped, &was);
    sprintf_s(out, n, " the game's take (action %d) called; entry %p of ItemID %d (the carried item's %d) +%d (%d -> %d entries); the "
              "game's drop (+B9EEF0) -> new dropped item %p; %s; read back %s %.4f m; flight speed [+1412] %.2f -> %s", action, entry,
              s_after.id[idx], want, count, s_before.n, s_after.n, dropped,
              moved ? "moved to the drawn item matrix" : Wd ? "*** SetWorldXform MISSING / FAULTED - left where the game dropped it ***"
                                                           : "*** no drawn hand pose - left where the game dropped it ***",
              readBack ? "off by" : "(not read)", readBack ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f, was,
              zeroed ? "0 (it falls)" : "*** NOT zeroed (faulted) ***");
}

// The grip opened (or the item was lost) while carrying: stow or give back (spec §4.5).
// explodedLive (final review finding 3): a held carriable that exploded but is still live - let go where it is, physics on, nothing else.
void ReleaseItem(int side, bool lost, bool explodedLive = false)
{
    HandState& H = g_hand[side];
    if (!H.item) { return; }
    void* item = H.item;
    if (lost) {
        HandCarryForget(H.carry, "lidhand: the container left the opened state");
        InterlockedIncrement(&g_nForgot);
        // The item may already be destroyed (§13.5): no call on it - its category is left as it is (trace 20261120_1634).
        const bool owedCat = H.catSet;
        H.catSet = false;
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FORGOT %s hand's %s item %p - %s%s", kSideName[side], KindName(H.kind), item,
            H.kind == lh::ItemKind::Chest ? "its container left state 1 (the game removes its placed items, §13.5)"
                                          : "the engine no longer returns it by its id, or it left the hand's holder (destroyed / "
                                            "taken by the game) - no call on it (INV-663)",
            owedCat ? " | collision category left at 2048 (the item is gone or the game's - not touched)" : "");
    } else {
        // Where the player SEES the item now (the drawn socket x its grip row - the root elements are drawn there): a dropped item is
        // released there (trace 20261120_2210). Read BEFORE anything is restored.
        float D0[12], Wd[12];
        const bool haveWd = RigDrawnHolderRead(side, D0);
        if (haveWd) { Mul34(D0, H.rel, Wd); }
        int action = -1;
        // only where the release reads it (review minor 5): OnItemRelease returns Drop for a Carriable without it
        const bool haveAction = lh::ReleaseNeedsAction(H.kind) && SafeAction(item, g_player, &action);
        const bool inShoulder = InShoulder(side);
        const lh::ItemRelease what = lh::OnItemRelease(inShoulder, haveAction ? action : -1, H.kind);
        // The item's own category back BEFORE it returns to its hook and before the game's take (trace 20261120_1634).
        char catText[192] = "";   // + the root-element restore text (trace 20261120_2113) - 96 could overflow strcat_s
        if (H.catSet) {
            const bool restored = SafeSetCategory(item, H.savedCat);
            sprintf_s(catText, " | collision category %s %d", restored ? "restored" : "*** RESTORE FAULTED *** (left 2048, wanted",
                      H.savedCat);
            if (!restored) { strcat_s(catText, ")"); }
            H.catSet = false;
        }
        // The item's root elements back to their locals latched at the take, BEFORE the give-back (trace 20261120_2113; RUN LP1: the
        // per-tick world writes leave the engine's rebuilt locals hand-relative, so the item floated where it was let go).
        int nRestored = 0;
        for (int k = 0; k < H.placeN; ++k) {
            if (SafeSetLocal(item, H.placeElem[k], H.placeLocal[k])) { ++nRestored; }
        }
        char localText[96] = "";
        if (H.placeN > 0) {
            sprintf_s(localText, " | %d of %d root element local(s) restored%s", nRestored, H.placeN,
                      nRestored == H.placeN ? "" : " *** a local write FAULTED ***");
        }
        strcat_s(catText, localText);
        // HOME FIRST (trace 20261120_2210): a chest item back on its hook (LP2's give-back); a placed item back to its world matrix (its
        // "taken" record is keyed by its world position - INV-758); a dropped item detached where it is shown (its OnDetached restarts
        // its physics; stowed, it is detached where it is and the take destroys it).
        bool back = false, detached = false;
        if (H.kind == lh::ItemKind::Chest) {
            back = HandCarryGiveBack(H.carry, "lidhand: release");
        } else if (H.kind == lh::ItemKind::Spawner) {
            back = HandCarryReleaseWorld(H.carry, H.carry.homeWorld, "lidhand: home");
        } else {
            back = HandCarryReleaseWorld(H.carry, (what == lh::ItemRelease::Drop && haveWd) ? Wd : nullptr, "lidhand: drop", &detached);
        }
        // FREE of the hand once the detach returned, even when the placement write after it faulted (review minor 8): a carriable then
        // still gets its physics back (it would otherwise hang in the air), a DroppedItem its drop line.
        const bool freed = lh::ReleaseFreed(back, detached);
        const char* detachText = back ? "ok" : freed ? "ok, but its placement write FAULTED (left where the holder had it)"
                                                     : "*** FAULTED twice - the object stays on the holder, the carry is forgotten ***";
        if (H.kind == lh::ItemKind::Carriable && explodedLive) {
            // EXPLODED IN THE HAND (final review finding 3): detached above where it is shown; physics back on so it is not left floating;
            // no credit, no player ignore, no velocity (it was not thrown).
            const bool physOn = freed && SafePhysicsOn(item);
            // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [CARRYHAND] EXPLODED IN HAND %s hand's carriable %p (its +1008 set, still live by id) - let go: detach %s | "
                "physics ON %s | no credit / velocity%s", kSideName[side], item, detachText, physOn ? "ok" : "*** NOT ON ***", catText);
        } else if (H.kind == lh::ItemKind::Carriable) {
            // THE THROW (trace 20261126_1500; Jay: "a physical throw by arm movement & releasing grip"): already detached at the drawn item
            // matrix above. Then: physics ON (ours - these classes' detach restarts nothing), the game's own throw writes (credit by exact
            // class, the player's 1 s ignore), the hand's velocity (1:1, capped) through ObjectDI +432, read back through +440.
            throwable::Vec3 hv{};
            double atMs = 0.0;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const bool haveV = H.hist.Fastest(QpcMs(now.QuadPart), kCarryVelWindowMs, hv, atMs);
            const lh::V3 v = haveV ? lh::CarryLaunchVelocity(lh::V3{ hv.x, hv.y, hv.z }, lh::kCarryMaxSpeed) : lh::V3{};
            const float hs = std::sqrt(hv.x * hv.x + hv.y * hv.y + hv.z * hv.z);
            const float vs = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            const bool physOn = freed && SafePhysicsOn(item);
            int credit = 0;
            const lh::CreditResult cred = freed ? SafeCredit(item, g_player, &credit) : lh::CreditResult::NotCalled;
            const bool ignOk = freed && SafeIgnorePlayer(g_player, item);
            const float vw[3] = { v.x, v.y, v.z };
            const bool velOk = freed && physOn && SafeSetLinVel(item, vw);
            float rb[3] = { 0.0f, 0.0f, 0.0f };
            const bool rbOk = velOk && SafeGetLinVel(item, rb);
            InterlockedIncrement(&g_nThrow);
            InterlockedIncrement(&g_nDrop);
            // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [CARRYHAND] THROW %s hand's carriable %p: detach %s | hand %s %.2f m/s (age %.0f ms) -> velocity (%.2f %.2f %.2f) "
                "%.2f m/s%s | physics ON %s | credit %s | player ignore 1 s %s | velocity %s, read back (%.2f %.2f %.2f) %s%s",
                kSideName[side], item, back ? (haveWd ? "at the drawn item matrix" : "*** at the holder (no drawn pose) ***") : detachText,
                haveV ? "speed" : "*** NO VELOCITY (fewer than 2 samples >= 30 ms apart) - dropped ***", hs,
                haveV ? (QpcMs(now.QuadPart) - atMs) : -1.0, v.x, v.y, v.z, vs, vs + 1e-4f < hs ? " (CAPPED)" : "",
                physOn ? "ok" : "*** NOT ON ***",
                lh::CreditText(cred, credit),
                ignOk ? "ok" : "*** NOT set ***", velOk ? "set (+432)" : "*** NOT SET ***", rb[0], rb[1], rb[2],
                rbOk ? "(+440)" : "*** NOT READ ***", catText);
        } else if (what == lh::ItemRelease::Stow && back) {
            const bool ok = SafeExecute(g_player, item);
            InterlockedIncrement(&g_nStow);
            // THE STOW BUZZ (trace 20261124_1910, Jay: "Need haptic response for item stowing"): the take's pulse on the stowing hand,
            // only once the game's take returned ok. rig_two_hand_haptic scales it with the other lidhand pulses (no new key).
            const char* pulseText = "no pulse (the take faulted)";
            if (ok) {
                if (!(RigTwoHandHaptic > 0.0f)) {
                    pulseText = "no pulse (rig_two_hand_haptic 0)";
                } else {
                    pulseText = PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40)
                        ? "pulse queued" : "pulse REFUSED (PulseController false)";
                }
            }
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] STOW %s hand's %s item %p over the shoulder - the game's own take (+B9B930, action %d) %s | %s%s",
                kSideName[side], KindName(H.kind), item, action, ok ? "called" : "*** FAULTED ***", pulseText, catText);
        } else if (what == lh::ItemRelease::Drop && freed && H.kind == lh::ItemKind::Dropped) {
            float was = -1.0f;
            const bool zeroed = SafeZeroDropSpeed(item, &was);
            InterlockedIncrement(&g_nDrop);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] DROP dropped %s hand's item %p - detached at the hand (%s); its own OnDetached restarts its "
                "physics | flight speed [+1412] %.2f -> %s%s", kSideName[side], item,
                !back ? detachText : haveWd ? "at the drawn item matrix" : "*** no drawn hand pose - left at the holder ***", was,
                zeroed ? "0 (it falls)" : "*** NOT zeroed (faulted) ***", catText);
        } else if (what == lh::ItemRelease::Drop && back) {
            char dropText[512];   // 320 -> 512 (trace 20261121_1627): the success line names two ItemIDs now (~340 chars at most)
            TakeAndDrop(item, H.kind, haveWd ? Wd : nullptr, action, dropText, sizeof(dropText));
            InterlockedIncrement(&g_nDrop);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] DROP %s %s hand's item %p (put home first) -%s%s", KindName(H.kind), kSideName[side], item, dropText,
                catText);
        } else {
            InterlockedIncrement(&g_nGiveBack);
            // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
            LOG("lidhand: [LIDHAND] GIVE BACK %s hand's %s item %p to its place (%s; action %d%s)%s%s", kSideName[side], KindName(H.kind),
                item, inShoulder ? "in the shoulder zone" : "away from the shoulder", action,
                action == 23 ? " = the inventory refuses it" : action < 0 ? " = not read" : "", back ? "" : " *** give-back failed ***",
                catText);
        }
    }
    H.kind = lh::ItemKind::Chest;
    H.physOff = false;
    H.item = nullptr;
    H.itemOwner = nullptr;
    H.placeN = 0;   // the drawn-hand placement stops with the carry; the engine composes the item from its hook again
    // the grip's fingers stop BEFORE g_carried clears (trace 20261120_2057): a finger pass that reads -1 writes nothing for this side
    InterlockedExchange(&g_poseRow[side], -1);
    H.poseRow = -1;
    g_carried[side] = nullptr;
}

}  // namespace

bool LidHandOnNearby(void* player, void* const* gameData, unsigned gameCount, void* const* gazeData, unsigned gazeCount,
                     void** promptOut)
{
    if (promptOut) { *promptOut = nullptr; }
    if (!LidByHand || !Ready() || !player) { return false; }
    g_player = player;
    // THE STAMP, HUD side (trace 20261125_1610 v2): another level, or the level's next id going down (a renumber) -> every cached identity
    // is made again (one GetMeshName + row match each, as at session start). `player` is the live PlayerDI the game passed this call.
    {
        static uintptr_t s_level = 0;
        static int s_next = 0;
        uintptr_t lv = 0;
        int nx = 0;
        if (g_objId && LevelStamp(player, &lv, &nx)) {
            const lh::StampVerdict v = lh::StampChange(s_level, s_next, lv, nx);
            if (v != lh::StampVerdict::None) {
                for (IdCache& c : g_idc) { c = IdCache{}; }
                g_idcNext = 0;
                static LONG s_said = 0;
                if (InterlockedIncrement(&s_said) <= 8) {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] ID STAMP (HUD) - %s (level %p -> %p, next id %d -> %d): the identity cache is cleared (%ld of 8 "
                        "logged)", v == lh::StampVerdict::Flush ? "another level" : "the level's ids were renumbered (next id went down)",
                        reinterpret_cast<void*>(s_level), reinterpret_cast<void*>(lv), s_next, nx, s_said);
                }
            }
            s_level = lv;
            s_next = nx;
        }
    }
    void* lids[kMaxPub]; int rows[kMaxPub]; int bones[kMaxPub]; float lbox[kMaxPub][6];
    void* items[kMaxPub]; float ibox[kMaxPub][6]; int ikind[kMaxPub];
    void* loots[kMaxPub]; float lootBox[kMaxPub][6];
    int nl = 0, ni = 0, nw = 0, nloot = 0;
    const unsigned total = gameCount + gazeCount;
    for (unsigned i = 0; i < total; ++i) {
        uintptr_t o = 0;
        if (i < gameCount) {
            if (!ReadPtr(reinterpret_cast<uintptr_t>(gameData) + 8 * i, &o) || !o) { continue; }
        } else {
            o = reinterpret_cast<uintptr_t>(gazeData[i - gameCount]);
            if (!o) { continue; }
        }
        void* obj = reinterpret_cast<void*>(o);
        bool dup = false;
        for (int k = 0; k < nl && !dup; ++k) { dup = lids[k] == obj; }
        for (int k = 0; k < ni && !dup; ++k) { dup = items[k] == obj; }
        for (int k = 0; k < nloot && !dup; ++k) { dup = loots[k] == obj; }
        if (dup) { continue; }
        if (IsVt(obj, g_vtContainer) && nl < kMaxPub) {
            // one published entry PER DOOR (trace 20261121_1549): same object + box, its own row / bone
            int dr[kMaxDoors], db[kMaxDoors];
            bool isLoot = false;
            int gid = -1;
            bool dyingNow = false;
            const bool gidOk = ObjIdentity(obj, &gid, &dyingNow);   // obj is live: the game's gather returned it this call
            const int nd = LidIdentity(obj, dr, db, &isLoot, gid, gidOk);
            float box[6];
            if (nd > 0 && SafeBox(obj, box)) {
                for (int d = 0; d < nd && nl < kMaxPub; ++d) {
                    lids[nl] = obj; rows[nl] = dr[d]; bones[nl] = db[d];
                    memcpy(lbox[nl], box, sizeof(box));
                    ++nl;
                }
            } else if (isLoot && nloot < kMaxPub && SafeBox(obj, lootBox[nloot])) {
                loots[nloot++] = obj;   // a purse (trace 20261121_2145): grip press = the game's open = the loot
            }
        } else if (ni < kMaxPub) {
            // a chest item, or a world item (trace 20261120_2210, INV-758): a DroppedItem / Flare, or a placed spawner (it IS the item)
            const int kind = IsVt(obj, g_vtItem) ? static_cast<int>(lh::ItemKind::Chest)
                           : (IsVt(obj, g_vtDropped) || IsVt(obj, g_vtFlare)) ? static_cast<int>(lh::ItemKind::Dropped)
                           : (IsVt(obj, g_vtSpawner) || IsVt(obj, g_vtSpawnerR) || IsVt(obj, g_vtSpawnerC)) ? static_cast<int>(lh::ItemKind::Spawner)
                           : CarriableNow(obj, player) ? static_cast<int>(lh::ItemKind::Carriable)
                           : -1;
            if (kind >= 0 && SafeBox(obj, ibox[ni])) {
                items[ni] = obj; ikind[ni] = kind; ++ni;
                if (kind != static_cast<int>(lh::ItemKind::Chest)) { ++nw; }
            } else if (kind < 0) {
                // RUN CO1 diagnostic: the first 12 gathered triggers NO kind claims, by vtable + mesh - a canister of a class we do not
                // test (e.g. a derived GasTankObject) shows here. COST: one interlocked increment per unclaimed candidate after 12.
                static LONG s_u = 0;
                const LONG u = InterlockedIncrement(&s_u);
                if (u <= 12) {
                    uintptr_t vt = 0;
                    ReadPtr(o, &vt);
                    char mesh[96] = "(name not read)";
                    if (g_getMeshName) { SafeMeshName(obj, mesh, sizeof(mesh)); }
                    const HMODULE gdm = GetModuleHandleA("gamedll_x64_rwdi.dll");
                    const uintptr_t gb = reinterpret_cast<uintptr_t>(gdm);
                    // [PROBE: CARRYHAND] gate=lid_by_hand  site=FLAT  log-only=yes
                    LOG("lidhand: [CARRYHAND] UNCLAIMED %ld of 12: trigger %p vt +%llX mesh '%s' (not a lid / item / carriable class)", u,
                        obj, static_cast<unsigned long long>(vt && gb && vt > gb ? vt - gb : 0), mesh);
                }
            } else if (kind == static_cast<int>(lh::ItemKind::Carriable)) {
                static LONG s_b = 0;
                if (InterlockedIncrement(&s_b) <= 4) {
                    // [PROBE: CARRYHAND] gate=lid_by_hand  site=FLAT  log-only=yes
                    LOG("lidhand: [CARRYHAND] *** carriable %p NOT published: its box (GetExtentsInWorld) did not read *** (%ld of 4)", obj,
                        s_b);
                }
            }
        }
    }
    Publish(lids, rows, bones, lbox, nl, items, ibox, ikind, ni, loots, lootBox, nloot);
    static LONG s_first = 0;
    if (InterlockedIncrement(&s_first) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST gather (logged unconditionally): %u game + %u gaze candidates -> %d lid(s) (one per door), %d "
            "item(s) (%d of them world items) | thread %lu", gameCount, gazeCount, nl, ni, nw, GetCurrentThreadId());
    }
    static LONG s_firstWorld = 0;
    if (nw > 0 && InterlockedIncrement(&s_firstWorld) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST world item gathered: %d world item(s) among %d item(s) - the gather lists them (RESEARCH §14 (2))",
            nw, ni);
    }
    // the game's pick: a lid container or a carried item -> the caller writes the game's own NULL pick
    uintptr_t h = 0, pick = 0;
    const uintptr_t pl = reinterpret_cast<uintptr_t>(player);
    if (!ReadPtr(pl + 5272, &h) || !h || !ReadPtr(pl + 5280, &pick) || !pick) { return false; }
    bool nullIt = pick == reinterpret_cast<uintptr_t>(g_carried[0]) || pick == reinterpret_cast<uintptr_t>(g_carried[1]);
    const bool carried = nullIt;   // a carried item: nulled, but no prompt (trace 20261123_1200, Jay "Lids + purses")
    for (int k = 0; k < nl && !nullIt; ++k) {
        if (reinterpret_cast<uintptr_t>(lids[k]) != pick) { continue; }
        // A LOCKED lid container keeps the game's own prompt (trace 20261119_1135, Jay "Go ahead"): X runs the game's lockpick
        // (OnExecuteTrigger action 60 in state 10 -> +EF79F0); a successful pick sets state 2 (+A6E520) -> nulled again, the hand opens it.
        if (State(lids[k]) == kStateLocked) {
            static void* s_lockedSaid[4] = {};
            static LONG  s_lockedPass = 0;
            bool said = false;
            for (void* p : s_lockedSaid) { said = said || p == lids[k]; }
            if (!said && s_lockedPass < 4) {
                s_lockedSaid[s_lockedPass++] = lids[k];
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] LOCKED - %s %p is locked (state 10): its prompt is left to the game (the lockpick); after a "
                    "successful pick it is closed (state 2) and opens by hand (%ld of 4 objects logged)",
                    RowObject(rows[k]), lids[k], s_lockedPass);
            }
            continue;
        }
        nullIt = true;
    }
    // a grip-loot container (a purse, trace 20261121_2145): hand only, as every lid container - its prompt and X are withheld; a
    // locked one keeps the game's prompt (the same rule as a lid)
    for (int k = 0; k < nloot && !nullIt; ++k) {
        nullIt = reinterpret_cast<uintptr_t>(loots[k]) == pick && State(loots[k]) != kStateLocked;
    }
    // a carriable the grip takes (trace 20261126_1815; Jay: "Stop working on carriables. Along with supressing RT throwing"): hand only,
    // as a lid - X has no target, so the game's carry (and its RT throw) never starts; its prompt goes to the HUD with the grip icon.
    bool carriablePick = false;
    for (int k = 0; k < ni && !nullIt; ++k) {
        nullIt = carriablePick = reinterpret_cast<uintptr_t>(items[k]) == pick &&
                                 lh::PickWithheldForItem(static_cast<lh::ItemKind>(ikind[k]));
    }
    if (carriablePick && InterlockedIncrement(&g_nCarryNull) == 1) {
        // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [CARRYHAND] FIRST CARRIABLE PICK WITHHELD - the game picked carriable %p: X has no target (no game carry, no RT "
            "throw), its prompt goes to the HUD with the grip icon", reinterpret_cast<void*>(pick));
    }
    if (nullIt && InterlockedIncrement(&g_nNull) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST NULL PICK - the game picked %p (a lid container / a carried item / a carriable): its prompt and X are "
            "withheld, the game's interaction never starts for it", reinterpret_cast<void*>(pick));
    }
    // the lid / purse the null hid goes to the HUD prompt only (looting.cpp PickGetHook); a carried item gets none
    if (nullIt && !carried && promptOut) { *promptOut = reinterpret_cast<void*>(pick); }
    return nullIt;
}

void LidHandTick()
{
    // the in-world prompt's game-thread half (trace 20261123_1356) - FIRST, before this feature's own gate (it has its own:
    // inworld_prompts=0 -> hide once, then one bool test).
    PromptQuadTick();
    if (!LidByHand || g_ready != 1 || !g_player) {
        g_busy[0] = g_busy[1] = 0;
        g_zone[0] = g_zone[1] = 0;
        return;
    }
    Snapshot s;
    const bool haveSnap = ReadSnapshot(s);
    // THE INSTANCE CHECK (trace 20261125_1610 v2) - before ANY use of a remembered slot this tick. A container is deleted when no reference
    // object is within 11 m (or none exists: the PlayerDI is replaced at a sleep / an outfit change) and a new one can be built into the
    // freed block at the same address (ENGINE_ARCHITECTURE "Game-object LIFETIME"). Serial with those deletes: both run in CLevel::Frame's
    // post-update loop (LevelDI slot 129 / our HookTail). COST: one stamp (3 reads) + three export calls per slot with an object.
    {
        auto letGo = [](int k, const char* why) {
            for (int side = 0; side < 2; ++side) {
                HandState& H = g_hand[side];
                if (H.lid != k) { continue; }
                H.lid = -1;    // FreeRelease is NOT called: it reads and writes the object, which may be gone
                H.free = false;
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] RELEASE %s hand's lid %s - %s (the instance check; nothing written to it)", kSideName[side],
                    RowObject(g_lids[k].row), why);
            }
        };
        static uintptr_t s_level = 0;
        static int s_next = 0;
        bool rekey = false;
        uintptr_t lv = 0;
        int nx = 0;
        // the PlayerDI (the rig's captured model IS it: roomscale.cpp:5's PlayerDI == the rig's model in the tester's log); null while the
        // game replaces the player - the stamp is then simply not read this tick
        void* const pl = const_cast<void*>(BoneProbeCapturedModel());
        // No stamp this tick (no player) -> no rekey: a changed id is then dropped. Deliberate - deferring it would keep writing into
        // what may be a different object (the defect this fixes); the cost is a renumber inside a player swap dropping live lids once.
        if (g_objId && pl && LevelStamp(pl, &lv, &nx)) {
            const lh::StampVerdict v = lh::StampChange(s_level, s_next, lv, nx);
            if (v != lh::StampVerdict::None) {
                static LONG s_said = 0;
                if (InterlockedIncrement(&s_said) <= 8) {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] ID STAMP (tick) - %s (level %p -> %p, next id %d -> %d): %s (%ld of 8 logged)",
                        v == lh::StampVerdict::Flush ? "another level" : "the level's ids were renumbered (next id went down)",
                        reinterpret_cast<void*>(s_level), reinterpret_cast<void*>(lv), s_next, nx,
                        v == lh::StampVerdict::Flush ? "every remembered lid is forgotten" : "every live lid adopts its new id", s_said);
                }
            }
            if (v == lh::StampVerdict::Flush) {
                for (int k = 0; k < kMaxLids; ++k) {
                    if (!g_lids[k].obj) { continue; }
                    letGo(k, "another level");
                    g_lids[k] = LidState{};
                }
            }
            rekey = v == lh::StampVerdict::Rekey;
            s_level = lv;
            s_next = nx;
        }
        for (int k = 0; k < kMaxLids; ++k) {
            LidState& L = g_lids[k];
            if (!L.obj) { continue; }
            int idNow = -1;
            bool dying = false;
            const bool readOk = ObjIdentity(L.obj, &idNow, &dying);
            const lh::SlotVerdict v = lh::CheckSlot(L.gidOk, L.gid, readOk, idNow, dying, rekey);
            if (v == lh::SlotVerdict::Rekey) {
                L.gid = idNow;
                L.gidOk = true;
            } else if (v == lh::SlotVerdict::Drop) {
                const LONG n = InterlockedIncrement(&g_nStaleSlot);
                const bool owned = g_hand[0].lid == k || g_hand[1].lid == k;
                if (n <= 16) {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] STALE LID SLOT - %s %p: %s (slot id %d, object id now %d)%s - forgotten, nothing written to it "
                        "(%ld of 16 logged)", RowObject(L.row), L.obj,
                        dying ? "the object is being / has been destroyed" : "a DIFFERENT object lives at this address now",
                        L.gidOk ? L.gid : -1, idNow, owned ? " | a hand held it - it lets go" : "", n);
                }
                letGo(k, dying ? "the container is being destroyed" : "a different container lives at its address now");
                L = LidState{};
            }
        }
    }
    // THE HELD DOORS (trace 20261121_1631, Jay CL1b: "after opening it I'm unable to open the other"): an OPENED container leaves the
    // game's gathered triggers, so none of its doors is published. A 2-door object's doors carry `hold` after its open - they join this
    // tick's local list (never g_pub*) while the object is still a ComplexContainer, so a hand can grab them. 1-door lids never get hold.
    int nHeldAdded = 0;
    for (int k = 0; haveSnap && k < kMaxLids && s.nl < kMaxPub; ++k) {
        const LidState& L = g_lids[k];
        if (!L.obj || !L.hold || !IsVt(L.obj, g_vtContainer)) { continue; }
        bool dup = false;
        for (int i = 0; i < s.nl && !dup; ++i) { dup = s.lid[i] == L.obj && s.row[i] == L.row; }
        if (dup || !SafeBox(L.obj, s.lbox[s.nl])) { continue; }
        s.lid[s.nl] = L.obj; s.row[s.nl] = L.row; s.bone[s.nl] = L.bone;
        ++s.nl;
        ++nHeldAdded;
    }
    static LONG s_heldSaid = 0;
    if (nHeldAdded > 0 && InterlockedIncrement(&s_heldSaid) == 1) {
        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
        LOG("lidhand: [LIDHAND] FIRST HELD DOORS joined the grab list (logged once): %d door(s) of an opened 2-door object - the game no "
            "longer gathers it, the hand can still grab them", nHeldAdded);
    }
    void* body = const_cast<void*>(BoneProbeCapturedModel());
    // each published lid's far edge in world, ONCE per tick (both hands read it): parent world . the bone's CURRENT local . edge
    float edgeA[kMaxPub][3], edgeB[kMaxPub][3];
    bool haveEdge[kMaxPub] = {};
    // a FREE LIFT row (trace 20261122_1830): its rim RING in world (centre, normal, radius) - the grab zone is the ring, not the edge
    float ringC[kMaxPub][3] = {}, ringN[kMaxPub][3] = {}, ringR[kMaxPub] = {};
    bool haveRing[kMaxPub] = {};
    for (int i = 0; haveSnap && i < s.nl; ++i) {
        float cur[12], pw[12];
        if (SafeLocal(s.lid[i], s.bone[i], cur) && SafeParentWorld(s.lid[i], s.bone[i], pw)) {
            lh::EdgeWorld(pw, cur, RowEdge(s.row[i]), edgeA[i], edgeB[i]);
            haveEdge[i] = true;
            const int ri = FreeRim(s.row[i]);
            if (ri >= 0) {
                float Wb[12];
                lh::Compose34(pw, cur, Wb);
                MulPoint(Wb, kLidRims[ri].centre, ringC[i]);
                MulRot(Wb, kLidRims[ri].normal, ringN[i]);
                ringR[i] = kLidRims[ri].radius;
                haveRing[i] = true;
            }
        }
    }
    // the hand's distance to published lid i's grab zone: the ring for a free row, else the far edge (-1 = not read)
    auto zoneDist = [&](int i, const float q[3]) -> float {
        if (haveRing[i]) { return lh::RingDist(ringC[i], ringN[i], ringR[i], q); }
        return haveEdge[i] ? lh::SegDist(edgeA[i], edgeB[i], q) : -1.0f;
    };
    for (int side = 0; side < 2; ++side) {
        HandState& H = g_hand[side];
        float sq = 0.0f;
        const bool down = GetControllerSqueeze(side == 0 ? kRightHand : kLeftHand, sq) && sq >= kGrip;
        const bool edge = down && !H.prevDown;
        H.prevDown = down;
        float hw[3];
        const bool haveHand = HandWorldPos(side, hw);
        bool isNear = false;   // (not `near`: windows.h defines near / far as empty macros)
        bool inZone = false;   // g_zone[side] at the end of this tick (trace 20261119_1215: the hip holsters yield to it)
        // ---- a carried item ----
        if (H.item) {
            // a carriable's hand history (trace 20261126_1500): the rig's game-world grip + its QPC, BEFORE the release test so the
            // let-go tick's own sample is in it. COST: one seqlock read per carrying hand per tick (the same read HandWorldPos makes).
            if (H.kind == lh::ItemKind::Carriable) {
                float gp[3], gr[9];
                long long gq = 0;
                if (RigGripWorldRead(side, gp, gr, &gq) && gq) { H.hist.Push(QpcMs(gq), throwable::Vec3{ gp[0], gp[1], gp[2] }); }
            }
            // a chest item is lost with its container's open state / its hook; a world item when the engine no longer returns it by
            // its id (destroyed - e.g. the 80-dropped-item cap) or it left the holder (trace 20261120_2210; INV-663: no read of it then)
            bool lost = H.kind == lh::ItemKind::Chest
                ? (!IsVt(H.itemOwner, g_vtContainer) || State(H.itemOwner) != kStateOpened || !HandCarryStillOn(H.carry))
                : HandCarryObjectGone(H.carry, g_player);
            // A CARRIABLE (final review finding 3): also lost while being destroyed; a LIVE one that has exploded (ExplodingObject +1008,
            // its explosion +A9E720 does not destroy it) is let go where it is, physics on, no credit / velocity. The object's own
            // fields are read only once the engine still returns it by its id (not lost).
            bool explodedLive = false;
            if (H.kind == lh::ItemKind::Carriable) {
                int gid = -1;
                bool dying = false, exploded = false;
                if (!lost) {
                    ObjIdentity(H.item, &gid, &dying);
                    unsigned char ex = 0;
                    exploded = !dying && IsVt(H.item, g_vtExploding) &&
                               SafeReadByte(reinterpret_cast<uintptr_t>(H.item) + kExplodedFlagOff, &ex) && ex != 0;
                }
                const lh::CarryLost v = lh::CarriableLostVerdict(lost, dying, exploded);
                lost = v == lh::CarryLost::Forget;
                explodedLive = v == lh::CarryLost::DetachLive;
            }
            if (lost) {
                ReleaseItem(side, true);
            } else if (explodedLive || !down) {
                ReleaseItem(side, false, explodedLive);
            }
            isNear = H.item != nullptr;
            inZone = isNear;
            // Still carried: its root-level elements at the DRAWN hand's grip socket (trace 20261120_2000). LidHandTick runs after
            // SolveArmIk published this frame's hand and after ItemCtlPlace (boneblend.cpp HookTail). COST: one seqlock read + up to
            // kMaxPlaced 3x4 products and engine writes per carrying hand per tick.
            if (H.item && H.placeN > 0) {
                float D0[12], D[12];
                bool okAll = RigDrawnHolderRead(side, D0);
                if (okAll) { Mul34(D0, H.rel, D); }   // the approved grip's item frame (identity rel = today's placement; trace 20261120_2057)
                for (int k = 0; okAll && k < H.placeN; ++k) {
                    float W[12];
                    Mul34(D, H.placeLocal[k], W);
                    okAll = SafeSetWorld(H.item, H.placeElem[k], W);
                }
                if (!okAll && !H.placeSaidFail) {
                    H.placeSaidFail = true;
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] PLACE REFUSED (first this carry) - %s hand's item %p: %s - it rides the holder this tick",
                        kSideName[side], H.item, RigDrawnHolderRead(side, D) ? "an element write faulted" : "no drawn hand pose (the "
                        "arm not solving, or the holder local not latched)");
                }
            }
        }
        // ---- an owned FREE LIFT lid (trace 20261122_1830): the cover rides the drawn holder; the let-go decides open / back on ----
        if (H.lid >= 0 && H.free) {
            LidState& L = g_lids[H.lid];
            const bool alive = IsVt(L.obj, g_vtContainer);
            if (!down || !alive || !haveHand) {
                FreeRelease(side, H.lid, !down ? "grip opened" : !haveHand ? "no hand pose" : "the container is gone", alive);
            } else {
                float D[12], pw[12];
                if (RigDrawnHolderRead(side, D) && SafeParentWorld(L.obj, L.bone, pw)) {
                    float W[12], pwi[12], loc[12];
                    lh::Compose34(D, H.freeRel, W);
                    lh::InvRigid34(pw, pwi);
                    lh::Compose34(pwi, W, loc);
                    SafeSetLocal(L.obj, L.bone, loc);
                } else if (!H.freeSaidNoHolder) {
                    H.freeSaidNoHolder = true;
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] FREE LIFT %s hand: no drawn hand pose / parent world this tick - the cover holds where it is "
                        "(logged once per carry)", kSideName[side]);
                }
                isNear = true;
                inZone = true;
            }
        } else
        // ---- an owned lid ----
        if (H.lid >= 0) {
            LidState& L = g_lids[H.lid];
            float pivotW[3], axisW[3];
            if (!down || !IsVt(L.obj, g_vtContainer) || !haveHand || !HingeWorld(L, pivotW, axisW)) {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] RELEASE %s hand's lid %s at %.1f %s (%s)", kSideName[side], RowObject(L.row), L.deg,
                    RowUnit(L.row), !down ? "grip opened" : !haveHand ? "no hand pose" : "the container is gone");
                H.lid = -1;
            } else {
                const float openDeg = RowOpenBase(L.row);
                const float prev = L.deg;
                float deg = 0.0f;
                float pwc[12];
                if (IsClipRow(L.row) && ClipSmoothed(L.row)) {
                    // the BASKET cover (trace 20261121_2225): the chosen frame sticks to its stretch of the looping path, and the written
                    // frame eases toward it over kClipEaseS (QPC dt; the first tick after the grab does not move)
                    const float target = SafeParentWorld(L.obj, L.bone, pwc)
                        ? lh::ClipNearestT(ClipFramesOf(L.row), ClipRow(L.row).nFrames, pwc, H.clipG, hw, H.clipT, kClipWindowFrames,
                                           kClipStickMPerFrame)
                        : H.clipT;
                    static LONGLONG s_qf = 0;
                    if (!s_qf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qf = f.QuadPart; }
                    LARGE_INTEGER q;
                    QueryPerformanceCounter(&q);
                    const float dt = H.clipQpc ? static_cast<float>(static_cast<double>(q.QuadPart - H.clipQpc) / static_cast<double>(s_qf)) : 0.0f;
                    H.clipQpc = q.QuadPart;
                    H.clipT = lh::ClipEase(H.clipT, target, dt, kClipEaseS);
                    deg = H.clipT;
                } else if (IsClipRow(L.row)) {
                    // a CLIP row (trace 20261121_2145): the clip frame whose grab point is nearest the hand, within 6 frames of now
                    deg = SafeParentWorld(L.obj, L.bone, pwc)
                        ? lh::ClipNearestT(ClipFramesOf(L.row), ClipRow(L.row).nFrames, pwc, H.clipG, hw, L.deg, kClipWindowFrames)
                        : L.deg;
                } else {
                    deg = lh::ClampLid(H.lidAtGrab + lh::HandAngleDeg(pivotW, axisW, H.grabW, hw), openDeg);
                }
                WriteLid(L, deg);
                isNear = true;
                inZone = true;
                if (lh::CrossesOpenPoint(prev, deg, openDeg, LidOpenFrac) && State(L.obj) == kStateClosed) {
                    // A 2-DOOR object (trace 20261121_1549, Jay: the other door "Stays shut"): the game's open pose moves BOTH doors, so
                    // every other door of this object is held at its angle read NOW (before the execute), and all its doors get `hold`.
                    int others = 0, held = 0;
                    for (int k = 0; haveSnap && k < s.nl; ++k) {
                        if (s.lid[k] != L.obj || s.row[k] == L.row) { continue; }
                        ++others;
                        LidState tmp{}; tmp.obj = s.lid[k]; tmp.row = s.row[k]; tmp.bone = s.bone[k];
                        float now = 0.0f;
                        if (!LidNowDeg(tmp, &now)) { continue; }
                        const int j = NewLidSlot(s.lid[k], s.row[k], s.bone[k]);
                        if (j < 0) { continue; }
                        LidState& O = g_lids[j];
                        O.hold = true;
                        if (g_hand[1 - side].lid != j) {   // the other hand drives it: leave its angle to that hand
                            O.deg = lh::ClampLid(now, RowEnd(O.row));
                            O.writing = true;
                            O.gameClip = false;
                            O.easing = false;
                            O.checkHold = false;
                        }
                        ++held;
                    }
                    if (others > 0) { L.hold = true; }
                    const bool ok = SafeExecute(g_player, L.obj);
                    const int st = State(L.obj);
                    InterlockedIncrement(&g_nOpen);
                    // trace 20261119_1310 (RUN LH7: "It'll open to X height, then close and do the games open animation again"): the
                    // execute's setter plays the lid's Open stage FROM ITS START; jump it to its END pose and ease ours there instead.
                    int key = -1;
                    const bool jumped = ok && st == kStateOpened && SafeOpenJump(L.obj, &key);
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] OPEN CALL %s %p at %.1f %s (open point %.1f): the game's own execute (+B9B930, action 6) %s"
                        " -> state %d (want 1) | open jump (stage key %d): %s - the hand lets go | other doors %d, held shut %d%s",
                        RowObject(L.row), L.obj, deg, RowUnit(L.row), LidOpenFrac * openDeg, ok ? "called" : "*** FAULTED ***", st, key,
                        jumped ? "the game's pose JUMPED to open, ours eases there from the hand's angle"
                        : !g_playStage ? "OFF (see READY) - the game's clip plays from its start"
                        : st != kStateOpened ? "not tried (state is not 1)"
                        : "*** REFUSED / FAULTED (no stage key or the call faulted) *** - the game's clip plays from its start",
                        others, held, others > held ? " *** NOT ALL HELD (angle unread / no free slot) - the game's pose shows on the rest ***"
                        : !haveSnap ? " (no snapshot this tick - siblings not checked)" : "");
                    if (jumped) {
                        L.easing = true;
                        L.easeFrom = deg;
                        LARGE_INTEGER q;
                        QueryPerformanceCounter(&q);
                        L.easeT0 = q.QuadPart;
                        L.writing = true;
                        L.gameClip = false;
                    } else {
                        L.gameClip = true;
                        L.writing = false;
                    }
                    H.lid = -1;
                }
            }
        }
        // ---- a new grab: an item first (it lies in an open container), else a lid ----
        if (edge && H.lid < 0 && !H.item && (!haveHand || !haveSnap)) {
            static LONG s_q = 0;
            if (InterlockedIncrement(&s_q) <= 4) {
                // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=yes
                LOG("lidhand: [CARRYHAND] PRESS (%s hand) NOT TESTED: %s%s (%ld of 4)", kSideName[side],
                    haveHand ? "" : "*** no game-world hand (RigGripWorldRead) *** ",
                    haveSnap ? "" : "*** no published gather snapshot ***", s_q);
            }
        }
        if (H.lid < 0 && !H.item && haveHand && haveSnap) {
            float dl[kMaxPub], di[kMaxPub];
            for (int i = 0; i < s.ni; ++i) {
                di[i] = -1.0f;
                // NOT the item either hand carries (trace 20261124_1950, Jay: "Block it"): a carried chest item stays its container's
                // item and in reach, so the free hand took it again with its home read off the other hand's holder (RUN LA2's machete)
                if (s.item[i] == g_carried[0] || s.item[i] == g_carried[1]) { continue; }
                // a world item (trace 20261120_2210): in reach is enough; a DroppedItem flying to a target (an arrow / a thrown item in
                // flight - its own CanExecute +6E2430 refuses it then) is not grabbed
                if (s.ikind[i] != static_cast<int>(lh::ItemKind::Chest)) {
                    unsigned char flying = 0;
                    if (s.ikind[i] == static_cast<int>(lh::ItemKind::Dropped) &&
                        (!SafeReadByte(reinterpret_cast<uintptr_t>(s.item[i]) + kDropFlyingOff, &flying) || flying)) {
                        continue;
                    }
                    di[i] = looting::BoxDist(s.ibox[i], hw);
                    continue;
                }
                uintptr_t owner = 0;
                if (!ReadPtr(reinterpret_cast<uintptr_t>(s.item[i]) + kItemOwnerOff, &owner) ||
                    !IsVt(reinterpret_cast<void*>(owner), g_vtContainer) || State(reinterpret_cast<void*>(owner)) != kStateOpened) {
                    continue;
                }
                // reachable only while its container's lid is open past the open point (a closed lid hides them, spec §4.5) - for a
                // 2-door object, ANY of its doors (trace 20261121_1549); an owner not published at all stays reachable, as before
                bool published = false, anyOpen = false;
                for (int k = 0; k < s.nl && !anyOpen; ++k) {
                    if (reinterpret_cast<uintptr_t>(s.lid[k]) != owner) { continue; }
                    published = true;
                    LidState tmp{}; tmp.obj = s.lid[k]; tmp.row = s.row[k]; tmp.bone = s.bone[k];
                    float now = 0.0f;
                    anyOpen = LidNowDeg(tmp, &now) && now >= LidOpenFrac * RowOpenBase(tmp.row);
                }
                if (published && !anyOpen) { continue; }
                di[i] = looting::BoxDist(s.ibox[i], hw);
            }
            for (int i = 0; i < s.nl; ++i) {
                dl[i] = -1.0f;
                const int st = State(s.lid[i]);
                if (st == kStateLocked) {
                    // the grab stays refused on a locked lid - and now says so (trace 20261119_1135; it was silent in RUN LH3)
                    static LONG s_lockedGrab = 0;
                    if (edge && haveEdge[i] && zoneDist(i, hw) <= kLidEdgeReachM &&
                        InterlockedIncrement(&s_lockedGrab) <= 8) {
                        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                        LOG("lidhand: [LIDHAND] GRAB REFUSED - LOCKED: %s hand on %s %p (state 10) - pick the lock first (the game's "
                            "prompt) (%ld of 8 logged)", kSideName[side], RowObject(s.row[i]), s.lid[i], s_lockedGrab);
                    }
                    continue;
                }
                if (st != kStateClosed && st != kStateOpened) { continue; }
                // THE LID ZONE (trace 20261118_2010): within kLidEdgeReachM of the lid's far edge, wherever the lid is now
                if (!haveEdge[i]) { continue; }
                dl[i] = zoneDist(i, hw);   // a free row: the rim ring (trace 20261122_1830)
            }
            const int bi = lh::NearestWithin(di, s.ni, LootReachM);
            int bl = bi < 0 ? lh::NearestWithin(dl, s.nl, kLidEdgeReachM) : -1;
            // THE DOOR TIE-BREAK (trace 20261121_1549): another door of the SAME object in reach -> the one whose leaf the hand is over
            // (the two edges meet at the seam, so the nearest edge alone cannot tell them apart). Never runs for a 1-door object.
            for (int k = 0; bl >= 0 && k < s.nl; ++k) {
                if (k == bl || s.lid[k] != s.lid[bl] || dl[k] < 0.0f || dl[k] > kLidEdgeReachM || !haveEdge[k]) { continue; }
                float over[2] = { -1.0f, -1.0f };
                const int cand[2] = { bl, k };
                for (int c = 0; c < 2; ++c) {
                    LidState tmp{}; tmp.obj = s.lid[cand[c]]; tmp.row = s.row[cand[c]]; tmp.bone = s.bone[cand[c]];
                    float pw[3], aw[3];
                    if (!HingeWorld(tmp, pw, aw)) { continue; }
                    const int e = cand[c];
                    const float em[3] = { (edgeA[e][0] + edgeB[e][0]) / 2, (edgeA[e][1] + edgeB[e][1]) / 2, (edgeA[e][2] + edgeB[e][2]) / 2 };
                    over[c] = lh::LeafOvershoot(pw, aw, em, hw);
                }
                if (over[0] >= 0.0f && over[1] >= 0.0f && over[1] < over[0]) { bl = k; }
            }
            // A GRIP-LOOT container (a purse, trace 20261121_2145): nearest in reach, after items and lids; any state is a zone (the
            // press on an opened one says so), the press only calls the game's open on a closed one
            int bo = -1;
            if (bi < 0 && bl < 0) {
                float dlo[kMaxPub];
                for (int i = 0; i < s.nloot; ++i) {
                    const int st = State(s.loot[i]);
                    dlo[i] = (st == kStateClosed || st == kStateOpened) ? looting::BoxDist(s.lootBox[i], hw) : -1.0f;
                }
                bo = lh::NearestWithin(dlo, s.nloot, LootReachM);
            }
            // RUN CO1 diagnostic: a grip press that takes nothing - the first 8 say what was published and how near the nearest
            // carriable was (any distance), or what blocked a press that HAD one in reach. COST: on a grip edge only.
            if (edge && (bi < 0 || g_padOwned[side] || !body) && bl < 0 && bo < 0) {
                static LONG s_p = 0;
                const LONG pn = InterlockedIncrement(&s_p);
                if (pn <= 8) {
                    int nc = 0;
                    float best = -1.0f;
                    for (int i = 0; i < s.ni; ++i) {
                        if (s.ikind[i] != static_cast<int>(lh::ItemKind::Carriable)) { continue; }
                        ++nc;
                        const float d = looting::BoxDist(s.ibox[i], hw);
                        if (best < 0.0f || d < best) { best = d; }
                    }
                    // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=yes
                    LOG("lidhand: [CARRYHAND] PRESS %ld of 8 (%s hand) took nothing: %s | published %d item(s), %d carriable(s), nearest "
                        "carriable box %.3f m (reach %.2f) | hand (%.2f %.2f %.2f)", pn, kSideName[side],
                        bi >= 0 ? (g_padOwned[side] ? "*** an earlier poller owns this press (holster / two-hand / reload) ***"
                                                    : "*** no player body model ***")
                                : "nothing in reach", s.ni, nc, best, LootReachM, hw[0], hw[1], hw[2]);
                }
            }
            isNear = isNear || bi >= 0 || bl >= 0 || bo >= 0;
            // THE ENTRY BUZZ (Jay: "Buzz is fine"): a light pulse when this hand comes into a lid's / an item's zone
            const bool zone = bi >= 0 || bl >= 0 || bo >= 0;
            if (zone && !H.prevZone && RigTwoHandHaptic > 0.0f) {
                PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic * 0.35f, 15);
            }
            H.prevZone = zone;
            inZone = inZone || zone;
            if (edge && !g_padOwned[side] && bi >= 0 && body) {
                void* item = s.item[bi];
                const lh::ItemKind kind = static_cast<lh::ItemKind>(s.ikind[bi]);
                // A PILE (HeavyObjectsSpawner, trace 20261126_1500): its own spawn once, then the object it handed out is the one taken
                if (kind == lh::ItemKind::Carriable && IsVt(item, g_vtHeavySpawner)) {
                    // only when the take can follow (review minor 4, trace 20261126_1800): a spawn whose take is refused leaves its
                    // object at the pile with the pile "one out" (SafeSpawnFrom then refuses every later grip)
                    int holderElem = -1;
                    const bool worldOk = HandCarryWorldTakeReady();
                    if (body) { SafeElemId(body, side == 0 ? "R_HandHolder" : "L_HandHolder", &holderElem); }
                    void* spawned = nullptr;
                    if (!lh::PileSpawnReady(worldOk, body != nullptr, holderElem)) {
                        // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                        LOG("lidhand: [CARRYHAND] SPAWN NOT ASKED %s hand: pile %p - the take could not follow (%s) - nothing spawned",
                            kSideName[side], item, !worldOk ? "the world take's exports are not ready" : !body ? "no player body"
                                                                                               : "the hand's holder element not found");
                    } else {
                        spawned = SafeSpawnFrom(item);
                        // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                        LOG("lidhand: [CARRYHAND] SPAWN %s hand: pile %p -> %s %p", kSideName[side], item,
                            spawned ? (IsVt(spawned, g_vtHeavy) ? "HeavyObject" : "*** an object of another class ***") : "*** REFUSED "
                            "(one already out, nothing handed out, or a fault) - not taken ***", spawned);
                    }
                    item = spawned;
                }
                const bool world = kind != lh::ItemKind::Chest;
                uintptr_t owner = 0;
                int home = -1, onElem = -1;
                if (!world) { ReadPtr(reinterpret_cast<uintptr_t>(item) + kItemOwnerOff, &owner); }
                const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
                // The hand's GRIP SOCKET, where the game hangs every held item (INV-493: a holder-less item sits at identity on it) -
                // in the palm; r_hand / l_hand put the item at the wrist (trace 20261120_1900, Jay: "it doesn't sit in the palm").
                const char* const holderName = side == 0 ? "R_HandHolder" : "L_HandHolder";
                // a chest item hangs off its container's hook element; a world item off none (HandCarryTakeWorld refuses one that does)
                const bool taken = item && SafeElemId(body, holderName, &onElem) && onElem >= 0 &&
                    (world ? HandCarryTakeWorld(H.carry, item, body, onElem, ident)
                           : (SafeParentMeshElem(item, &home) && home >= 0 &&
                              HandCarryTakeAs(H.carry, item, reinterpret_cast<void*>(owner), home, body, onElem, ident)));
                if (taken) {
                    H.item = item;
                    H.itemOwner = reinterpret_cast<void*>(owner);
                    H.kind = kind;
                    g_carried[side] = item;
                    InterlockedIncrement(&g_nTake);
                    if (RigTwoHandHaptic > 0.0f) { PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40); }
                    // A held DroppedItem stops simulating: physics OFF right after the attach, the same tick (the game's +ECF2E0 does it
                    // just before its attach; no physics step runs between the two on this thread). Its OnDetached restarts it.
                    // A carriable (trace 20261126_1500): the same physics OFF; ITS detach restarts nothing - ReleaseItem turns it back on.
                    const bool physKind = kind == lh::ItemKind::Dropped || kind == lh::ItemKind::Carriable;
                    H.physOff = physKind && SafePhysicsOff(item);
                    if (kind == lh::ItemKind::Carriable) { H.hist.Clear(); }   // the let-go velocity is the motion AFTER the take
                    char physText[96] = "";
                    if (physKind) {
                        strcpy_s(physText, H.physOff ? " | physics OFF while held (the game's slot +256 type 0)"
                                                     : " | *** physics NOT switched off (no bind / faulted) - it may fight the hand ***");
                    }
                    // The game's loose-item category while carried: the player's headroom check stops seeing it at the shoulder
                    // (trace 20261120_1634). Restored at the give-back (ReleaseItem). An event - never per frame. Set only when it
                    // differs (DroppedItem / spawners already carry 2048 - INV-758), so only a change is ever restored.
                    char catText[224] = " | collision category not changed (PhysicsBind export missing)";   // 112 + physText (<= 96)
                    H.catSet = false;
                    if (g_physBind) {
                        int cat = 0;
                        if (!SafeGetCategory(item, &cat)) {
                            strcpy_s(catText, " | *** collision category NOT changed: the bind read faulted ***");
                        } else if (cat == kLooseItemCategory) {
                            sprintf_s(catText, " | collision category already %d (the game's loose-item value)", cat);
                        } else if (SafeSetCategory(item, kLooseItemCategory)) {
                            H.savedCat = cat;
                            H.catSet = true;
                            sprintf_s(catText, " | collision category %d -> %d (the game's loose-item value)", cat, kLooseItemCategory);
                        } else {
                            strcpy_s(catText, " | *** collision category NOT changed: the bind write faulted ***");
                        }
                    }
                    strcat_s(catText, physText);
                    // The drawn-hand placement (trace 20261120_2000): latch each ROOT-LEVEL element's local now (the item sits at
                    // identity on the holder, so its object frame IS the holder frame); every tick writes them from the drawn hand.
                    H.placeN = 0;
                    H.placeSaidFail = false;
                    int nEl = 0;
                    char placeText[96] = " | drawn-hand placement OFF (exports missing)";
                    if (g_setWorld && g_elemNum && SafeElemNum(item, &nEl)) {
                        for (int e = 0; e < nEl && H.placeN < kMaxPlaced; ++e) {
                            int par = -2;
                            if (SafeElemParent(item, e, &par) && par < 0 && SafeLocal(item, e, H.placeLocal[H.placeN])) {
                                H.placeElem[H.placeN++] = e;
                            }
                        }
                        sprintf_s(placeText, " | placed from the drawn hand: %d root element(s) of %d", H.placeN, nEl);
                    }
                    // The item's mesh, once per take (trace 20261120_1905): the base name every per-item pose row is keyed by.
                    char itemMesh[96] = "(name not read)";
                    {
                        char full[96] = {};
                        if (g_getMeshName && SafeMeshName(item, full, sizeof(full))) {
                            const char* s = full;
                            for (const char* q = full; *q; ++q) { if (*q == '/' || *q == '\\') { s = q + 1; } }
                            strncpy_s(itemMesh, sizeof(itemMesh), s, _TRUNCATE);
                            char* dot = strrchr(itemMesh, '.');
                            if (dot) { *dot = 0; }
                        }
                    }
                    // Jay's approved grip for this mesh (trace 20261120_2057): the row, this side's item frame in the drawn socket frame;
                    // published to the finger pass AFTER g_carried. An event - never per frame.
                    const int prow = lh::PoseFind(kLidPoseNames.n, kLidPoseCount, itemMesh);
                    H.poseRow = prow;
                    const float ident12[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
                    for (int k = 0; k < 12; ++k) { H.rel[k] = ident12[k]; }
                    if (prow >= 0) {
                        const LidPoseRow& PR = kLidPoses[prow];
                        for (int r = 0; r < 3; ++r) {
                            for (int c = 0; c < 3; ++c) { H.rel[4 * r + c] = PR.relR[side][3 * r + c]; }
                            H.rel[4 * r + 3] = PR.relT[side][r];
                        }
                    }
                    InterlockedExchange(&g_poseRow[side], prow);
                    char poseText[96];
                    if (prow >= 0) {
                        sprintf_s(poseText, " | pose row %d (Jay's grip, %s hand)", prow, kSideName[side]);
                    } else {
                        strcpy_s(poseText, " | NO POSE ROW (today's placement, the game's fingers)");
                    }
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] TAKE %s hand: %s item %p mesh '%s' from %s %p (its hook element %d) - drawn on %s "
                        "(element %d) until let go%s%s%s", kSideName[side], KindName(kind), item, itemMesh,
                        world ? "the world (home = its world matrix)" : "container", reinterpret_cast<void*>(owner), home,
                        holderName, onElem, catText, placeText, poseText);
                    if (kind == lh::ItemKind::Carriable) {
                        // [PROBE: CARRYHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                        LOG("lidhand: [CARRYHAND] TAKE %s hand: %s %p mesh '%s' - the game's take NOT called (no game carry, "
                            "no RT throw); physics %s; let go = thrown with the hand's velocity", kSideName[side],
                            IsVt(item, g_vtExploding) ? "ExplodingObject" : IsVt(item, g_vtHeavy) ? "HeavyObject" : "*** other class ***",
                            item, itemMesh, H.physOff ? "OFF while held" : "*** NOT switched off ***");
                    }
                } else {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] REFUSED take of %s item %p (%s hand): home element %d, hand element %d - not carried%s",
                        KindName(kind), item, kSideName[side], home, onElem,
                        world ? " (a world item: see WORLD TAKE REFUSED / WORLD CARRY for the reason)" : "");
                }
            } else if (edge && !g_padOwned[side] && bl >= 0) {
                const int k = NewLidSlot(s.lid[bl], s.row[bl], s.bone[bl]);
                float pivotW[3], axisW[3], now = 0.0f;
                float pwg[12];
                const bool freeRow = k >= 0 && FreeRim(g_lids[k].row) >= 0;
                if (k >= 0 && HingeWorld(g_lids[k], pivotW, axisW) && LidNowDeg(g_lids[k], &now) &&
                    (!IsClipRow(g_lids[k].row) || SafeParentWorld(g_lids[k].obj, g_lids[k].bone, pwg)) &&
                    (!freeRow || FreeGrab(side, g_lids[k], pwg))) {   // a FREE LIFT row (trace 20261122_1830): rides the drawn holder
                    LidState& L = g_lids[k];
                    H.lid = k;
                    H.lidAtGrab = lh::ClampLid(now, RowEnd(L.row));
                    memcpy(H.grabW, hw, sizeof(H.grabW));
                    if (IsClipRow(L.row)) {
                        // the hand's point in the bone's OWN frame at the grab (trace 20261121_2145): g = inv(parentW . L(t)) . hand
                        float Lt[12], Wb[12];
                        RowLocal(L.row, H.lidAtGrab, Lt);
                        Mul34(pwg, Lt, Wb);
                        const float d[3] = { hw[0] - Wb[3], hw[1] - Wb[7], hw[2] - Wb[11] };
                        for (int c = 0; c < 3; ++c) { H.clipG[c] = Wb[0 * 4 + c] * d[0] + Wb[1 * 4 + c] * d[1] + Wb[2 * 4 + c] * d[2]; }
                        if (ClipSmoothed(L.row)) {
                            // the BASKET cover (trace 20261121_2225): the grab point ON the rim under the hand (the grab edge, bone frame),
                            // not the hand's own point (0.19 m off the rim in RUN CF2) - its path no longer loops round the hand
                            const float* e = RowEdge(L.row);
                            float rim[3];
                            lh::SegClosest(e, e + 3, H.clipG, rim);
                            for (int c = 0; c < 3; ++c) { H.clipG[c] = rim[c]; }
                        }
                        H.clipT = H.lidAtGrab;
                        H.clipQpc = 0;
                    }
                    L.deg = H.lidAtGrab;
                    L.gameClip = false;
                    L.writing = true;
                    L.easing = false;      // a grab mid-ease takes the lid from our eased angle (trace 20261119_1310)
                    L.checkHold = false;
                    InterlockedIncrement(&g_nGrab);
                    if (RigTwoHandHaptic > 0.0f) { PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40); }
                    const float em[3] = { (edgeA[bl][0] + edgeB[bl][0]) / 2, (edgeA[bl][1] + edgeB[bl][1]) / 2,
                                          (edgeA[bl][2] + edgeB[bl][2]) / 2 };
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] GRAB %s hand: %s %p (state %d) at %.1f %s, hand %.3f m from the lid's edge (zone %.2f) | "
                        "hand %.3f m from the hinge line, the edge %.3f m (equal = on the edge; larger = more hand travel per degree)%s",
                        kSideName[side], RowObject(L.row), L.obj, State(L.obj), H.lidAtGrab, RowUnit(L.row), dl[bl], kLidEdgeReachM,
                        lh::LineDist(pivotW, axisW, hw), lh::LineDist(pivotW, axisW, em),
                        H.free ? " | FREE LIFT - the hand's distance is to the RIM RING; the cover rides the hand, let go lifted > 0.20 m = "
                                 "the game's open" : "");
                } else {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] REFUSED grab of %p (%s hand): %s", s.lid[bl], kSideName[side],
                        k < 0 ? "no free lid slot" : freeRow ? "a FREE LIFT cover, but no drawn hand pose / the lid's local or parent "
                        "world did not read" : "the hinge / lid local did not read");
                }
            } else if (edge && !g_padOwned[side] && bo >= 0) {
                // GRIP LOOT (trace 20261121_2145, Jay: "just holding grip on the item to loot it should be enough"): the game's own
                // execute on a CLOSED purse - its open is the money pickup ("pick_up_money", Trigger_Open). An opened one: no call.
                void* obj = s.loot[bo];
                const int st0 = State(obj);
                if (st0 == kStateClosed) {
                    const bool ok = SafeExecute(g_player, obj);
                    const int st = State(obj);
                    InterlockedIncrement(&g_nOpen);
                    if (RigTwoHandHaptic > 0.0f) { PulseController(side == 0 ? kRightHand : kLeftHand, RigTwoHandHaptic, 40); }
                    char mesh[96] = "(name not read)";
                    SafeMeshName(obj, mesh, sizeof(mesh));
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] GRIP LOOT %s hand: %s %p - the game's own execute (+B9B930) %s -> state %d (want 1 = opened, "
                        "the loot taken)", kSideName[side], mesh, obj, ok ? "called" : "*** FAULTED ***", st);
                } else {
                    static void* s_lootRefSaid[8] = {};
                    static LONG  s_lootRefN = 0;
                    bool said = false;
                    for (void* p : s_lootRefSaid) { said = said || p == obj; }
                    if (!said && s_lootRefN < 8) {
                        s_lootRefSaid[s_lootRefN++] = obj;
                        // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                        LOG("lidhand: [LIDHAND] GRIP LOOT REFUSED %s hand: %p is in state %d (not closed - already looted); no call "
                            "(%ld of 8 objects logged)", kSideName[side], obj, st0, s_lootRefN);
                    }
                }
            }
            // A MISS names itself (trace 20261119_1215; RUN LH5's 13 unowned presses logged nothing): a grip edge that started no
            // grab / take with a lid edge or an item within 1.5 m. Raw distances (every published lid / item, whatever its state).
            if (edge && H.lid < 0 && !H.item && bo < 0) {
                float ml = -1.0f, mi = -1.0f;
                int li = -1;
                for (int i = 0; i < s.nl; ++i) {
                    if (!haveEdge[i]) { continue; }
                    const float d = zoneDist(i, hw);
                    if (ml < 0.0f || d < ml) { ml = d; li = i; }
                }
                for (int i = 0; i < s.ni; ++i) {
                    const float d = looting::BoxDist(s.ibox[i], hw);
                    if (mi < 0.0f || d < mi) { mi = d; }
                }
                static LONG s_miss = 0;
                if (((ml >= 0.0f && ml < 1.5f) || (mi >= 0.0f && mi < 1.5f)) && InterlockedIncrement(&s_miss) <= 40) {
                    // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                    LOG("lidhand: [LIDHAND] MISS %s hand press: nearest lid edge %.3f m (%s, state %d; zone %.2f), nearest item %.3f m "
                        "(reach %.2f; item %s) | press owned elsewhere %ld (a holster / two-hand / reload / loot took it) | in a zone %d "
                        "(%ld of 40 logged)", kSideName[side], ml, li >= 0 ? RowObject(s.row[li]) : "none",
                        li >= 0 ? State(s.lid[li]) : -1, kLidEdgeReachM, mi, LootReachM,
                        bi >= 0 ? "in reach" : "none in reach (closed / below the open point / not open)", g_padOwned[side],
                        zone ? 1 : 0, s_miss);
                }
            }
        }
        InterlockedExchange(&g_busy[side], isNear ? 1 : 0);
        InterlockedExchange(&g_zone[side], inZone ? 1 : 0);
    }
    // ---- lids released off rest keep their angle (visual only), until the game's clip owns them or they are far ----
    float any[3];
    const bool haveAny = HandWorldPos(0, any) || HandWorldPos(1, any);
    for (int i = 0; i < kMaxLids; ++i) {
        LidState& L = g_lids[i];
        if (!L.obj || g_hand[0].lid == i || g_hand[1].lid == i) { continue; }
        if (!IsVt(L.obj, g_vtContainer)) { L = LidState{}; continue; }
        // the tick after the ease (nothing of ours written since): the game's own pose, once per object (trace 20261119_1310)
        if (L.checkHold) {
            L.checkHold = false;
            float gameDeg = -1.0f;
            const bool read = LidNowDeg(L, &gameDeg);
            static LONG s_hold = 0;
            if (InterlockedIncrement(&s_hold) <= 8) {
                // [PROBE: LIDHAND] gate=lid_by_hand  site=HEADSET  log-only=no
                LOG("lidhand: [LIDHAND] HOLD %s %p - after the ease the game's own pose reads %s%.1f %s (open end %.1f; ~open = the "
                    "jump held, ~0 = it fell back closed) | state %d (%ld of 8 logged)", RowObject(L.row), L.obj,
                    read ? "" : "*** UNREAD *** ", gameDeg, RowUnit(L.row), RowEnd(L.row), State(L.obj), s_hold);
            }
        }
        if (L.easing) {
            // from the hand's angle to the open angle over kOpenEaseS (smoothstep), then the game's jumped pose holds it
            static LONGLONG s_freq = 0;
            if (!s_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_freq = f.QuadPart; }
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            const float t = static_cast<float>(static_cast<double>(q.QuadPart - L.easeT0) / static_cast<double>(s_freq)) / kOpenEaseS;
            if (t >= 1.0f) {
                L.easing = false;
                L.writing = false;
                L.gameClip = true;
                L.checkHold = true;
                continue;
            }
            const float u = t <= 0.0f ? 0.0f : t * t * (3.0f - 2.0f * t);
            L.deg = L.easeFrom + (RowEnd(L.row) - L.easeFrom) * u;
            WriteLid(L, L.deg);
            continue;
        }
        if (!L.writing || L.gameClip) { continue; }
        float box[6];
        if (haveAny && SafeBox(L.obj, box) && looting::BoxDist(box, any) > kDropDistM) { L.writing = false; continue; }
        WriteLid(L, L.deg);
    }
}

void LidHandPoll(bool vrGrip, bool leftOwned, bool rightOwned, bool& withholdL1, bool& withholdR1)
{
    withholdL1 = withholdR1 = false;
    if (!LidByHand || !vrGrip) { return; }
    float sqL = 0.0f, sqR = 0.0f;
    const bool downL = GetControllerSqueeze(kLeftHand, sqL) && sqL >= kGrip;
    const bool downR = GetControllerSqueeze(kRightHand, sqR) && sqR >= kGrip;
    InterlockedExchange(&g_padOwned[1], downL && leftOwned ? 1 : 0);
    InterlockedExchange(&g_padOwned[0], downR && rightOwned ? 1 : 0);
    withholdL1 = downL && !leftOwned && g_busy[1] != 0;
    withholdR1 = downR && !rightOwned && g_busy[0] != 0;
}

bool LidHandHandInZone(int side)
{
    if (!LidByHand || g_ready != 1 || side < 0 || side > 1) { return false; }
    return g_zone[side] != 0;
}

bool LidHandCarrying(int side)
{
    if (!LidByHand || side < 0 || side > 1) { return false; }
    return g_carried[side] != nullptr;   // set at the take, cleared in ReleaseItem (trace 20261120_1815)
}

bool LidHandIsCarriable(void* obj)
{
    // the vtables are written once before g_ready = 1 (Ready); IsVt reads the object's first qword, guarded (trace 20261126_1815)
    if (!LidByHand || g_ready != 1 || !obj) { return false; }
    return IsVt(obj, g_vtExploding) || IsVt(obj, g_vtHeavy) || IsVt(obj, g_vtHeavySpawner);
}

void* LidHandCarriedItem(int side)
{
    if (!LidByHand || side < 0 || side > 1) { return nullptr; }
    return g_carried[side];   // the AFW mask's carried loot (trace 20261124_1925)
}

int LidHandPoseRow(int side)
{
    if (!LidByHand || side < 0 || side > 1) { return -1; }
    const LONG r = g_poseRow[side];   // set at the take after g_carried, -1 before g_carried clears (trace 20261120_2057)
    return (r >= 0 && r < kLidPoseCount) ? static_cast<int>(r) : -1;
}

bool LidHandPoseData(int row, int side, const float (**pin)[9], const float (**pinT)[3], const float (**rest)[9],
                     const char* const** names, const char** mesh)
{
    if (row < 0 || row >= kLidPoseCount || side < 0 || side > 1) { return false; }
    *pin = kLidPoses[row].pin[side];
    *pinT = kLidPoses[row].pinT[side];
    *rest = kLidPinRest[side];
    *names = kLidPinName[side];
    *mesh = kLidPoses[row].mesh;
    return true;
}

int LidHandPinCount() { return kLidPinCount; }

}  // namespace dlvr
