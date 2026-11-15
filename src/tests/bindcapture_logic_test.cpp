// Standalone unit test for src_beta/src/ovr2xr/bindcapture_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-28-controls-rebind.md Task 5. Exit code = number of failures.
#include "bindcapture_logic.h"

#include <cstdio>
#include <cstring>

using namespace dlvr;
using namespace dlvr::bindcapture;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static Ctl Idle() { return Ctl{}; }

int main()
{
    const int kick = binds::FindAction("_ACTION_KICK");
    const int fwd = binds::FindAction("_ACTION_FORWARD");
    const int sprint = binds::FindAction("_ACTION_SPRINT_TOGGLE");
    const int fire = binds::FindAction("_ACTION_FIRE_WEAPON");
    CHECK(kick >= 0 && fwd >= 0 && sprint >= 0 && fire >= 0);

    // left grip after Begin -> LEFTSHOULDER, kick keeps its analog flag (false), half = kick's default (false)
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        CHECK(Poll(&st, kick, Idle(), false, &s) == Result::Waiting);
        Ctl c = Idle();
        c.lgrip = 0.8f;
        CHECK(Poll(&st, kick, c, false, &s) == Result::Captured);
        CHECK(s.bound && s.input == binds::FindInput("EJoy__BUTTON_LEFTSHOULDER") && !s.analog && !s.reverse && !s.half);
    }
    // right stick up -> AXIS_RY, reverse false (= up, inputs_pad.scr :110 / :228), half true; sprint's analog flag (false)
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        Ctl c = Idle();
        c.ry = 0.9f;
        CHECK(Poll(&st, sprint, c, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__AXIS_RY") && !s.reverse && s.half && !s.analog);
    }
    // left stick left -> AXIS_X reverse true (STRAFE_LEFT :222); right stick down -> AXIS_RY reverse true
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        Ctl c = Idle();
        c.lx = -0.9f;
        CHECK(Poll(&st, kick, c, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__AXIS_X") && s.reverse && s.half);
        Begin(&st, Idle());
        c = Idle();
        c.ry = -0.9f;
        CHECK(Poll(&st, kick, c, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__AXIS_RY") && s.reverse);
    }
    // FORWARD (analog true) onto a button keeps analog true (the psvita preset's form, inputs_pad.scr :468)
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        Ctl c = Idle();
        c.a = true;
        CHECK(Poll(&st, fwd, c, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__BUTTON_A") && s.analog);
    }
    // right trigger -> AXIS_RIGHTTRIGGER; fire keeps its own half flag (true, :46)
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        Ctl c = Idle();
        c.rt = 0.9f;
        CHECK(Poll(&st, fire, c, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__AXIS_RIGHTTRIGGER") && !s.analog && s.half);
    }
    // an input already held at Begin (the A that opened the capture) does not count until released and pressed again
    {
        State st;
        Ctl held = Idle();
        held.a = true;
        Begin(&st, held);
        binds::Slot s{};
        CHECK(Poll(&st, kick, held, false, &s) == Result::Waiting);
        CHECK(Poll(&st, kick, Idle(), false, &s) == Result::Waiting);
        CHECK(Poll(&st, kick, held, false, &s) == Result::Captured);
        CHECK(s.input == binds::FindInput("EJoy__BUTTON_A"));
    }
    // menu cancels and leaves the slot untouched (Review Focus 4); backspace = unbound
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{ true, 3, true, true, true };
        const binds::Slot before = s;
        Ctl c = Idle();
        c.menu = true;
        CHECK(Poll(&st, kick, c, false, &s) == Result::Cancelled);
        CHECK(binds::Equal(s, before));
        Begin(&st, Idle());
        CHECK(Poll(&st, kick, Idle(), true, &s) == Result::Captured);
        CHECK(!s.bound);
    }
    // below the thresholds: stick 0.6, grip 0.4 -> still waiting
    {
        State st;
        Begin(&st, Idle());
        binds::Slot s{};
        Ctl c = Idle();
        c.ry = 0.6f;
        c.lgrip = 0.4f;
        CHECK(Poll(&st, kick, c, false, &s) == Result::Waiting);
    }
    // plain names for the tab
    {
        char buf[64];
        SlotName(binds::Slot{ true, binds::FindInput("EJoy__BUTTON_LEFTSHOULDER"), false, false, false }, buf, sizeof(buf));
        CHECK(std::strcmp(buf, "Left grip") == 0);
        SlotName(binds::Slot{ true, binds::FindInput("EJoy__AXIS_RY"), false, false, true }, buf, sizeof(buf));
        CHECK(std::strcmp(buf, "Right stick up") == 0);
        SlotName(binds::Slot{ true, binds::FindInput("EJoy__AXIS_X"), true, true, true }, buf, sizeof(buf));
        CHECK(std::strcmp(buf, "Left stick left") == 0);
        SlotName(binds::Slot{ false, 0, false, false, false }, buf, sizeof(buf));
        CHECK(std::strcmp(buf, "Unbound") == 0);
        // every input in the table has a name (a missing one would print the raw EJoy id)
        for (int i = 0; i < binds::gen::kInputCount; ++i) {
            SlotName(binds::Slot{ true, i, false, false, false }, buf, sizeof(buf));
            CHECK(std::strncmp(buf, "EJoy", 4) != 0);
        }
    }

    std::printf("%s - %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
