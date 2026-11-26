// lidhand_logic_test.cpp - unit tests for lidhand_logic.h (plan docs/superpowers/plans/2026-11-18-hand-open-lids-phase2.md Task 2).
// Standalone: cl /nologo /EHsc /W4 /std:c++17 /I <tree>\src\ovr2xr lidhand_logic_test.cpp. Prints "lidhand_logic_test: N failure(s)"
// and exits N. Expected values from OUR data: tools/lid_hinge_table.py --dump-frame 49 (the chest's clip, frame 49).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "lidhand_logic.h"

namespace lh = dlvr::lidhand;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}
static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// (a) LidLocal reproduces the game's own clip: the chest MESH REST rotated 58.400368 deg about its hinge == clip frame 49's local
//     (angle measured from the mesh rest - the clip's frame 0 sits 0.22 deg off it, so the first version of this test, at 58.180975 =
//     the angle from frame 0, failed by exactly that).
static void TestLidLocalMatchesClip(bool deliberateMiss)
{
    const float rest[12] = { 0.002451f, -0.000000f, -0.999997f, 0.379868f, 0.000000f, 1.000000f, -0.000000f, -0.000000f,
                             0.999997f, 0.000000f, 0.002451f, -0.255129f };
    const float axis[3] = { 0.0f, 1.0f, 0.0f };
    const float clipF49[12] = { 0.853012f, 0.000000f, -0.521891f, 0.379868f, -0.000000f, 1.000000f, -0.000000f, -0.000000f,
                                0.521891f, 0.000000f, 0.853012f, -0.255129f };
    float out[12];
    lh::LidLocal(rest, axis, deliberateMiss ? 50.0f : 58.400368f, out);
    double worst = 0.0;
    for (int i = 0; i < 12; ++i) { worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - clipF49[i])); }
    Check(deliberateMiss ? worst > 1e-3 : worst <= 1e-3, deliberateMiss ? "LidLocal deliberate miss must differ" : "LidLocal == clip f49");
    // and the angle back out of the local
    const float back = lh::LidAngleFromLocal(rest, out, axis);
    Check(deliberateMiss || Near(back, 58.400368, 1e-3), "LidAngleFromLocal round trip");
}

// (b) the hand's angle about the hinge, relative to where it grabbed: +30 one way, -30 the other.
static void TestHandAngle()
{
    const float pivot[3] = { 1.0f, 0.5f, 2.0f };
    const float axis[3] = { 0.0f, 1.0f, 0.0f };
    const float grab[3] = { 1.0f + 0.4f, 0.8f, 2.0f };       // 0.4 m out along +X, any height
    const double r = 30.0 * 3.14159265358979323846 / 180.0;
    // a +30 deg turn about +Y takes +X toward -Z
    const float handP[3] = { static_cast<float>(1.0 + 0.4 * std::cos(r)), 0.6f, static_cast<float>(2.0 - 0.4 * std::sin(r)) };
    const float handM[3] = { static_cast<float>(1.0 + 0.4 * std::cos(r)), 0.6f, static_cast<float>(2.0 + 0.4 * std::sin(r)) };
    Check(Near(lh::HandAngleDeg(pivot, axis, grab, handP), 30.0, 1e-3), "HandAngleDeg +30");
    Check(Near(lh::HandAngleDeg(pivot, axis, grab, handM), -30.0, 1e-3), "HandAngleDeg -30");
    const float onAxis[3] = { 1.0f, 3.0f, 2.0f };            // on the hinge line: no direction -> 0, never NaN
    Check(lh::HandAngleDeg(pivot, axis, grab, onAxis) == 0.0f, "HandAngleDeg on the axis = 0");
}

// (c) the open point fires once on the way up, never on the way back, and the clamp holds the lid in 0..open.
static void TestOpenPointAndClamp()
{
    int fires = 0;
    float prev = 0.0f;
    for (int i = 0; i <= 60; ++i) { const float now = static_cast<float>(i); if (lh::CrossesOpenPoint(prev, now, 58.181f, 0.5f)) { ++fires; } prev = now; }
    for (int i = 60; i >= 0; --i) { const float now = static_cast<float>(i); if (lh::CrossesOpenPoint(prev, now, 58.181f, 0.5f)) { ++fires; } prev = now; }
    Check(fires == 1, "CrossesOpenPoint fires exactly once over 0->60->0");
    Check(lh::ClampLid(-5.0f, 58.181f) == 0.0f && lh::ClampLid(70.0f, 58.181f) == 58.181f && lh::ClampLid(20.0f, 58.181f) == 20.0f,
          "ClampLid 0..open");
}

// (d) the nearest within reach.
static void TestNearest()
{
    const float d[3] = { 0.08f, 0.05f, 0.30f };
    Check(lh::NearestWithin(d, 3, 0.25f) == 1, "NearestWithin picks 0.05 over 0.08");
    const float far[1] = { 0.30f };
    Check(lh::NearestWithin(far, 1, 0.25f) == -1, "NearestWithin -1 beyond reach");
    const float none[2] = { -1.0f, -1.0f };
    Check(lh::NearestWithin(none, 2, 0.25f) == -1, "NearestWithin ignores no-reading (-1)");
}

// (e) releasing an item: stow only in the shoulder zone with the take action 1 (23 = inventory full).
static void TestItemRelease()
{
    // (the 2-argument form is gone - plan 2026-11-20-world-items-by-hand Task 6; a chest item away from the shoulder now DROPS, Jay:
    //  "chest items drop with physics too")
    Check(lh::OnItemRelease(true, 1, lh::ItemKind::Chest) == lh::ItemRelease::Stow, "shoulder + action 1 = Stow");
    Check(lh::OnItemRelease(true, 23, lh::ItemKind::Chest) == lh::ItemRelease::GiveBack, "shoulder + action 23 = GiveBack");
    Check(lh::OnItemRelease(false, 1, lh::ItemKind::Chest) == lh::ItemRelease::Drop, "away from the shoulder = Drop (was GiveBack)");
}

