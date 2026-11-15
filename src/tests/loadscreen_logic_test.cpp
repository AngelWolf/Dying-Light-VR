// Standalone unit test for src/src/ovr2xr/loadscreen_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-20-loading-screen-void-design.md §4 / §5. Exit code = number of failures.
#include "loadscreen_logic.h"

#include <cstdio>

using namespace dlvr::loadscreen;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void edges()
{
    State s;
    CHECK(Step(s, true) == Edge::Start);
    CHECK(s.loading);
    CHECK(Step(s, true) == Edge::None);
    CHECK(Step(s, false) == Edge::End);
    CHECK(!s.loading);
    CHECK(Step(s, false) == Edge::None);
}

static void start_clears_pictures()
{
    State s;
    s.havePic[0] = s.havePic[1] = true;
    s.latched = true;
    CHECK(Step(s, true) == Edge::Start);
    CHECK(!s.havePic[0] && !s.havePic[1]);
    CHECK(!s.latched);
}

// Two background-render screens back to back: the second must latch again (Review Focus 2).
static void restart_relatches()
{
    State s;
    Step(s, true);
    s.latched = true;
    Step(s, false);
    CHECK(!s.latched);
    Step(s, true);
    CHECK(!s.latched);
}

static void decide_off_when_not_loading()
{
    State s;
    s.latched = true;
    s.havePic[0] = s.havePic[1] = true;
    const Plan p = Decide(s, false);
    CHECK(!p.skipProjection && !p.quad[0] && !p.quad[1]);
}

// No head pose yet (no latch): the void is up, the quads wait (Review Focus 1).
static void start_without_views_waits()
{
    State s;
    Step(s, true);
    s.havePic[0] = s.havePic[1] = true;
    const Plan p = Decide(s, false);
    CHECK(p.skipProjection);
    CHECK(!p.quad[0] && !p.quad[1]);
}

static void quads_need_their_own_picture()
{
    State s;
    Step(s, true);
    s.latched = true;
    s.havePic[0] = true;
    const Plan p = Decide(s, false);
    CHECK(p.skipProjection && p.quad[0] && !p.quad[1]);
}

// A fault restores the game image at once and for the rest of the session (Review Focus 3).
static void fault_restores_projection()
{
    State s;
    Step(s, true);
    s.latched = true;
    s.havePic[0] = s.havePic[1] = true;
    s.fault = true;
    Plan p = Decide(s, false);
    CHECK(!p.skipProjection && !p.quad[0] && !p.quad[1]);
    Step(s, false);
    Step(s, true);
    CHECK(s.fault);
    p = Decide(s, false);
    CHECK(!p.skipProjection);
}

// The calibration screen already skips the game image and owns the void.
static void calib_void_wins()
{
    State s;
    Step(s, true);
    s.latched = true;
    s.havePic[0] = s.havePic[1] = true;
    const Plan p = Decide(s, true);
    CHECK(!p.skipProjection && !p.quad[0] && !p.quad[1]);
}

// Amendment 1: the void SEQUENCE. Inputs: the engine flag, the loading module's visible screens (-1 unreadable), the HUD
// (uipin::HudShowing: 1 showing), a menu (uipin::MenuState: > 0 showing).
static void flag_alone_is_a_void()
{
    State s;
    CHECK(SequenceActive(s, true, 0, 0, 0));
    CHECK(SequenceActive(s, true, -1, -1, -1));   // the flag needs no level read
}

static void module_screen_extends_void()
{
    State s;
    SequenceActive(s, true, 0, 0, 0);
    CHECK(SequenceActive(s, false, 1, 0, 0));   // "Press A to continue" / the gap: the module still shows a screen
    CHECK(SequenceActive(s, false, 2, 0, 0));
}

static void module_alone_never_starts()
{
    State s;
    CHECK(!SequenceActive(s, false, 1, 0, 0));
}

static void hud_ends_extension()
{
    State s;
    SequenceActive(s, true, 1, 0, 0);
    CHECK(!SequenceActive(s, false, 1, 1, 0));
    CHECK(!SequenceActive(s, false, 1, 0, 0));   // ended: the module alone does not bring it back
}

static void menu_ends_extension()
{
    State s;
    SequenceActive(s, true, 1, 0, 0);
    CHECK(!SequenceActive(s, false, 1, 0, 1));
}

static void module_gone_ends_extension()
{
    State s;
    SequenceActive(s, true, 1, 0, 0);
    CHECK(!SequenceActive(s, false, 0, 0, 0));
}

