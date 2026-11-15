// Standalone unit test for src/src/ovr2xr/breakout_logic.h - NOT part of the DLL build.
// Physical breakout: docs/superpowers/specs/2026-10-24-physical-breakout-design.md, plan Task 2. Exit code = failures.
#include "breakout_logic.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using dlvr::breakout::Decide;
using dlvr::breakout::Expired;
using dlvr::breakout::GrabState;
using dlvr::breakout::Kind;
using dlvr::breakout::ShoveDetector;
using dlvr::breakout::ShoveInfo;
using dlvr::breakout::ShoveParams;

// OpenXR tracking space: y up, -z forward. Head at the origin, hands 0.3 m in front of it at chest height.
static const float kHead[3] = { 0.0f, 1.6f, 0.0f };
static const double kDt = 0.002;   // the scePad poll runs ~570/s

struct Move {
    double t0, t1;    // seconds the hand moves
    float v[3];       // m/s while moving
};

// Runs the detector from 0 to `endS`; each hand starts at its rest point and moves by `l` / `r`. `lostL0..lostL1` = the left
// controller reports no pose. Returns the number of shoves.
static int Run(double endS, const Move* l, int nl, const Move* r, int nr, double lostL0 = -1.0, double lostL1 = -1.0,
               ShoveInfo* last = nullptr)
{
    ShoveDetector d;
    ShoveParams p;
    float pl[3] = { -0.2f, 1.2f, -0.3f }, pr[3] = { 0.2f, 1.2f, -0.3f };
    int n = 0;
    for (double t = 0.0; t <= endS; t += kDt) {
        for (int i = 0; i < nl; ++i) {
            if (t > l[i].t0 && t <= l[i].t1) { for (int k = 0; k < 3; ++k) { pl[k] += l[i].v[k] * static_cast<float>(kDt); } }
        }
        for (int i = 0; i < nr; ++i) {
            if (t > r[i].t0 && t <= r[i].t1) { for (int k = 0; k < 3; ++k) { pr[k] += r[i].v[k] * static_cast<float>(kDt); } }
        }
        const bool haveL = !(t >= lostL0 && t <= lostL1);
        ShoveInfo info{};
        if (d.Push(t, pl, haveL, pr, true, kHead, p, &info)) {
            ++n;
            if (last) { *last = info; }
        }
    }
    return n;
}

static void push_forward_both_hands_shoves()
{
    const Move m[] = { { 0.2, 0.35, { 0.0f, 0.0f, -2.0f } } };
    ShoveInfo info{};
    CHECK(Run(0.8, m, 1, m, 1, -1.0, -1.0, &info) == 1);
    CHECK(info.speedL >= 1.6f && info.speedR >= 1.6f);
    CHECK(info.angleDeg < 5.0f);
}

static void one_hand_never_shoves()
{
    const Move m[] = { { 0.2, 0.35, { 0.0f, 0.0f, -3.0f } } };
    CHECK(Run(0.8, nullptr, 0, m, 1) == 0);
}

static void below_threshold_no_shove()
{
    const Move m[] = { { 0.2, 0.5, { 0.0f, 0.0f, -1.4f } } };
    CHECK(Run(0.8, m, 1, m, 1) == 0);
}

static void hands_out_of_sync_no_shove()
{
    const Move r[] = { { 0.2, 0.35, { 0.0f, 0.0f, -2.0f } } };
    const Move l[] = { { 0.45, 0.6, { 0.0f, 0.0f, -2.0f } } };
    CHECK(Run(1.0, l, 1, r, 1) == 0);
}

static void diverging_hands_no_shove()
{
    const Move l[] = { { 0.2, 0.35, { -2.0f, 0.0f, 0.0f } } };
    const Move r[] = { { 0.2, 0.35, { 2.0f, 0.0f, 0.0f } } };
    CHECK(Run(0.8, l, 1, r, 1) == 0);
}

static void pull_toward_head_rejected()
{
    const Move m[] = { { 0.2, 0.3, { 0.0f, 0.0f, 2.0f } } };   // +z = back toward the head
    CHECK(Run(0.8, m, 1, m, 1) == 0);
}

static void throw_aside_accepted()
{
    const Move m[] = { { 0.2, 0.35, { 2.0f, 0.0f, 0.0f } } };   // both to the right, across the body
    CHECK(Run(0.8, m, 1, m, 1) == 1);
}

static void long_push_counts_once()
{
    const Move m[] = { { 0.2, 1.2, { 0.0f, 0.0f, -2.0f } } };
    CHECK(Run(1.6, m, 1, m, 1) == 1);
}

static void rearm_after_slowdown()
{
    const Move m[] = { { 0.2, 0.35, { 0.0f, 0.0f, -2.0f } }, { 0.65, 0.8, { 0.0f, 0.0f, 2.0f } }, { 1.1, 1.25, { 0.0f, 0.0f, -2.0f } } };
    // push, rest, bring the hands back (a pull - never counted), rest, push again: two shoves
    CHECK(Run(1.6, m, 3, m, 3) == 2);
}

static void tracking_lost_hand_no_shove()
{
    const Move m[] = { { 0.2, 0.35, { 0.0f, 0.0f, -2.0f } } };
    CHECK(Run(0.8, m, 1, m, 1, 0.15, 0.6) == 0);   // the left controller has no pose for the whole push
}

static GrabState G(bool grabbed, int type, int phase, float elapsed = 1.0f, float late = 0.3f, bool blocked = false)
{
    GrabState g{};
    g.grabbed = grabbed;
    g.type = type;
    g.phase = phase;
    g.elapsedS = elapsed;
    g.lateBeginS = late;
    g.winBlocked = blocked;
    return g;
}