// (h) plan 2026-11-20-world-items-by-hand Task 3 (Jay: "Just do the physics then instead", "chest items drop with physics too"): the
//     release by kind. Shoulder + action 1 = Stow; inventory refuses (23) or unread (-1) = GiveBack for an item with a home (Chest,
//     Spawner), Drop for a DroppedItem (no home - it falls); anything else = Drop.
static void TestItemReleaseByKind()
{
    using K = lh::ItemKind;
    using R = lh::ItemRelease;
    const K kinds[3] = { K::Chest, K::Spawner, K::Dropped };
    for (const K k : kinds) {
        Check(lh::OnItemRelease(true, 1, k) == R::Stow, "release_shoulder_takes");
        Check(lh::OnItemRelease(false, 1, k) == R::Drop, "release_away_drops");
    }
    Check(lh::OnItemRelease(false, 23, K::Chest) == R::GiveBack, "release_inventory_full_goes_home (chest)");
    Check(lh::OnItemRelease(false, 23, K::Spawner) == R::GiveBack, "release_inventory_full_goes_home (spawner)");
    Check(lh::OnItemRelease(false, 23, K::Dropped) == R::Drop, "release_inventory_full_goes_home (dropped: no home, falls)");
    Check(lh::OnItemRelease(true, 23, K::Chest) == R::GiveBack, "release_shoulder_full (chest)");
    Check(lh::OnItemRelease(true, 23, K::Dropped) == R::Drop, "release_shoulder_full (dropped)");
    Check(lh::OnItemRelease(false, -1, K::Spawner) == R::GiveBack, "release_unread_action (spawner, as 23)");
    Check(lh::OnItemRelease(true, -1, K::Chest) == R::GiveBack, "release_unread_action (chest at the shoulder, as 23)");
    // release_is_per_side: the rule holds no state - the right hand's answer does not change the left hand's
    const R right = lh::OnItemRelease(true, 1, K::Spawner);
    const R left = lh::OnItemRelease(false, 1, K::Chest);
    Check(right == R::Stow && left == R::Drop && lh::OnItemRelease(true, 1, K::Spawner) == right, "release_is_per_side");
}

// (h2) Plan 2026-11-26-carried-objects-by-hand Task 2: a carriable (gas canister / crate - no inventory entry) is never stowed and never
//      given back: every let-go is a Drop (it leaves the hand with the hand's velocity). The production change that fails this: any
//      Carriable branch that reaches the action test (shoulder + 1 -> Stow, 23 / -1 -> GiveBack).
static void TestCarriableRelease()
{
    using K = lh::ItemKind;
    using R = lh::ItemRelease;
    Check(lh::OnItemRelease(true, 29, K::Carriable) == R::Drop, "carriable_shoulder_29_drops");
    Check(lh::OnItemRelease(false, -1, K::Carriable) == R::Drop, "carriable_unread_action_drops");
    Check(lh::OnItemRelease(true, 1, K::Carriable) == R::Drop, "carriable_never_stows");
    Check(lh::OnItemRelease(false, 23, K::Carriable) == R::Drop, "carriable_never_goes_home");
    // the three existing kinds unchanged by the new kind (appended last)
    Check(static_cast<int>(K::Chest) == 0 && static_cast<int>(K::Spawner) == 1 && static_cast<int>(K::Dropped) == 2 &&
          static_cast<int>(K::Carriable) == 3, "carriable_appended_last");
}

// (h2b) Final review findings 1 / 2: a carriable is grabbable only when the game's action is 29 AND its own CanExecute (slot +1048)
//       agrees - CanExecute is where the pile's "one already out" (+880 / +888), "enabled" (+416) and IsActiveOnModule live (woc1 :773,
//       :550, :688). The production change that fails this: publishing on the action alone (the reviewed code).
// RUN CO2 (2026-11-26): a carriable gas cylinder showing the game's X prompt read action 1 - the action test returns 1 when the object's
// own CanExecuteTake (vfunc +1632) passes, 29 only on a later path (woc1 +AA1820 / +B33350 / +B35B30). The production change that fails
// this: accepting 29 alone (the build RUN CO1 / CO2 ran).
static void TestCarriableGrabbable()
{
    Check(lh::CarriableGrabbable(1, true), "grabbable_1_and_canexecute (RUN CO2's canister)");
    Check(lh::CarriableGrabbable(29, true), "grabbable_29_and_canexecute");
    Check(!lh::CarriableGrabbable(1, false), "not_grabbable_1_when_canexecute_refuses");
    Check(!lh::CarriableGrabbable(29, false), "not_grabbable_when_canexecute_refuses (pile out / disabled / inactive)");
    Check(!lh::CarriableGrabbable(0, true), "not_grabbable_other_action");
    Check(!lh::CarriableGrabbable(23, true), "not_grabbable_action_23");
    Check(!lh::CarriableGrabbable(-1, true), "not_grabbable_action_unread");
    Check(lh::IsCarryAction(1) && lh::IsCarryAction(29) && !lh::IsCarryAction(0) && !lh::IsCarryAction(12), "carry_actions_are_1_and_29");
}

// (h2c) Final review finding 3: a held carriable that the engine no longer returns by id, or that is being destroyed = Forget (no call
//       on it); one that is LIVE but has exploded (ExplodingObject +1008 set by its explosion +A9E720, woc4 :39) = DetachLive (let go
//       where it is, physics on, no credit / velocity); else Keep. The production change that fails this: the reviewed lost test, which
//       only knew "gone".
static void TestCarriableLost()
{
    using L = lh::CarryLost;
    Check(lh::CarriableLostVerdict(false, false, false) == L::Keep, "carriable_kept");
    Check(lh::CarriableLostVerdict(true, false, false) == L::Forget, "carriable_gone_forgotten");
    Check(lh::CarriableLostVerdict(false, true, false) == L::Forget, "carriable_dying_forgotten");
    Check(lh::CarriableLostVerdict(true, false, true) == L::Forget, "carriable_gone_beats_exploded");
    Check(lh::CarriableLostVerdict(false, true, true) == L::Forget, "carriable_dying_beats_exploded");
    Check(lh::CarriableLostVerdict(false, false, true) == L::DetachLive, "carriable_exploded_live_detached");
}

