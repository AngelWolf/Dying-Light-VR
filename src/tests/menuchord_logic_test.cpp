// Standalone unit test for src/src/ovr2xr/menuchord_logic.h - NOT part of the DLL build. Exit code = number of failures.
// SteamVR's X + Y chord: the 60 ms hold-back, the chord, taps, single presses, and the disabled pass-through.
#include "menuchord_logic.h"

#include <cstdio>

using namespace dlvr::menuchord;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void disabled_passes_straight_through()
{
    Machine m;
    Out o = Step(m, false, true, false, 1000);
    CHECK(o.sendX && !o.sendY && !o.chord && m.st == State::Idle);
    o = Step(m, false, true, true, 1010);
    CHECK(o.sendX && o.sendY && !o.chord && m.st == State::Idle);
    o = Step(m, false, false, false, 1020);
    CHECK(!o.sendX && !o.sendY && !o.chord);
}

static void chord_inside_window_sends_neither_bit()
{
    Machine m;
    Out o = Step(m, true, true, false, 1000);            // X first
    CHECK(!o.sendX && !o.sendY && !o.chord && m.st == State::Window);
    o = Step(m, true, true, false, 1030);
    CHECK(!o.sendX && !o.sendY && !o.chord);
    o = Step(m, true, true, true, 1050);                 // Y joins at 50 ms
    CHECK(!o.sendX && !o.sendY && o.chord && m.st == State::Chord);
    o = Step(m, true, true, true, 1600);                 // held: the menu machine sees down
    CHECK(!o.sendX && !o.sendY && o.chord);
    o = Step(m, true, true, false, 1610);                // Y let go first
    CHECK(!o.sendX && !o.sendY && !o.chord && m.st == State::WaitRelease);
    o = Step(m, true, true, false, 1700);                // X still down: nothing leaks
    CHECK(!o.sendX && !o.sendY && !o.chord);
    o = Step(m, true, false, false, 1710);
    CHECK(!o.sendX && !o.sendY && !o.chord && m.st == State::Idle);
}

static void both_in_same_poll_is_a_chord()
{
    Machine m;
    Out o = Step(m, true, true, true, 1000);
    CHECK(o.chord && !o.sendX && !o.sendY && m.st == State::Chord);
}

static void y_first_then_x_is_a_chord()
{
    Machine m;
    Step(m, true, false, true, 1000);
    CHECK(m.st == State::Window && !m.firstIsX);
    Out o = Step(m, true, true, true, 1059);             // 59 ms - inside
    CHECK(o.chord);
}

static void window_expiry_passes_the_single_button()
{
    Machine m;
    Out o = Step(m, true, true, false, 1000);
    CHECK(!o.sendX);
    o = Step(m, true, true, false, 1059);
    CHECK(!o.sendX && m.st == State::Window);
    o = Step(m, true, true, false, 1060);                // 60 ms reached
    CHECK(o.sendX && !o.sendY && !o.chord && m.st == State::Pass);
    o = Step(m, true, true, true, 1200);                 // Y pressed later: a plain press, not a chord
    CHECK(o.sendX && o.sendY && !o.chord && m.st == State::Pass);
    o = Step(m, true, false, true, 1300);
    CHECK(!o.sendX && o.sendY);
    o = Step(m, true, false, false, 1310);
    CHECK(!o.sendX && !o.sendY && m.st == State::Idle);
}

static void quick_tap_is_resent_as_a_pulse()
{
    Machine m;
    Step(m, true, false, true, 1000);                    // Y down
    Out o = Step(m, true, false, false, 1030);           // up at 30 ms - never reached the game yet
    CHECK(!o.sendX && o.sendY && m.st == State::Tap);
    o = Step(m, true, false, false, 1129);
    CHECK(o.sendY);
    o = Step(m, true, false, false, 1130);               // 100 ms pulse done
    CHECK(!o.sendY && m.st == State::Idle);
}

static void first_released_while_other_down_is_tap_plus_press()
{
    Machine m;
    Step(m, true, true, false, 1000);                    // X down
    Out o = Step(m, true, false, true, 1040);            // X up, Y down in the same poll - rolled, not a chord
    CHECK(o.sendX && o.sendY && !o.chord && m.st == State::Tap);
    o = Step(m, true, false, true, 1140);                // pulse done, Y still held
    CHECK(!o.sendX && o.sendY && m.st == State::Pass);
    o = Step(m, true, false, false, 1200);
    CHECK(!o.sendY && m.st == State::Idle);
}

static void disable_mid_window_resets()
{
    Machine m;
    Step(m, true, true, false, 1000);
    Out o = Step(m, false, true, false, 1010);
    CHECK(o.sendX && m.st == State::Idle);
}

static void runtime_names()
{
    CHECK(RuntimeIsSteamVr("SteamVR/OpenXR"));
    CHECK(RuntimeIsSteamVr("SteamVR/OpenXR in Meta compatibility mode"));
    CHECK(!RuntimeIsSteamVr("VirtualDesktopXR"));
    CHECK(!RuntimeIsSteamVr("WiVRn 'v26.9'"));
    CHECK(!RuntimeIsSteamVr("Oculus"));
    CHECK(!RuntimeIsSteamVr("SteamVR"));
    CHECK(!RuntimeIsSteamVr(nullptr));
}

int main()
{
    disabled_passes_straight_through();
    chord_inside_window_sends_neither_bit();
    both_in_same_poll_is_a_chord();
    y_first_then_x_is_a_chord();
    window_expiry_passes_the_single_button();
    quick_tap_is_resent_as_a_pulse();
    first_released_while_other_down_is_tap_plus_press();
    disable_mid_window_resets();
    runtime_names();
    std::printf("menuchord_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
