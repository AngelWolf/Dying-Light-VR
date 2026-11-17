// Standalone unit test for src/src/ovr2xr/menutap_logic.h - NOT part of the DLL build. Exit code = number of failures.
// The left menu button: tap = SELECT, 0.5 s hold = START (today's rules, unchanged), and the co-op teleport latch (INV-745).
#include "menutap_logic.h"

#include <cstdio>

using namespace dlvr::menutap;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// The caller's protocol: ask NeedsOffer first, feed the offer only then.
struct Rig {
    Machine m;
    bool    offer = false;
    float   prog = -1.0f;
    bool    menu = false;
    int     reads = 0;
    Out Poll(bool down, unsigned long long t)
    {
        bool of = false;
        float p = -1.0f;
        if (NeedsOffer(m, down, t)) { ++reads; of = offer; p = prog; }
        return Step(m, down, t, of, p, menu);
    }
};

static void tap_is_select_pulse()
{
    Rig r;
    Out o = r.Poll(true, 1000);
    CHECK(!o.touchpad && !o.start && r.m.st == State::Down);
    o = r.Poll(true, 1200);
    CHECK(!o.touchpad && !o.start);
    o = r.Poll(false, 1300);
    CHECK(o.touchpad && !o.start && r.m.st == State::SelectPulse);
    o = r.Poll(false, 1399);
    CHECK(o.touchpad);
    o = r.Poll(false, 1400);
    CHECK(!o.touchpad && r.m.st == State::Idle);
    CHECK(r.reads == 0);   // a tap never reads game memory
}

static void hold_without_offer_is_start_as_today()
{
    Rig r;
    r.Poll(true, 0);
    Out o = r.Poll(true, 499);
    CHECK(!o.start && r.reads == 0);
    o = r.Poll(true, 500);
    CHECK(o.start && !o.touchpad && r.m.st == State::StartHeld && r.reads == 1);
    o = r.Poll(true, 5000);
    CHECK(o.start && r.reads == 1);   // START held never reads again
    o = r.Poll(false, 5001);
    CHECK(!o.start && r.m.st == State::Idle);
}

static void hold_with_offer_latches_then_fires()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;
    r.Poll(true, 0);
    Out o = r.Poll(true, 500);
    CHECK(o.touchpad && !o.start && r.m.st == State::TeleportLatch);
    o = r.Poll(false, 600);   // let go - the latch holds it
    CHECK(o.touchpad);
    r.prog = 0.4f; o = r.Poll(false, 1200);
    CHECK(o.touchpad);
    r.prog = 0.95f; o = r.Poll(false, 1900);
    CHECK(o.touchpad);
    r.offer = false; r.prog = -1.0f;   // the game fired: float reset to -1.0
    o = r.Poll(false, 1950);
    CHECK(!o.touchpad && o.ended == EndWhy::OfferGone && r.m.st == State::WaitRelease);
    o = r.Poll(false, 1960);
    CHECK(r.m.st == State::Idle);
}

static void missed_minus_one_still_ends_on_the_drop()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;
    r.Poll(true, 0); r.Poll(true, 500); r.Poll(false, 600);
    r.prog = 0.9f; r.Poll(false, 1500);
    r.prog = 0.02f;   // the -1.0 frame was missed; the next refill reads small
    Out o = r.Poll(false, 1520);
    CHECK(!o.touchpad && o.ended == EndWhy::ProgressReset);
}

static void slow_drain_during_holdtime_does_not_end()
{
    // A progress left over from an earlier partial hold drains while the game's HoldTime(0.5) has not yet passed - small steps.
    Rig r;
    r.offer = true; r.prog = 0.30f;
    r.Poll(true, 0); r.Poll(true, 500);
    float p = 0.30f;
    for (unsigned long long t = 516; t < 1000; t += 16) {
        p -= 0.016f; if (p < 0.0f) { p = 0.0f; }
        r.prog = p;
        Out o = r.Poll(false, t);
        CHECK(o.touchpad && o.ended == EndWhy::None);
    }
}

static void game_menu_ends_the_latch_and_blocks_it()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;
    r.Poll(true, 0); r.Poll(true, 500); r.Poll(false, 600);
    r.menu = true;
    Out o = r.Poll(false, 700);
    CHECK(!o.touchpad && o.ended == EndWhy::GameMenu);
    // a hold that lands with a game menu up is START, as today
    Rig q;
    q.offer = true; q.prog = 0.0f; q.menu = true;
    q.Poll(true, 0);
    o = q.Poll(true, 500);
    CHECK(o.start && !o.touchpad);
}

static void new_press_cancels_and_never_becomes_start()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;
    r.Poll(true, 0); r.Poll(true, 500);
    r.Poll(false, 600);
    Out o = r.Poll(true, 800);   // pressed again
    CHECK(!o.touchpad && o.ended == EndWhy::Cancelled && r.m.st == State::WaitRelease);
    o = r.Poll(true, 2000);      // still held long after - no START, no SELECT
    CHECK(!o.touchpad && !o.start);
    o = r.Poll(false, 2100);
    CHECK(!o.touchpad && !o.start && r.m.st == State::Idle);
}

static void held_through_the_latch_is_not_a_cancel()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;
    r.Poll(true, 0); r.Poll(true, 500);
    Out o = r.Poll(true, 900);   // never let go
    CHECK(o.touchpad && o.ended == EndWhy::None);
}

static void blocked_offer_ends_at_the_cap()
{
    Rig r;
    r.offer = true; r.prog = 0.0f;   // blocked: on offer, pinned at 0.0
    r.Poll(true, 0); r.Poll(true, 500); r.Poll(false, 600);
    Out o = r.Poll(false, 3499);
    CHECK(o.touchpad);
    o = r.Poll(false, 3500);
    CHECK(!o.touchpad && o.ended == EndWhy::Cap);
}

static void offer_from_progress()
{
    CHECK(!OfferFromProgress(-1.0f));
    CHECK(OfferFromProgress(0.0f));
    CHECK(OfferFromProgress(1.0f));
    CHECK(OfferFromProgress(0.5f));
    CHECK(!OfferFromProgress(1.5f));
    CHECK(!OfferFromProgress(-0.01f));
    float z = 0.0f;
    CHECK(!OfferFromProgress(z / z));   // NaN
}

static_assert(kHoldMs == 500, "D-175: the hold stays under VD's ~1 s menu hold");
static_assert(kPulseMs == 100, "SELECT's pulse (INV-299)");
static_assert(kLatchCapMs == 3000, "the latch cap (INV-745: fill 1.0 s + 0 or 0.5 s)");

static void pinned_values() {}

int main()
{
    tap_is_select_pulse();
    hold_without_offer_is_start_as_today();
    hold_with_offer_latches_then_fires();
    missed_minus_one_still_ends_on_the_drop();
    slow_drain_during_holdtime_does_not_end();
    game_menu_ends_the_latch_and_blocks_it();
    new_press_cancels_and_never_becomes_start();
    held_through_the_latch_is_not_a_cancel();
    blocked_offer_ends_at_the_cap();
    offer_from_progress();
    pinned_values();
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