// (h3) The let-go velocity: the hand's own velocity 1:1, its LENGTH clamped to the cap (spec §6b: 16 m/s, the game's own top 8 x 2).
//      The production change that fails this: any rescale (the throwables' item-speed share) or a per-axis clamp.
static void TestCarryLaunchVelocity()
{
    Check(Near(lh::kCarryMaxSpeed, 16.0, 1e-6), "carry_cap_is_16");
    lh::V3 a = lh::CarryLaunchVelocity(lh::V3{ 3.0f, 0.0f, 4.0f }, 16.0f);
    Check(Near(a.x, 3.0, 1e-4) && Near(a.y, 0.0, 1e-4) && Near(a.z, 4.0, 1e-4), "carry_velocity_under_cap_is_1to1");
    lh::V3 b = lh::CarryLaunchVelocity(lh::V3{ 30.0f, 0.0f, 40.0f }, 16.0f);
    Check(Near(b.x, 9.6, 1e-4) && Near(b.y, 0.0, 1e-4) && Near(b.z, 12.8, 1e-4), "carry_velocity_length_clamped");
    lh::V3 c = lh::CarryLaunchVelocity(lh::V3{ 0.0f, 0.0f, 0.0f }, 16.0f);
    Check(Near(c.x, 0.0, 1e-9) && Near(c.y, 0.0, 1e-9) && Near(c.z, 0.0, 1e-9), "carry_velocity_still_is_zero");
    lh::V3 d = lh::CarryLaunchVelocity(lh::V3{ 0.0f, -20.0f, 0.0f }, 16.0f);
    Check(Near(d.y, -16.0, 1e-4), "carry_velocity_down_clamped_too");
    // review minor 7: a non-finite hand velocity is dropped (0), never sent to the physics
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    lh::V3 e = lh::CarryLaunchVelocity(lh::V3{ nan, 1.0f, 1.0f }, 16.0f);
    Check(e.x == 0.0f && e.y == 0.0f && e.z == 0.0f, "carry_velocity_nan_is_zero");
    lh::V3 f = lh::CarryLaunchVelocity(lh::V3{ 1.0f, inf, 1.0f }, 16.0f);
    Check(f.x == 0.0f && f.y == 0.0f && f.z == 0.0f, "carry_velocity_inf_is_zero");
}

// The 5 review minors (trace 20261126_1800).
static void TestCarryMinors()
{
    // minor 4: a pile spawns only when the take can follow
    Check(lh::PileSpawnReady(true, true, 3), "pile_spawn_ready_all_ok");
    Check(!lh::PileSpawnReady(false, true, 3), "pile_spawn_no_world_take");
    Check(!lh::PileSpawnReady(true, false, 3), "pile_spawn_no_body");
    Check(!lh::PileSpawnReady(true, true, -1), "pile_spawn_no_holder_element");
    // minor 5: the release reads the action only where it is used
    Check(!lh::ReleaseNeedsAction(lh::ItemKind::Carriable), "release_carriable_no_action");
    Check(lh::ReleaseNeedsAction(lh::ItemKind::Chest) && lh::ReleaseNeedsAction(lh::ItemKind::Spawner) &&
          lh::ReleaseNeedsAction(lh::ItemKind::Dropped), "release_others_read_action");
    // minor 6: the credit line names the real reason
    Check(std::strstr(lh::CreditText(lh::CreditResult::Given, 912), "+912") != nullptr, "credit_text_heavy");
    Check(std::strstr(lh::CreditText(lh::CreditResult::Given, 1344), "+1344") != nullptr, "credit_text_exploding");
    Check(std::strstr(lh::CreditText(lh::CreditResult::NotExactClass, 0), "not one of the two") != nullptr, "credit_text_class");
    Check(std::strstr(lh::CreditText(lh::CreditResult::Faulted, 0), "FAULTED") != nullptr, "credit_text_fault");
    Check(std::strstr(lh::CreditText(lh::CreditResult::NoExport, 0), "OFF") != nullptr, "credit_text_no_export");
    Check(std::strstr(lh::CreditText(lh::CreditResult::NotCalled, 0), "not called") != nullptr, "credit_text_not_called");
    Check(std::strstr(lh::CreditText(lh::CreditResult::Faulted, 0), "not one of the two") == nullptr, "credit_text_fault_not_class");
    // minor 8: an object whose detach worked is free even when the placement write faulted
    Check(lh::ReleaseFreed(true, true), "freed_clean_release");
    Check(lh::ReleaseFreed(false, true), "freed_detached_placement_faulted");
    Check(!lh::ReleaseFreed(false, false), "not_freed_detach_faulted");
}

// The carriables' prompt (trace 20261126_1815): X withheld on a grabbable carriable (the grip only), the HUD keeps the game's "Take".
static void TestCarryPrompt()
{
    Check(lh::PickWithheldForItem(lh::ItemKind::Carriable), "pick_withheld_carriable");
    Check(!lh::PickWithheldForItem(lh::ItemKind::Chest) && !lh::PickWithheldForItem(lh::ItemKind::Dropped) &&
          !lh::PickWithheldForItem(lh::ItemKind::Spawner), "pick_kept_other_items");
    Check(lh::PromptKeepsGameText(true), "prompt_carriable_keeps_take");
    Check(!lh::PromptKeepsGameText(false), "prompt_lid_says_grab");
}

