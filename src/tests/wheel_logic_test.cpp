// Standalone unit test for src/src/ovr2xr/wheel_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-27-vr-selection-wheels.md Task 2. Exit code = number of failures.
#include "wheel_logic.h"

#include <cstdio>

using namespace dlvr::wheel;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const unsigned kHold = 750, kLead = 200;

// held 0..100 ms, released at 120: Tap at the release, never Lead / Open
static void tap_before_lead()
{
    HoldMachine m;
    bool sawOther = false;
    for (unsigned long long t = 0; t <= 100; t += 10) {
        const Out o = m.Update(true, t, kHold, kLead);
        if (o != Out::Nothing) { sawOther = true; }
    }
    CHECK(!sawOther);
    CHECK(m.Update(false, 120, kHold, kLead) == Out::Tap);
    CHECK(!m.Open());
    CHECK(m.Update(false, 130, kHold, kLead) == Out::Nothing);
}

// Lead once at 550 (750 - 200); a release at 600 = TapByGame (the button already reached the game), not Tap
static void release_after_lead_is_game_tap()
{
    HoldMachine m;
    int leads = 0;
    for (unsigned long long t = 0; t <= 590; t += 10) {
        if (m.Update(true, t, kHold, kLead) == Out::Lead) { ++leads; }
    }
    CHECK(leads == 1);
    CHECK(m.ButtonDown());
    CHECK(m.Update(false, 600, kHold, kLead) == Out::TapByGame);
    CHECK(!m.ButtonDown());
}

// Open exactly once at 750, Confirm at the release, button down from the lead to the release
static void open_at_750()
{
    HoldMachine m;
    int opens = 0;
    for (unsigned long long t = 0; t <= 900; t += 10) {
        const Out o = m.Update(true, t, kHold, kLead);
        if (o == Out::Open) {
            ++opens;
            CHECK(t == 750);
        }
    }
    CHECK(opens == 1);
    CHECK(m.Open());
    CHECK(m.ButtonDown());
    CHECK(m.Update(false, 910, kHold, kLead) == Out::Confirm);
    CHECK(!m.Open());
}

// Abort while open: the next Update says Cancel, the machine is closed, the button up; a still-held grip starts nothing
static void abort_while_open()
{
    HoldMachine m;
    for (unsigned long long t = 0; t <= 800; t += 10) { m.Update(true, t, kHold, kLead); }
    CHECK(m.Open());
    m.Abort();
    CHECK(m.Update(true, 810, kHold, kLead) == Out::Cancel);
    CHECK(!m.Open());
    CHECK(!m.ButtonDown());
    bool sawOther = false;
    for (unsigned long long t = 820; t <= 2000; t += 10) {
        if (m.Update(true, t, kHold, kLead) != Out::Nothing) { sawOther = true; }
    }
    CHECK(!sawOther);   // needs a fresh press edge
    CHECK(m.Update(false, 2010, kHold, kLead) == Out::Nothing);
}

static void slot_centre_is_none()
{
    SlotPicker p;
    for (int i = 0; i < 5; ++i) { p.Update(true, 0.05f, 0.05f, 0.12f); }
    CHECK(p.Committed() == Slot::None);
}

static Slot Settle(float dx, float dy)
{
    SlotPicker p;
    for (int i = 0; i < 3; ++i) { p.Update(true, dx, dy, 0.12f); }
    return p.Committed();
}

static void slot_axes()
{
    CHECK(Settle(0.3f, 0.0f) == Slot::Right);
    CHECK(Settle(-0.3f, 0.0f) == Slot::Left);
    CHECK(Settle(0.0f, 0.3f) == Slot::Up);
    CHECK(Settle(0.0f, -0.3f) == Slot::Down);
    CHECK(Settle(0.3f, 0.2f) == Slot::Right);
}

// a new slot needs 3 consecutive polls
static void slot_needs_three_polls()
{
    SlotPicker p;
    p.Update(true, 0.0f, 0.3f, 0.12f);
    p.Update(true, 0.0f, 0.3f, 0.12f);
    CHECK(p.Committed() == Slot::None);
    p.Update(true, 0.0f, 0.3f, 0.12f);
    CHECK(p.Committed() == Slot::Up);
    p.Update(true, 0.3f, 0.0f, 0.12f);
    CHECK(p.Committed() == Slot::Up);
    p.Update(true, 0.3f, 0.0f, 0.12f);
    p.Update(true, 0.3f, 0.0f, 0.12f);
    CHECK(p.Committed() == Slot::Right);
}

// pose lost -> None at once (Review Focus 1)
static void pose_lost_cancels()
{
    SlotPicker p;
    for (int i = 0; i < 3; ++i) { p.Update(true, 0.3f, 0.0f, 0.12f); }
    CHECK(p.Committed() == Slot::Right);
    p.Update(false, 0.3f, 0.0f, 0.12f);
    CHECK(p.Committed() == Slot::None);
}

static void stick_values()
{
    float x = 9, y = 9;
    StickFor(Slot::Up, x, y);    CHECK(x == 0.0f && y == 1.0f);
    StickFor(Slot::Right, x, y); CHECK(x == 1.0f && y == 0.0f);
    StickFor(Slot::Down, x, y);  CHECK(x == 0.0f && y == -1.0f);
    StickFor(Slot::Left, x, y);  CHECK(x == -1.0f && y == 0.0f);
    StickFor(Slot::None, x, y);  CHECK(x == 0.0f && y == 0.0f);
}

// D11: the slot the game's cursor is steered to - the hand's slot, else (centre) the player's current slot (game index 0..3 =
// up/right/down/left, INV-640 / spec 6a); an unreadable current slot (-1 or out of range) -> None (feed nothing)
static void target_slot()
{
    CHECK(TargetSlot(Slot::Left, 0) == Slot::Left);
    CHECK(TargetSlot(Slot::None, 0) == Slot::Up);
    CHECK(TargetSlot(Slot::None, 1) == Slot::Right);
    CHECK(TargetSlot(Slot::None, 2) == Slot::Down);
    CHECK(TargetSlot(Slot::None, 3) == Slot::Left);
    CHECK(TargetSlot(Slot::None, -1) == Slot::None);
    CHECK(TargetSlot(Slot::None, 4) == Slot::None);
}

int main()
{
    tap_before_lead();
    release_after_lead_is_game_tap();
    open_at_750();
    abort_while_open();
    slot_centre_is_none();
    slot_axes();
    slot_needs_three_polls();
    pose_lost_cancels();
    stick_values();
    target_slot();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