static void unreadable_ends_extension()
{
    State s;
    SequenceActive(s, true, 1, 0, 0);
    CHECK(!SequenceActive(s, false, -1, 0, 0));
}

static void flag_restarts_sequence()
{
    State s;
    SequenceActive(s, true, 0, 0, 0);
    SequenceActive(s, false, 0, 0, 0);            // ended
    CHECK(SequenceActive(s, true, 0, 0, 0));      // the next flag starts a new one
    CHECK(SequenceActive(s, false, 1, 0, 0));
}

// The HUD on with the flag still set (a fast-travel fade): the flag wins - the engine draws its background path.
static void flag_wins_over_hud()
{
    State s;
    CHECK(SequenceActive(s, true, 0, 1, 1));
}

// The HUD box sits inside each eye's own viewport (Review Focus 5).
static void box_inside_offset_viewport()
{
    const dlvr::uipanel::EyeTan t = { 1.3764f, 1.3764f, 1.4747f, 1.4747f };   // Jay's Quest 3 (INV-735)
    dlvr::uipanel::Rect c{}, r0{}, r1{}, r2{};
    CHECK(dlvr::uipanel::UiCrop(2688, 2880, t, &c));
    CHECK(EyeBox(2688, 0, 2688, 2880, t, &r1));
    CHECK(r1.x == c.x + 2688 && r1.y == c.y && r1.w == c.w && r1.h == c.h);
    CHECK(EyeBox(0, 0, 2688, 2880, t, &r0));
    CHECK(r0.x == c.x && r0.y == c.y && r0.w == c.w && r0.h == c.h);
    CHECK(EyeBox(10, 20, 2688, 2880, t, &r2));
    CHECK(r2.x == c.x + 10 && r2.y == c.y + 20);
    const dlvr::uipanel::EyeTan none = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(!EyeBox(0, 0, 2688, 2880, none, &r0));          // no field of view yet: refused, not guessed
}

// Final review I1: a usable projection is not a valid head pose - with no located pose the views are the game's echo
// (zeros before the first locate) and the panel would latch at the origin. The latch needs both.
static void latch_needs_valid_pose()
{
    CHECK(LatchAllowed(true, true));
    CHECK(!LatchAllowed(true, false));
    CHECK(!LatchAllowed(false, true));
}

// Final review I2: once the flag read has faulted, it is not read again (no exception per frame).
static void fault_stops_reading()
{
    State s;
    CHECK(ReadAllowed(s));
    s.fault = true;
    CHECK(!ReadAllowed(s));
}

// Final review I3: a void that never gets a picture gives the game image back for the rest of THAT screen, and the
// next screen tries again.
static void watchdog_restores_projection()
{
    State s;
    Step(s, true);
    CHECK(!WatchdogTrips(s, kWatchdogMs - 1));
    CHECK(WatchdogTrips(s, kWatchdogMs));
    s.gaveUp = true;
    CHECK(!WatchdogTrips(s, kWatchdogMs + 1000));   // trips once
    const Plan p = Decide(s, false);
    CHECK(!p.skipProjection && !p.quad[0] && !p.quad[1]);
    Step(s, false);
    Step(s, true);
    CHECK(!s.gaveUp && !s.shown);
    CHECK(Decide(s, false).skipProjection);
    s.shown = true;                                  // a quad reached the headset: no trip
    CHECK(!WatchdogTrips(s, kWatchdogMs + 1000));
    State n;                                         // not loading: never trips
    CHECK(!WatchdogTrips(n, kWatchdogMs + 1000));
    State f;
    Step(f, true);
    f.fault = true;                                  // already refused: never trips
    CHECK(!WatchdogTrips(f, kWatchdogMs + 1000));
}

// The spec's fixed placement (§4.2) - a changed constant fails the compile.
static_assert(kDistanceM == 1.8f, "panel distance");
static_assert(kHeightM == -0.25f, "panel height");
static_assert(kScale == 1.0f, "panel scale");

int main()
{
    edges();
    start_clears_pictures();
    restart_relatches();
    decide_off_when_not_loading();
    start_without_views_waits();
    quads_need_their_own_picture();
    fault_restores_projection();
    calib_void_wins();
    flag_alone_is_a_void();
    module_screen_extends_void();
    module_alone_never_starts();
    hud_ends_extension();
    menu_ends_extension();
    module_gone_ends_extension();
    unreadable_ends_extension();
    flag_restarts_sequence();
    flag_wins_over_hud();
    box_inside_offset_viewport();
    latch_needs_valid_pose();
    fault_stops_reading();
    watchdog_restores_projection();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