// (i) Task 6 - which inventory entry the take added or raised (before / after snapshots of entry pointers + stack counts, INV-755).
static void TestInvDelta()
{
    int a = 0, b = 0, c = 0;   // stand-ins for three inventory entries (only their addresses are compared)
    const void* bp[2] = { &a, &b };
    int count = -9;
    {   // a NEW entry -> its whole stack
        const int bc[2] = { 5, 1 };
        const void* ap[3] = { &a, &b, &c };
        const int ac[3] = { 5, 1, 3 };
        Check(lh::InvDelta(bp, bc, 2, ap, ac, 3, &count) == 2 && count == 3, "inv_delta_new_entry_whole_stack");
    }
    {   // stack_raise_drops_the_rise: an owned stack of 5 raised to 8 -> that entry, 3 (Review Focus 4)
        const int bc[2] = { 5, 1 };
        const void* ap[2] = { &a, &b };
        const int ac[2] = { 8, 1 };
        Check(lh::InvDelta(bp, bc, 2, ap, ac, 2, &count) == 0 && count == 3, "stack_raise_drops_the_rise");
    }
    {   // a new non-stacking entry with count 0 -> 1
        const int bc[2] = { 5, 1 };
        const void* ap[3] = { &a, &b, &c };
        const int ac[3] = { 5, 1, 0 };
        Check(lh::InvDelta(bp, bc, 2, ap, ac, 3, &count) == 2 && count == 1, "inv_delta_new_zero_count_is_one");
    }
    {   // nothing new, nothing raised (ammo / cash: no entry) -> -1 (Review Focus 2)
        const int bc[2] = { 5, 1 };
        const void* ap[2] = { &a, &b };
        const int ac[2] = { 5, 1 };
        Check(lh::InvDelta(bp, bc, 2, ap, ac, 2, &count) == -1, "inv_delta_none");
    }
    {   // a count that FELL elsewhere (something used) is not the take
        const int bc[2] = { 5, 1 };
        const void* ap[2] = { &a, &b };
        const int ac[2] = { 4, 1 };
        Check(lh::InvDelta(bp, bc, 2, ap, ac, 2, &count) == -1, "inv_delta_fall_is_not_a_take");
    }
}

// (i2) trace 20261121_1627 - RUN CL1b: an UNRELATED entry (the equipped grappling hook, id 77) changed BEFORE the taken item's stack
//      (duct tape, id 12, 4 -> 5) in snapshot order. InvDelta picks the hook (index 0) - the defect; InvDeltaMatch(want 12) must pick the
//      tape (index 1, +1). deliberateMiss asks for the hook's id: the tape must then NOT be picked.
static void TestInvDeltaMatch(bool deliberateMiss)
{
    int hook = 0, tape = 0, other = 0;
    const void* bp[3] = { &hook, &tape, &other };
    const int bc[3] = { 1, 4, 2 };
    const void* ap[3] = { &hook, &tape, &other };
    const int ac[3] = { 2, 5, 2 };
    const int aid[3] = { 77, 12, 30 };
    int count = -9;
    Check(lh::InvDelta(bp, bc, 3, ap, ac, 3, &count) == 0, "control: the OLD rule picks the hook (CL1b's defect reproduced)");
    const int got = lh::InvDeltaMatch(bp, bc, 3, ap, ac, aid, 3, deliberateMiss ? 77 : 12, &count);
    if (deliberateMiss) {
        Check(got != 1, "deliberate miss: asking for the hook's id must not return the tape");
        std::printf("  (miss: want 77 -> index %d, count %d - %s)\n", got, count, got == 1 ? "TAPE - BAD" : "not the tape - good");
    } else {
        Check(got == 1 && count == 1, "inv_delta_match picks the taken item's stack (+1), not the earlier changed entry");
    }
    // a new entry of the item, no stack before -> its whole stack
    {
        const void* ap2[4] = { &hook, &tape, &other, &count };
        const int ac2[4] = { 2, 4, 2, 3 };
        const int aid2[4] = { 77, 12, 30, 55 };
        Check(lh::InvDeltaMatch(bp, bc, 3, ap2, ac2, aid2, 4, 55, &count) == 3 && count == 3, "inv_delta_match_new_entry");
    }
    // the item's id never seen / never raised -> -1 (taken as a normal pickup); an unread want -> -1
    Check(lh::InvDeltaMatch(bp, bc, 3, ap, ac, aid, 3, 99, &count) == -1, "inv_delta_match_none");
    Check(lh::InvDeltaMatch(bp, bc, 3, ap, ac, aid, 3, lh::kNoId, &count) == -1, "inv_delta_match_unread_id");
}

// (g) trace 20261120_2057 - the per-item pose row by the item's mesh as GetMeshName reports it (folder, case, ".msh" all vary): the key is
//     the lower-case base name; an unknown / empty / null mesh is -1 (today's placement); a name that only PREFIXES a key does not match.
static void TestPoseFind()
{
    const char* const names[] = { "loot_ok_pouch_a", "loot_ok_sugar_a", "wn_shuriken_a_bundle" };
    const int n = 3;
    Check(lh::PoseFind(names, n, "loot_ok_pouch_a") == 0, "exact key");
    Check(lh::PoseFind(names, n, "Data/Objects/Loot/LOOT_OK_POUCH_A.msh") == 0, "folder + upper case + .msh");
    Check(lh::PoseFind(names, n, "data\\x\\loot_ok_sugar_a.msh") == 1, "backslash folder");
    Check(lh::PoseFind(names, n, "wn_shuriken_a_bundle") == 2, "last key");
    Check(lh::PoseFind(names, n, "loot_ok_pouch") == -1, "a prefix of a key is not a match");
    Check(lh::PoseFind(names, n, "misc_unknown.msh") == -1, "unknown");
    Check(lh::PoseFind(names, n, "") == -1, "empty");
    Check(lh::PoseFind(names, n, nullptr) == -1, "null");
}