static void not_grabbed_ignores()
{
    CHECK(Decide(G(false, 0, 5)) == Kind::Ignore);
    CHECK(Decide(G(false, 5, 5)) == Kind::Ignore);
}

static void volatile_taps_any_phase()
{
    CHECK(Decide(G(true, 5, 0)) == Kind::Tap);
    CHECK(Decide(G(true, 5, 5)) == Kind::Tap);
    CHECK(Decide(G(true, 5, 10)) == Kind::Tap);
}

static void zombie_fight_phase8()
{
    for (int type = 0; type <= 3; ++type) {
        CHECK(Decide(G(true, type, 5)) == Kind::Phase8);
        CHECK(Decide(G(true, type, 10)) == Kind::Phase8);
    }
}

static void zombie_early_begin_holds()
{
    CHECK(Decide(G(true, 0, 2, 0.1f, 0.3f)) == Kind::Hold);
}

static void zombie_late_begin_phase8()
{
    CHECK(Decide(G(true, 0, 2, 0.3f, 0.3f)) == Kind::Phase8);
}

// breakout_run1: +177 stays set through the whole fight phase (17 of 17 shoves held) while the game's own KICK sets phase 8 there
// without reading it (INV-618, +D21180) - the block gates the tap bar only, never the zombie phase 8.
static void win_block_does_not_hold_phase8()
{
    CHECK(Decide(G(true, 1, 5, 1.0f, 0.3f, true)) == Kind::Phase8);
    CHECK(Decide(G(true, 2, 10, 1.0f, 0.3f, true)) == Kind::Phase8);
    CHECK(Decide(G(true, 0, 2, 0.4f, 0.3f, true)) == Kind::Phase8);
    CHECK(Decide(G(true, 0, 2, 0.1f, 0.3f, true)) == Kind::Hold);   // early begin still waits, block or not
}

static void finished_phase_ignores()
{
    const int phases[] = { 6, 7, 8, 9, 16 };
    for (int ph : phases) { CHECK(Decide(G(true, 0, ph)) == Kind::Ignore); }
}

static void leg_grab_ignored()
{
    CHECK(Decide(G(true, 4, 5)) == Kind::Ignore);
}

static void other_types_ignored()
{
    const int types[] = { 6, 7, 12, -1 };
    for (int ty : types) { CHECK(Decide(G(true, ty, 5)) == Kind::Ignore); }
}

// Final review Important 1: a grab is identified by its enemy, so a grab that ends and the next that starts with no free frame
// between them is still two grabs (the type is re-read, a held shove from the first is dropped).
static void rise_is_a_new_grab()
{
    const dlvr::breakout::GrabEdge e = dlvr::breakout::Edges(false, 0, true, 0x1000);
    CHECK(e.started && !e.ended);
}

static void fall_ends_grab()
{
    const dlvr::breakout::GrabEdge e = dlvr::breakout::Edges(true, 0x1000, false, 0);
    CHECK(e.ended && !e.started);
}

static void same_enemy_same_grab()
{
    const dlvr::breakout::GrabEdge e = dlvr::breakout::Edges(true, 0x1000, true, 0x1000);
    CHECK(!e.started && !e.ended);
}

static void enemy_change_is_a_new_grab()
{
    const dlvr::breakout::GrabEdge e = dlvr::breakout::Edges(true, 0x1000, true, 0x2000);
    CHECK(e.ended && e.started);
}

static void expired_shove_dropped()
{
    CHECK(Expired(10.0, 11.01));
    CHECK(!Expired(10.0, 10.99));
}

// The melee gate (Jay 2026-10-27: an attack as a zombie grabs should count): open for the first kMeleeOpenMs of a grab, closed
// after - shoves come later (first detected median 759 ms, 5 of 200 before 400 ms). Not grabbed = open.
static void melee_gate_opens_for_the_start_of_a_grab()
{
    using dlvr::breakout::MeleeGateClosed;
    using dlvr::breakout::kMeleeOpenMs;
    static_assert(kMeleeOpenMs == 400, "the open window is 400 ms (Jay 2026-10-27)");
    CHECK(!MeleeGateClosed(false, 0, 99999));            // not grabbed: never closed
    CHECK(!MeleeGateClosed(true, 10000, 10000));         // the grab's first frame: open
    CHECK(!MeleeGateClosed(true, 10000, 10399));         // just inside the window
    CHECK(MeleeGateClosed(true, 10000, 10400));          // the window's end: closed
    CHECK(MeleeGateClosed(true, 10000, 13000));          // the shove's time: closed
    CHECK(!MeleeGateClosed(true, 0, 5000));              // no start stamp (0): open, never a stale close
    CHECK(MeleeGateClosed(true, 10000, 9000) == false);  // a clock that reads before the stamp: open
}

int main()
{
    melee_gate_opens_for_the_start_of_a_grab();
    push_forward_both_hands_shoves();
    one_hand_never_shoves();
    below_threshold_no_shove();
    hands_out_of_sync_no_shove();
    diverging_hands_no_shove();
    pull_toward_head_rejected();
    throw_aside_accepted();
    long_push_counts_once();
    rearm_after_slowdown();
    tracking_lost_hand_no_shove();
    not_grabbed_ignores();
    volatile_taps_any_phase();
    zombie_fight_phase8();
    zombie_early_begin_holds();
    zombie_late_begin_phase8();
    win_block_does_not_hold_phase8();
    finished_phase_ignores();
    leg_grab_ignored();
    other_types_ignored();
    expired_shove_dropped();
    rise_is_a_new_grab();
    fall_ends_grab();
    same_enemy_same_grab();
    enemy_change_is_a_new_grab();
    if (g_fail == 0) { std::printf("ALL PASS\n"); }
    return g_fail;
}
