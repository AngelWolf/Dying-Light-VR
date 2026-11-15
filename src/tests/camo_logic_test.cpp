// Standalone unit test for src/src/ovr2xr/camo_logic.h - NOT part of the DLL build.
// The camouflage gesture: docs/superpowers/specs/2026-11-07-camouflage-gesture-design.md, plan Task 1. Exit code = failures.
#include "camo_logic.h"

#include <cmath>
#include <cstdio>
#include <string_view>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using camo::Decide;
using camo::Facts;
using camo::HeadLocal;
using camo::LimbOwners;
using camo::Outcome;
using camo::WipeParams;
using camo::WipeState;
using camo::WipeStep;

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static const WipeParams kP = { 0.25f, 0.15f, 0.12f, 1.0f };
static const float kIdQ[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

static const float kRightX[3] = { 1.0f, 0.0f, 0.0f };

// Sweeps x from x0 to x1 at (y, z) over `durS` in 10 ms samples starting at t0, with a still head at the origin facing -z (so the
// head frame == tracking space); returns how many times WipeStep fired and the index of the first fire (-1 = none).
static int Sweep(WipeState& s, float x0, float x1, float y, float z, double t0, double durS, int* firstAt)
{
    const int n = static_cast<int>(durS / 0.01 + 0.5);
    int fires = 0;
    *firstAt = -1;
    for (int i = 0; i <= n; ++i) {
        const float x = x0 + (x1 - x0) * (static_cast<float>(i) / static_cast<float>(n));
        const float p[3] = { x, y, z };
        if (WipeStep(s, kP, p, p, kRightX, t0 + 0.01 * i)) {
            if (*firstAt < 0) { *firstAt = i; }
            ++fires;
        }
    }
    return fires;
}

static void head_local_identity()
{
    const float head[3] = { 1.0f, 2.0f, 3.0f };
    const float p[3] = { 1.1f, 2.0f, 2.8f };
    float o[3] = {};
    CHECK(HeadLocal(head, kIdQ, p, o));
    CHECK(Near(o[0], 0.1f, 1e-5f));
    CHECK(Near(o[1], 0.0f, 1e-5f));
    CHECK(Near(o[2], -0.2f, 1e-5f));
}

static void head_local_yaw90()
{
    // 90 deg about +y: the head's forward (-z) points along world -x.
    const float s = std::sqrt(0.5f);
    const float q[4] = { 0.0f, s, 0.0f, s };
    const float head[3] = { 0.0f, 1.6f, 0.0f };
    const float p[3] = { -1.0f, 1.6f, 0.0f };
    float o[3] = {};
    CHECK(HeadLocal(head, q, p, o));
    CHECK(Near(o[0], 0.0f, 1e-5f));
    CHECK(Near(o[1], 0.0f, 1e-5f));
    CHECK(Near(o[2], -1.0f, 1e-5f));
}

static void head_local_rejects_bad_quat()
{
    const float head[3] = { 0.0f, 0.0f, 0.0f };
    const float q[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float p[3] = { 0.1f, 0.0f, 0.0f };
    float o[3] = {};
    CHECK(!HeadLocal(head, q, p, o));
}

static void wipe_across_fires()
{
    WipeState s{};
    int first = -1;
    CHECK(Sweep(s, -0.10f, 0.10f, 0.0f, -0.12f, 0.0, 0.2, &first) == 1);
    // 20 samples over 0.20 m = 0.01 m each; travel 0.12 is first reached at sample 12.
    CHECK(first == 12);
}

static void slow_pass_below_sweep_is_not_a_wipe()
{
    WipeState s{};
    int first = -1;
    // 0.20 m over 2.5 s: the 0.12 m of travel is reached at 1.5 s, past the 1.0 s window.
    CHECK(Sweep(s, -0.10f, 0.10f, 0.0f, -0.12f, 0.0, 2.5, &first) == 0);
}

static void touch_is_not_a_wipe()
{
    WipeState s{};
    int fires = 0;
    for (int i = 0; i <= 200; ++i) {
        const float j = (i % 2) ? 0.02f : -0.02f;
        const float p[3] = { j, -j, -0.1f };
        fires += WipeStep(s, kP, p, p, kRightX, 0.01 * i) ? 1 : 0;
    }
    CHECK(fires == 0);
}

// Final review I1: a corpse held still 0.2 m in front of the face while the HEAD turns 40 deg (yaw -20 -> +20 over 0.3 s) must not
// wipe - its head-frame x sweeps ~0.15 m, but the hand does not move.
static void head_turn_with_still_hand_is_not_a_wipe()
{
    WipeState s{};
    const float head[3] = { 0.0f, 1.6f, 0.0f };
    const float hand[3] = { 0.0f, 1.6f, -0.2f };   // tracking space, never moves
    int fires = 0;
    float spanSeen = 0.0f, minX = 1.0f, maxX = -1.0f;
    for (int i = 0; i <= 30; ++i) {
        const float a = (-20.0f + 40.0f * (static_cast<float>(i) / 30.0f)) * 3.14159265f / 180.0f;
        const float q[4] = { 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
        float hl[3] = {}, right[3] = {};
        CHECK(HeadLocal(head, q, hand, hl));
        camo::HeadRight(q, right);
        if (hl[0] < minX) { minX = hl[0]; }
        if (hl[0] > maxX) { maxX = hl[0]; }
        fires += WipeStep(s, kP, hl, hand, right, 0.01 * i) ? 1 : 0;
    }
    spanSeen = maxX - minX;
    CHECK(spanSeen > 0.12f);   // the old head-frame measure would have fired
    CHECK(fires == 0);
}

// The same head turn while the hand really does cross the face (it moves 0.2 m along the head's right) still wipes.
static void hand_sweep_during_head_turn_wipes()
{
    WipeState s{};
    const float head[3] = { 0.0f, 1.6f, 0.0f };
    int fires = 0;
    for (int i = 0; i <= 20; ++i) {
        const float a = (5.0f * (static_cast<float>(i) / 20.0f)) * 3.14159265f / 180.0f;
        const float q[4] = { 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f) };
        const float hand[3] = { -0.10f + 0.20f * (static_cast<float>(i) / 20.0f), 1.6f, -0.15f };
        float hl[3] = {}, right[3] = {};
        CHECK(HeadLocal(head, q, hand, hl));
        camo::HeadRight(q, right);
        fires += WipeStep(s, kP, hl, hand, right, 0.01 * i) ? 1 : 0;
    }
    CHECK(fires == 1);
}

static void head_right_yaw90()
{
    // 90 deg about +y: the head's right (+x) points along world -z.
    const float s = std::sqrt(0.5f);
    const float q[4] = { 0.0f, s, 0.0f, s };
    float r[3] = {};
    camo::HeadRight(q, r);
    CHECK(Near(r[0], 0.0f, 1e-5f));
    CHECK(Near(r[1], 0.0f, 1e-5f));
    CHECK(Near(r[2], -1.0f, 1e-5f));
}

static void depth_outside_zone_is_not_a_wipe()
{
    WipeState a{};
    WipeState b{};
    int first = -1;
    CHECK(Sweep(a, -0.10f, 0.10f, 0.0f, -0.40f, 0.0, 0.2, &first) == 0);
    CHECK(Sweep(b, -0.10f, 0.10f, 0.0f, 0.10f, 0.0, 0.2, &first) == 0);
}

static void one_wipe_per_entry()
{
    WipeState s{};
    int first = -1;
    CHECK(Sweep(s, -0.10f, 0.10f, 0.0f, -0.12f, 0.0, 0.2, &first) == 1);
    CHECK(Sweep(s, 0.10f, -0.10f, 0.0f, -0.12f, 0.21, 0.2, &first) == 0);   // back again, never left the zone
    const float out[3] = { 0.0f, 0.30f, -0.12f };
    CHECK(!WipeStep(s, kP, out, out, kRightX, 0.45));                        // leave (above the zone)
    CHECK(Sweep(s, -0.10f, 0.10f, 0.0f, -0.12f, 0.50, 0.2, &first) == 1);   // re-enter + sweep
}

static Facts Good()
{
    Facts f{};
    f.holding = true;
    f.readable = true;
    f.infected = true;
    f.used = false;
    f.ready = true;
    f.modeOk = true;
    f.durationS = 30.0f;
    return f;
}

static void decide_order()
{
    CHECK(Decide(Good()) == Outcome::Start);
    Facts f = Good();
    f.durationS = 0.0f;
    CHECK(Decide(f) == Outcome::NoDuration);
    f = Good();
    f.modeOk = false;
    CHECK(Decide(f) == Outcome::ModeBlocked);
    f = Good();
    f.ready = false;
    CHECK(Decide(f) == Outcome::NotReady);
    f.modeOk = false;   // not ready is reported before the mode test
    CHECK(Decide(f) == Outcome::NotReady);
    f = Good();
    f.used = true;
    f.ready = false;    // used up is reported before not ready
    CHECK(Decide(f) == Outcome::UsedUp);
    f = Good();
    f.infected = false;
    CHECK(Decide(f) == Outcome::NotInfected);
    f = Good();
    f.readable = false;
    CHECK(Decide(f) == Outcome::Unreadable);
    Facts bad{};   // everything false / zero
    CHECK(Decide(bad) == Outcome::NotHolding);
    CHECK(std::string_view(camo::OutcomeName(Outcome::Start)) == "started");
    CHECK(std::string_view(camo::OutcomeName(Outcome::UsedUp)) == "used up");
    CHECK(std::string_view(camo::OutcomeName(Outcome::NotReady)) == "not ready");
}

static void second_start_same_corpse_refused()
{
    Facts f = Good();
    CHECK(Decide(f) == Outcome::Start);
    f.used = true;   // the caller cleared the corpse's once-flag on the first start
    CHECK(Decide(f) == Outcome::UsedUp);
}

static void limb_owners_record_find()
{
    LimbOwners m;
    int l1 = 0, a1 = 0;
    m.Record(&l1, 901, &a1, 77, true);
    const void* owner = nullptr;
    int id = 0, limbId = 0;
    bool inf = false, used = true;
    CHECK(m.Find(&l1, &limbId, &owner, &id, &inf, &used));
    CHECK(limbId == 901);   // final review I2: the limb's OWN id, so a later object at the same address is told apart
    CHECK(owner == &a1);
    CHECK(id == 77);
    CHECK(inf);
    CHECK(!used);
    m.Forget(&l1);
    CHECK(!m.Find(&l1, &limbId, &owner, &id, &inf, &used));
    CHECK(m.Count() == 0);
}

static void limb_own_use_when_owner_gone()
{
    LimbOwners m;
    int l1 = 0, a1 = 0;
    m.Record(&l1, 3, &a1, 5, true);
    m.MarkOwnUsed(&l1);
    const void* owner = nullptr;
    int id = 0, limbId = 0;
    bool inf = false, used = false;
    CHECK(m.Find(&l1, &limbId, &owner, &id, &inf, &used));
    CHECK(used);
}

static void limb_owners_evict_oldest()
{
    static int limbs[129];
    int a = 0;
    LimbOwners m;
    for (int i = 0; i < 129; ++i) { m.Record(&limbs[i], 1000 + i, &a, i, false); }
    const void* owner = nullptr;
    int id = 0, limbId = 0;
    bool inf = true, used = true;
    CHECK(!m.Find(&limbs[0], &limbId, &owner, &id, &inf, &used));
    CHECK(m.Find(&limbs[128], &limbId, &owner, &id, &inf, &used));
    CHECK(id == 128);
    CHECK(limbId == 1128);
    CHECK(m.Count() == 128);
}

static void limb_owners_rerecord_updates()
{
    LimbOwners m;
    int l1 = 0, a1 = 0, a2 = 0;
    m.Record(&l1, 10, &a1, 1, false);
    m.MarkOwnUsed(&l1);
    m.Record(&l1, 20, &a2, 2, true);   // the same address reused by a new limb: replaced, not duplicated, its use fresh
    const void* owner = nullptr;
    int id = 0, limbId = 0;
    bool inf = false, used = true;
    CHECK(m.Find(&l1, &limbId, &owner, &id, &inf, &used));
    CHECK(limbId == 20);
    CHECK(owner == &a2);
    CHECK(id == 2);
    CHECK(inf);
    CHECK(!used);
    CHECK(m.Count() == 1);
}

int main()
{
    head_local_identity();
    head_local_yaw90();
    head_local_rejects_bad_quat();
    wipe_across_fires();
    slow_pass_below_sweep_is_not_a_wipe();
    touch_is_not_a_wipe();
    head_turn_with_still_hand_is_not_a_wipe();
    hand_sweep_during_head_turn_wipes();
    head_right_yaw90();
    depth_outside_zone_is_not_a_wipe();
    one_wipe_per_entry();
    decide_order();
    second_start_same_corpse_refused();
    limb_owners_record_find();
    limb_own_use_when_owner_gone();
    limb_owners_evict_oldest();
    limb_owners_rerecord_updates();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