// (f) trace 20261118_2010 - the lid's far edge (lid_hinge_table.inc, OUR mesh). In the lid bone's PARENT frame (parentW = identity):
//     at 0 deg the edge's middle sits R from the pivot (tool: basket 0.651, big chest 0.865, sedan_b 0.566), and a hand that moves
//     WITH the edge from closed to open turns HandAngleDeg by exactly +openDeg - the grab maths and the drawn edge agree in sign and size.
#include "lid_hinge_table.inc"
static void TestEdge(const char* object, double wantR, bool deliberateMiss)
{
    const LidHinge* h = nullptr;
    for (const LidHinge& x : kLidHinges) { if (std::strcmp(x.object, object) == 0) { h = &x; } }
    Check(h != nullptr, object);
    if (!h) { return; }
    const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float L0[12], Lo[12], a0[3], b0[3], ao[3], bo[3];
    lh::LidLocal(h->rest, h->axis, 0.0f, L0);
    lh::LidLocal(h->rest, h->axis, deliberateMiss ? -h->openDeg : h->openDeg, Lo);
    lh::EdgeWorld(I, L0, h->edge, a0, b0);
    lh::EdgeWorld(I, Lo, h->edge, ao, bo);
    const float m0[3] = { (a0[0] + b0[0]) / 2, (a0[1] + b0[1]) / 2, (a0[2] + b0[2]) / 2 };
    const float mo[3] = { (ao[0] + bo[0]) / 2, (ao[1] + bo[1]) / 2, (ao[2] + bo[2]) / 2 };
    char what[160];
    std::snprintf(what, sizeof(what), "%s edge R %.3f (want %.3f)", object, lh::LineDist(h->pivot, h->axis, m0), wantR);
    Check(Near(lh::LineDist(h->pivot, h->axis, m0), wantR, 0.002), what);
    const float turned = lh::HandAngleDeg(h->pivot, h->axis, m0, mo);
    std::snprintf(what, sizeof(what), "%s hand-with-edge turns %.3f (want %s%.3f)", object, turned, deliberateMiss ? "NOT " : "",
                  h->openDeg);
    Check(deliberateMiss ? !Near(turned, h->openDeg, 0.01) : Near(turned, h->openDeg, 0.01), what);
}

// (f2) trace 20261121_1549 - the door tie-break on the wardrobe's two doors (one mesh, both lid bones' parent = bone_root, so both
//      rows are in ONE frame - parentW = identity as TestEdge). Their closed edges meet at the seam. A hand 10% in from the seam on the
//      LEFT leaf: left overshoot 0, right > 0.04 m; the mirror for the right leaf. deliberateMiss puts the hand on the OTHER leaf
//      (the input a wrong pick would see) - the check must then NOT pass.
static void TestLeafOvershoot(bool deliberateMiss)
{
    const LidHinge* d[2] = {};
    for (const LidHinge& x : kLidHinges) {
        if (std::strcmp(x.object, "loot_wardrobe (left door)") == 0) { d[0] = &x; }
        if (std::strcmp(x.object, "loot_wardrobe (right door)") == 0) { d[1] = &x; }
    }
    Check(d[0] && d[1], "wardrobe rows present");
    if (!d[0] || !d[1]) { return; }
    const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float mid[2][3];
    for (int i = 0; i < 2; ++i) {
        float L0[12], a[3], b[3];
        lh::LidLocal(d[i]->rest, d[i]->axis, 0.0f, L0);
        lh::EdgeWorld(I, L0, d[i]->edge, a, b);
        for (int k = 0; k < 3; ++k) { mid[i][k] = (a[k] + b[k]) / 2; }
    }
    float seam = 0.0f;
    for (int k = 0; k < 3; ++k) { seam += (mid[0][k] - mid[1][k]) * (mid[0][k] - mid[1][k]); }
    char what[160];
    std::snprintf(what, sizeof(what), "wardrobe edges meet at the seam (%.4f m apart, want < 0.01)", std::sqrt(seam));
    Check(std::sqrt(seam) < 0.01f, what);
    for (int on = 0; on < 2; ++on) {
        const int src = deliberateMiss ? 1 - on : on;
        float hand[3];
        for (int k = 0; k < 3; ++k) { hand[k] = 0.9f * mid[src][k] + 0.1f * d[src]->pivot[k]; }
        const float own = lh::LeafOvershoot(d[on]->pivot, d[on]->axis, mid[on], hand);
        const float other = lh::LeafOvershoot(d[1 - on]->pivot, d[1 - on]->axis, mid[1 - on], hand);
        const bool pass = own == 0.0f && other > 0.04f;
        std::snprintf(what, sizeof(what), "%s leaf: own overshoot %.4f, other %.4f (want own 0, other > 0.04)%s", on == 0 ? "left" : "right",
                      own, other, deliberateMiss ? " - DELIBERATE MISS (hand on the other leaf): must NOT pass" : "");
        Check(deliberateMiss ? !pass : pass, what);
        if (deliberateMiss) { std::printf("  (miss %s: own %.4f other %.4f -> %s)\n", on == 0 ? "left" : "right", own, other, pass ? "PASSED - BAD" : "refused - good"); }
    }
}

// (g) segment / line distances.
static void TestDistances()
{
    const float a[3] = { 0, 0, 0 }, b[3] = { 1, 0, 0 };
    const float mid[3] = { 0.5f, 0.2f, 0 }, past[3] = { 1.3f, 0.4f, 0 }, before[3] = { -0.3f, 0, 0 };
    Check(Near(lh::SegDist(a, b, mid), 0.2, 1e-6), "SegDist perpendicular 0.2");
    Check(Near(lh::SegDist(a, b, past), 0.5, 1e-6), "SegDist past the end 0.5");
    Check(Near(lh::SegDist(a, b, before), 0.3, 1e-6), "SegDist before the start 0.3");
    Check(Near(lh::SegDist(a, a, mid), std::sqrt(0.29), 1e-6), "SegDist zero-length = point distance");
    const float ax[3] = { 2, 0, 0 };
    Check(Near(lh::LineDist(a, ax, past), 0.4, 1e-6), "LineDist ignores the ends");
}

