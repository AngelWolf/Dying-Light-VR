// Standalone unit test for src_beta/src/ovr2xr/heldmesh_logic.h - NOT part of the DLL build.
// Trace docs/traces/20261119_1840_weapon_stage_rekey_c1.md. Exit code = number of failures.
#include "heldmesh_logic.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using namespace heldmesh;

static const unsigned long long kClean = KeyOf("wn_machete_a_hq.msh");
static const unsigned long long kDmA   = KeyOf("wn_machete_a_dm_a_hq.msh");
static const unsigned long long kDmB   = KeyOf("wn_machete_a_dm_b_hq.msh");

static void keys()
{
    CHECK(kClean != kDmA && kDmA != kDmB && kClean != kDmB);
    CHECK(KeyOf(nullptr) == kUnread);
    CHECK(KeyOf("") == kUnread);
    CHECK(KeyOf("wn_machete_a_hq.msh") == kClean);            // deterministic
    CHECK(kClean != 0 && kDmA != 0);                          // 0 means "never stepped"
}

// A weapon whose mesh never changes: one resolve at the item change, then never again - today's behaviour exactly.
static void unchanged_mesh_is_todays_path()
{
    Track t;
    CHECK(Step(t, kClean, true));
    for (int i = 0; i < 100; ++i) { CHECK(!Step(t, kClean, false)); }
    CHECK(WriterMayWrite(t, kClean));
}

// The in-place stage swap (SP2 16:21:35): re-resolve on the swap update AND the next; the writer refuses on the swap update.
static void in_place_swap_resolves_twice()
{
    Track t;
    Step(t, kClean, true);
    CHECK(Step(t, kDmA, false));          // the name changed on the same item
    CHECK(t.again);
    CHECK(!WriterMayWrite(t, kDmA));      // settling: nothing written this update
    CHECK(Step(t, kDmA, false));          // the second pass
    CHECK(!t.again);
    CHECK(WriterMayWrite(t, kDmA));
    CHECK(!Step(t, kDmA, false));         // then quiet
}

// The writer sees a key the list was not built on (the swap landed between Capture and the writer): refused.
static void writer_refuses_a_mesh_the_list_was_not_built_on()
{
    Track t;
    Step(t, kClean, true);
    CHECK(WriterMayWrite(t, kClean));
    CHECK(!WriterMayWrite(t, kDmA));
    Track never;
    CHECK(!WriterMayWrite(never, kClean));   // never stepped
}

// Two swaps on consecutive updates (dm_a then dm_b): each restarts the double pass.
static void back_to_back_swaps()
{
    Track t;
    Step(t, kClean, true);
    CHECK(Step(t, kDmA, false));
    CHECK(Step(t, kDmB, false));
    CHECK(t.again);
    CHECK(Step(t, kDmB, false));
    CHECK(!Step(t, kDmB, false));
}

// A repair back to clean (dm_b -> clean): same rule.
static void repair_back_to_clean()
{
    Track t;
    Step(t, kDmB, true);
    CHECK(Step(t, kClean, false));
    CHECK(Step(t, kClean, false));
    CHECK(!Step(t, kClean, false));
    CHECK(WriterMayWrite(t, kClean));
}

// A new item while settling: the item path wins, one pass, no `again`.
static void item_change_clears_settling()
{
    Track t;
    Step(t, kClean, true);
    Step(t, kDmA, false);
    CHECK(t.again);
    CHECK(Step(t, kClean, true));
    CHECK(!t.again);
    CHECK(!Step(t, kClean, false));
}

// An unreadable name stays one constant key: no spurious re-resolves.
static void unreadable_name_is_stable()
{
    Track t;
    Step(t, KeyOf(nullptr), true);
    for (int i = 0; i < 10; ++i) { CHECK(!Step(t, KeyOf(""), false)); }
}

int main()
{
    keys();
    unchanged_mesh_is_todays_path();
    in_place_swap_resolves_twice();
    writer_refuses_a_mesh_the_list_was_not_built_on();
    back_to_back_swaps();
    repair_back_to_clean();
    item_change_clears_settling();
    unreadable_name_is_stable();
    std::printf("heldmesh_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