// (clip-follow, plan 2026-11-21-clip-follow-containers Task 2) a 3-frame "clip": a door turning 0 / 90 / 180 deg about Z, its bone at
// (1, 0, 0) in the parent frame, translation sliding +0.1 m in y per frame - the path of a grab point g = (1, 0, 0) in the bone frame.
static void ClipFixture(float f[3][12])
{
    for (int k = 0; k < 3; ++k) {
        const double a = 3.14159265358979 / 2.0 * k;
        const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
        const float m[12] = { c, -s, 0, 1.0f, s, c, 0, 0.1f * k, 0, 0, 1, 0 };
        std::memcpy(f[k], m, sizeof(m));
    }
}

static void TestClipLocalAt(bool deliberateMiss)
{
    float f[3][12];
    ClipFixture(f);
    float out[12];
    lh::ClipLocalAt(f, 3, 0.0f, out);
    double w = 0.0;
    for (int i = 0; i < 12; ++i) { w = std::fmax(w, std::fabs(out[i] - f[0][i])); }
    Check(w <= 1e-6, "ClipLocalAt t=0 == frame 0");
    lh::ClipLocalAt(f, 3, 2.0f, out);
    w = 0.0;
    for (int i = 0; i < 12; ++i) { w = std::fmax(w, std::fabs(out[i] - f[2][i])); }
    Check(w <= 1e-6, "ClipLocalAt t=n-1 == the last frame");
    lh::ClipLocalAt(f, 3, -3.0f, out);
    Check(Near(out[0], f[0][0], 1e-6) && Near(out[7], f[0][7], 1e-6), "ClipLocalAt clamps below 0");
    lh::ClipLocalAt(f, 3, 5.0f, out);
    Check(Near(out[0], f[2][0], 1e-6) && Near(out[7], f[2][7], 1e-6), "ClipLocalAt clamps above n-1");
    // mid-frame: 45 deg about Z (slerp), translation y 0.05
    lh::ClipLocalAt(f, 3, 0.5f, out);
    const double ang = std::atan2(out[4], out[0]) * 180.0 / 3.14159265358979;
    const double want = deliberateMiss ? 40.0 : 45.0;
    Check(deliberateMiss ? !Near(ang, want, 1e-3) : Near(ang, want, 1e-3), deliberateMiss ? "ClipLocalAt deliberate miss (40 != 45)"
                                                                                          : "ClipLocalAt t=0.5 slerps to 45 deg");
    Check(deliberateMiss || Near(out[7], 0.05, 1e-6), "ClipLocalAt t=0.5 lerps the translation");
}

static void TestClipNearestT()
{
    float f[3][12];
    ClipFixture(f);
    const float pw[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    const float g[3] = { 1.0f, 0.0f, 0.0f };
    // the hand placed AT the path point of t = 1.25
    float L[12], p[3];
    lh::ClipLocalAt(f, 3, 1.25f, L);
    lh::MulPoint34(L, g, p);
    const float t = lh::ClipNearestT(f, 3, pw, g, p, 1.0f, 6.0f);
    Check(Near(t, 1.25, 0.25), "ClipNearestT: a hand on the path returns its t");
    // WindowedSearchRefusesJump: a 41-frame clip turning 4.5 deg per frame; the hand at t = 30's point, tNow 5, window 6 -> <= 11
    static float big[41][12];
    for (int k = 0; k < 41; ++k) {
        const double a = 3.14159265358979 / 40.0 * k;
        const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
        const float m[12] = { c, -s, 0, 1.0f, s, c, 0, 0, 0, 0, 1, 0 };
        std::memcpy(big[k], m, sizeof(m));
    }
    lh::ClipLocalAt(big, 41, 30.0f, L);
    lh::MulPoint34(L, g, p);
    const float tj = lh::ClipNearestT(big, 41, pw, g, p, 5.0f, 6.0f);
    Check(tj <= 11.0f && tj >= 10.5f, "ClipNearestT: a hand 25 frames ahead moves t at most the window (6)");
}

static void TestClipTFromLocal()
{
    float f[3][12];
    ClipFixture(f);
    float cur[12];
    lh::ClipLocalAt(f, 3, 1.5f, cur);
    Check(Near(lh::ClipTFromLocal(f, 3, cur), 1.5, 0.25), "ClipTFromLocal reads a mid-frame t");
    lh::ClipLocalAt(f, 3, 0.0f, cur);
    Check(Near(lh::ClipTFromLocal(f, 3, cur), 0.0, 0.25), "ClipTFromLocal reads t = 0 at rest");
    // the per-tick cost (final review, trace 20261121_2145): a 67-frame table (the basket's size) costs at most 67 frame compares + 9
    // interpolations - and still reads a mid-frame t exactly
    static float big[67][12];
    for (int k = 0; k < 67; ++k) {
        const double a = 3.14159265358979 / 2.0 / 66.0 * k;
        const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
        const float m[12] = { c, -s, 0, 0.01f * k, s, c, 0, 0, 0, 0, 1, 0 };
        std::memcpy(big[k], m, sizeof(m));
    }
    lh::ClipLocalAt(big, 67, 41.75f, cur);
    int evals = 0;
    const float tb = lh::ClipTFromLocal(big, 67, cur, &evals);
    Check(Near(tb, 41.75, 0.13), "ClipTFromLocal reads t = 41.75 on a 67-frame table");
    Check(evals <= 9, "ClipTFromLocal interpolates at most 9 times (coarse frames, then +-1 frame)");
}

// (basket smoothing, trace 20261121_2225) a path that goes OUT and comes BACK along x (the cover's loop): t=2 at x 0.2000, t=8 at
// x 0.2005; the hand at x 0.2004 is 0.1 mm nearer t=8. tNow = 3: without stickiness t jumps to 8; with 0.003 m per frame it stays at 2.
static void TestClipStick(bool deliberateMiss)
{
    static float f[11][12];
    const float xs[11] = { 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.4f, 0.3f, 0.2005f, 0.1f, 0.0f };
    for (int k = 0; k < 11; ++k) {
        const float m[12] = { 1, 0, 0, xs[k], 0, 1, 0, 0, 0, 0, 1, 0 };
        std::memcpy(f[k], m, sizeof(m));
    }
    const float pw[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    const float g[3] = { 0, 0, 0 };
    const float hand[3] = { 0.2004f, 0, 0 };
    const float loose = lh::ClipNearestT(f, 11, pw, g, hand, 3.0f, 6.0f);
    Check(Near(loose, 8.0, 0.13), "ClipNearestT without stickiness takes the nearer far frame (8)");
    const float stuck = lh::ClipNearestT(f, 11, pw, g, hand, 3.0f, 6.0f, deliberateMiss ? 0.0f : 0.003f);
    Check(deliberateMiss ? Near(stuck, 8.0, 0.13) : Near(stuck, 2.0, 0.13),
          deliberateMiss ? "deliberate miss: stickiness 0 still jumps" : "ClipNearestT with stickiness keeps the current stretch (2)");
}

static void TestClipEase()
{
    Check(Near(lh::ClipEase(10.0f, 20.0f, 0.0f, 0.05f), 10.0, 1e-6), "ClipEase dt 0 does not move");
    Check(Near(lh::ClipEase(10.0f, 20.0f, 0.05f, 0.05f), 10.0 + 10.0 * (1.0 - std::exp(-1.0)), 1e-4), "ClipEase dt = tau goes 63.2 %");
    Check(Near(lh::ClipEase(10.0f, 20.0f, 0.5f, 0.05f), 20.0, 1e-3), "ClipEase dt = 10 tau arrives");
    Check(Near(lh::ClipEase(10.0f, 20.0f, 0.01f, 0.0f), 20.0, 1e-6), "ClipEase tau 0 = no easing");
}

static void TestSegClosest()
{
    const float a[3] = { 0, 0, 0 }, b[3] = { 1, 0, 0 };
    float o[3];
    const float q1[3] = { 0.3f, 0.5f, -0.2f };
    lh::SegClosest(a, b, q1, o);
    Check(Near(o[0], 0.3, 1e-6) && Near(o[1], 0.0, 1e-6) && Near(o[2], 0.0, 1e-6), "SegClosest projects onto the segment");
    const float q2[3] = { -2.0f, 1.0f, 0.0f };
    lh::SegClosest(a, b, q2, o);
    Check(Near(o[0], 0.0, 1e-6), "SegClosest clamps before the start");
    const float q3[3] = { 5.0f, 0.0f, 3.0f };
    lh::SegClosest(a, b, q3, o);
    Check(Near(o[0], 1.0, 1e-6), "SegClosest clamps past the end");
}

// THE FREE LIFT (trace 20261122_1830): the rigid inverse round-trips, a hand-carried pose reproduces itself, the ring distance is right
// on, inside and above the rim, and the let-go opens only past 0.20 m.
static void TestFreeLift()
{
    // a rigid 3x4: 30 deg about z, translated
    const double a = 30.0 * 3.14159265358979 / 180.0;
    const float M[12] = { static_cast<float>(std::cos(a)), static_cast<float>(-std::sin(a)), 0.0f, 0.4f,
                          static_cast<float>(std::sin(a)), static_cast<float>(std::cos(a)), 0.0f, -1.2f,
                          0.0f, 0.0f, 1.0f, 0.9f };
    float Mi[12], I[12];
    lh::InvRigid34(M, Mi);
    lh::Compose34(Mi, M, I);
    const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    double worst = 0.0;
    for (int k = 0; k < 12; ++k) { worst = std::fmax(worst, std::fabs(I[k] - id[k])); }
    Check(worst < 1e-5, "InvRigid34 . M == identity");
    // the carry: rel = inv(D0) . W0; with the hand still (D = D0), W = D . rel reproduces W0
    const float D0[12] = { 0, -1, 0, 1.5f, 1, 0, 0, 0.2f, 0, 0, 1, 1.1f };
    float D0i[12], rel[12], W[12];
    lh::InvRigid34(D0, D0i);
    lh::Compose34(D0i, M, rel);
    lh::Compose34(D0, rel, W);
    worst = 0.0;
    for (int k = 0; k < 12; ++k) { worst = std::fmax(worst, std::fabs(W[k] - M[k])); }
    Check(worst < 1e-5, "D0 . (inv(D0) . W0) == W0 (a still hand leaves the cover where it was)");
    // the ring (the basket's rim: radius 0.358 m, normal z)
    const float c[3] = { 0, 0, 0 }, n[3] = { 0, 0, 1 };
    const float on[3] = { 0.358f, 0, 0 }, centre[3] = { 0, 0, 0 }, above[3] = { 0, 0.358f, 0.1f }, out[3] = { 0.558f, 0, 0 };
    Check(Near(lh::RingDist(c, n, 0.358f, on), 0.0, 1e-6), "RingDist on the rim = 0");
    Check(Near(lh::RingDist(c, n, 0.358f, centre), 0.358, 1e-6), "RingDist at the centre = the radius");
    Check(Near(lh::RingDist(c, n, 0.358f, above), 0.1, 1e-6), "RingDist 0.1 m above a rim point = 0.1");
    Check(Near(lh::RingDist(c, n, 0.358f, out), 0.2, 1e-6), "RingDist 0.2 m outside the rim = 0.2");
    // the let-go rule
    const float rest[3] = { 1.0f, 2.0f, 0.9f };
    const float lifted[3] = { 1.0f, 2.0f, 1.15f }, nudged[3] = { 1.05f, 2.0f, 0.95f };
    Check(lh::FreeLiftOpens(rest, lifted), "a 0.25 m lift opens");
    Check(!lh::FreeLiftOpens(rest, nudged), "a 0.07 m nudge goes back on");
    Check(!lh::FreeLiftOpens(rest, rest), "no movement goes back on");
}

// (p) trace 20261125_1610 v2 - the INSTANCE CHECK of a remembered lid slot, read from the object itself (GetID / IsDestroying /
//     IsDestroyed). The tester's sleep (evidence/tester_20261124_223836_lidhand): containers deleted while the PlayerDI was replaced,
//     a new one built into a freed block at the same address -> the slot must be DROPPED, never kept. deliberateMiss: a different id
//     where Keep is expected must not give Keep.
static void TestCheckSlot(bool deliberateMiss)
{
    using V = lh::SlotVerdict;
    if (deliberateMiss) {
        Check(lh::CheckSlot(true, 41, true, 42, false, false) != V::Keep, "deliberate miss: another id must not be kept");
        return;
    }
    Check(lh::CheckSlot(true, 41, false, 0, false, false) == V::Unanswered, "a read that faulted -> no verdict (today's behaviour)");
    Check(lh::CheckSlot(true, 41, true, 41, true, false) == V::Drop, "the same id but destroying / destroyed -> drop");
    Check(lh::CheckSlot(true, 41, true, 41, true, true) == V::Drop, "dying wins over a rekey");
    Check(lh::CheckSlot(false, -1, true, 57, false, false) == V::Rekey, "no stored id yet, live -> adopt");
    Check(lh::CheckSlot(true, 41, true, 9, false, true) == V::Rekey, "a renumber on the same level, live -> adopt the new id");
    Check(lh::CheckSlot(true, 41, true, 41, false, false) == V::Keep, "the same id, live -> keep");
    Check(lh::CheckSlot(true, 41, true, 42, false, false) == V::Drop, "another id at the same address -> drop (a new object)");
    Check(lh::CheckSlot(false, -1, true, 57, true, false) == V::Drop, "no stored id but dying -> drop");
}

// (q) the level / id-counter stamp (record+72 = level, level+3452 = the next id): a different level = flush, the counter going DOWN on
//     the same level = a renumber (CLevel +2AC350) = rekey. deliberateMiss: a drop on the same level must not read as "nothing".
static void TestStampChange(bool deliberateMiss)
{
    using S = lh::StampVerdict;
    const uintptr_t A = 0x1000, B = 0x2000;
    if (deliberateMiss) {
        Check(lh::StampChange(A, 900, A, 12) != S::None, "deliberate miss: the counter going down must not be ignored");
        return;
    }
    Check(lh::StampChange(0, 0, A, 900) == S::None, "no previous stamp -> nothing to compare");
    Check(lh::StampChange(A, 900, 0, 0) == S::None, "this stamp did not read -> nothing decided");
    Check(lh::StampChange(A, 900, A, 900) == S::None, "unchanged");
    Check(lh::StampChange(A, 900, A, 951) == S::None, "the counter rising (new objects) -> nothing");
    Check(lh::StampChange(A, 900, A, 12) == S::Rekey, "the counter going down on the same level -> rekey");
    Check(lh::StampChange(A, 900, B, 900) == S::Flush, "another level -> flush");
    Check(lh::StampChange(A, 900, B, 3) == S::Flush, "another level wins over the counter");
}

// (r) the HUD thread's identity-cache hit: the live candidate's id vs the id the entry was made with. deliberateMiss: two different
//     ids must read as stale.
static void TestCacheHitStale(bool deliberateMiss)
{
    if (deliberateMiss) {
        Check(lh::CacheHitStale(true, 41, true, 42), "deliberate miss: a different id must be stale");
        return;
    }
    Check(lh::CacheHitStale(true, 41, true, 42), "a different id -> stale");
    Check(!lh::CacheHitStale(true, 41, true, 41), "the same id -> not stale");
    Check(!lh::CacheHitStale(false, -1, true, 41), "an entry without an id -> not stale (today's behaviour)");
    Check(!lh::CacheHitStale(true, 41, false, 0), "the live id did not read -> not stale (today's behaviour)");
}

int main()
{
    TestFreeLift();
    TestClipStick(false);
    TestClipStick(true);
    TestClipEase();
    TestSegClosest();
    TestClipLocalAt(false);
    TestClipLocalAt(true);
    TestClipNearestT();
    TestClipTFromLocal();
    TestEdge("basket_opening", 0.651, false);
    TestEdge("chest_loot_big", 0.865, false);
    TestEdge("sedan_b_trunk_opening", 0.566, false);
    TestEdge("basket_opening", 0.651, true);
    // trace 20261121_1549 - the new rows (R from tools/lid_hinge_table.py's run, 2026-11-21)
    TestEdge("military_box_a", 0.878, false);
    TestEdge("military_box_b", 0.878, false);
    TestEdge("dlc_chest_loot_big", 0.865, false);
    TestEdge("first_aid_box_opening", 0.391, false);
    TestEdge("fridge_closed_opening (a)", 0.693, false);
    TestEdge("fridge_closed_opening (b)", 0.694, false);
    TestEdge("furniture_a_opening", 0.501, false);
    TestEdge("fridge_opening (d)", 0.736, false);
    TestEdge("steel_locker_opening", 0.339, false);
    TestEdge("toolbox_opening", 0.356, false);
    TestEdge("loot_wardrobe (left door)", 0.538, false);
    TestEdge("loot_wardrobe (right door)", 0.538, false);
    TestEdge("furniture_j_opening (left door)", 0.498, false);
    TestEdge("furniture_j_opening (right door)", 0.498, false);
    TestEdge("loot_wardrobe (left door)", 0.538, true);
    TestLeafOvershoot(false);
    TestLeafOvershoot(true);
    TestDistances();
    TestLidLocalMatchesClip(false);
    TestLidLocalMatchesClip(true);
    TestHandAngle();
    TestOpenPointAndClamp();
    TestNearest();
    TestItemRelease();
    TestItemReleaseByKind();
    TestCarriableRelease();
    TestCarriableGrabbable();
    TestCarriableLost();
    TestCarryLaunchVelocity();
    TestCarryMinors();
    TestCarryPrompt();
    TestInvDelta();
    TestInvDeltaMatch(false);
    TestInvDeltaMatch(true);
    TestPoseFind();
    TestCheckSlot(false);
    TestCheckSlot(true);
    TestStampChange(false);
    TestStampChange(true);
    TestCacheHitStale(false);
    TestCacheHitStale(true);
    std::printf("lidhand_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
