// bindcapture_logic - the Controls tab's "press an input" step: Touch controller state -> a binds::Slot.
// docs/superpowers/plans/2026-10-28-controls-rebind.md Task 5. Pure logic, no OpenXR / Windows: overlay.cpp fills a Ctl from
// controllers.h each frame while a slot is capturing; bindcapture_logic_test.cpp tests it.
//
// The input a press becomes is the pad input the layer ALREADY sends for that control (dlvr_FilterScePad's fixed 1:1 map):
// grips = L1 / R1, A/B/X/Y, stick clicks = L3 / R3, triggers = the L2 / R2 axes, stick directions = the stick axes. A stick
// direction follows inputs_pad.scr's own convention: Y / RY reverse=false is UP (:110, :219, :228), X / RX reverse=false is
// RIGHT (:223, :226); half = true. The action keeps its own analog flag (FORWARD analog=true on a button = the psvita preset's
// form, :468; KICK analog=false on a stick half = no stock precedent, provisional until the Task 4 flat run - ledger G3 (5)).
// Edge-based: an input held when capture began (the A that opened it) counts only after it is released and pressed again.
// The left menu button cancels; Backspace (keyboard) captures "unbound".
#pragma once

#include <cstdio>
#include <cstring>

#include "../common/binds_logic.h"

namespace dlvr::bindcapture {

struct Ctl {
    float lgrip = 0.0f, rgrip = 0.0f, lt = 0.0f, rt = 0.0f;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;   // OpenXR convention: +y = stick pushed forward (up)
    bool a = false, b = false, x = false, y = false, l3 = false, r3 = false, menu = false;
};

enum class Result { Waiting, Captured, Cancelled };

constexpr float kPressThreshold = 0.5f;   // grips / triggers - the layer's own kGripThreshold (ovr2xr.cpp)
constexpr float kStickThreshold = 0.7f;   // a deliberate push, not drift

// One bit per capturable control.
enum : unsigned {
    kLGrip = 1u << 0, kRGrip = 1u << 1, kLT = 1u << 2, kRT = 1u << 3, kA = 1u << 4, kB = 1u << 5, kX = 1u << 6, kY = 1u << 7,
    kL3 = 1u << 8, kR3 = 1u << 9, kLUp = 1u << 10, kLDown = 1u << 11, kLLeft = 1u << 12, kLRight = 1u << 13,
    kRUp = 1u << 14, kRDown = 1u << 15, kRLeft = 1u << 16, kRRight = 1u << 17, kCount = 18,
};

struct State {
    unsigned held = 0;   // controls down at Begin, not yet released
};

inline unsigned Down(const Ctl& c)
{
    unsigned m = 0;
    if (c.lgrip >= kPressThreshold) { m |= kLGrip; }
    if (c.rgrip >= kPressThreshold) { m |= kRGrip; }
    if (c.lt >= kPressThreshold) { m |= kLT; }
    if (c.rt >= kPressThreshold) { m |= kRT; }
    if (c.a) { m |= kA; }
    if (c.b) { m |= kB; }
    if (c.x) { m |= kX; }
    if (c.y) { m |= kY; }
    if (c.l3) { m |= kL3; }
    if (c.r3) { m |= kR3; }
    if (c.ly >= kStickThreshold) { m |= kLUp; }
    if (c.ly <= -kStickThreshold) { m |= kLDown; }
    if (c.lx <= -kStickThreshold) { m |= kLLeft; }
    if (c.lx >= kStickThreshold) { m |= kLRight; }
    if (c.ry >= kStickThreshold) { m |= kRUp; }
    if (c.ry <= -kStickThreshold) { m |= kRDown; }
    if (c.rx <= -kStickThreshold) { m |= kRLeft; }
    if (c.rx >= kStickThreshold) { m |= kRRight; }
    return m;
}

inline void Begin(State* st, const Ctl& now) { st->held = Down(now); }

namespace detail {

// The action's own flags from its slot-1 line in the generated table (the line exists for every action, bound or not).
inline void ActionFlags(int action, bool* analog, bool* half)
{
    *analog = false;
    *half = false;
    for (int i = 0; i < binds::gen::kLineCount; ++i) {
        if (binds::gen::kLines[i].action == action && binds::gen::kLines[i].slot == 0) {
            *analog = binds::gen::kLines[i].analog;
            *half = binds::gen::kLines[i].half;
            return;
        }
    }
}

}  // namespace detail

// Poll once per frame while capturing. On Captured, *out is the new slot (bound, or unbound for Backspace).
inline Result Poll(State* st, int action, const Ctl& now, bool backspace, binds::Slot* out)
{
    if (now.menu) { return Result::Cancelled; }
    if (backspace) {
        *out = binds::Slot{ false, 0, false, false, false };
        return Result::Captured;
    }
    const unsigned down = Down(now);
    st->held &= down;                       // a control released since Begin is free from now on
    const unsigned fresh = down & ~st->held;
    if (!fresh) { return Result::Waiting; }
    bool analog = false, half = false;
    detail::ActionFlags(action, &analog, &half);
    struct Map { unsigned bit; const char* input; bool stick; bool reverse; };
    static const Map kMap[] = {
        { kLGrip, "EJoy__BUTTON_LEFTSHOULDER", false, false }, { kRGrip, "EJoy__BUTTON_RIGHTSHOULDER", false, false },
        { kLT, "EJoy__AXIS_LEFTTRIGGER", false, false },       { kRT, "EJoy__AXIS_RIGHTTRIGGER", false, false },
        { kA, "EJoy__BUTTON_A", false, false },                { kB, "EJoy__BUTTON_B", false, false },
        { kX, "EJoy__BUTTON_X", false, false },                { kY, "EJoy__BUTTON_Y", false, false },
        { kL3, "EJoy__BUTTON_LEFTTHUMB", false, false },       { kR3, "EJoy__BUTTON_RIGHTTHUMB", false, false },
        { kLUp, "EJoy__AXIS_Y", true, false },                 { kLDown, "EJoy__AXIS_Y", true, true },
        { kLLeft, "EJoy__AXIS_X", true, true },                { kLRight, "EJoy__AXIS_X", true, false },
        { kRUp, "EJoy__AXIS_RY", true, false },                { kRDown, "EJoy__AXIS_RY", true, true },
        { kRLeft, "EJoy__AXIS_RX", true, true },               { kRRight, "EJoy__AXIS_RX", true, false },
    };
    for (const Map& m : kMap) {
        if ((fresh & m.bit) == 0) { continue; }
        const int in = binds::FindInput(m.input);
        if (in < 0) { return Result::Waiting; }   // cannot happen with the generated table; never invent an input
        *out = binds::Slot{ true, in, analog, m.reverse, m.stick ? true : half };
        return Result::Captured;
    }
    return Result::Waiting;
}

// The plain name a slot shows on the tab.
inline void SlotName(const binds::Slot& s, char* out, size_t cch)
{
    if (!s.bound) {
        std::snprintf(out, cch, "Unbound");
        return;
    }
    const char* in = binds::gen::kInputs[s.input];
    // minus = the reverse=true half of a stick axis; both = the whole axis (half=false - the buggy's steering lines, :254-255)
    struct N { const char* input; const char* plain; const char* minus; const char* both; };
    static const N kNames[] = {
        { "EJoy__BUTTON_A", "A", nullptr, nullptr }, { "EJoy__BUTTON_B", "B", nullptr, nullptr },
        { "EJoy__BUTTON_X", "X", nullptr, nullptr }, { "EJoy__BUTTON_Y", "Y", nullptr, nullptr },
        { "EJoy__BUTTON_LEFTSHOULDER", "Left grip", nullptr, nullptr },
        { "EJoy__BUTTON_RIGHTSHOULDER", "Right grip", nullptr, nullptr },
        { "EJoy__BUTTON_LEFTTHUMB", "Left stick click", nullptr, nullptr },
        { "EJoy__BUTTON_RIGHTTHUMB", "Right stick click", nullptr, nullptr },
        { "EJoy__BUTTON_BACK", "Menu button (tap)", nullptr, nullptr },
        { "EJoy__BUTTON_TOUCHPAD", "Menu button (tap)", nullptr, nullptr },
        { "EJoy__BUTTON_START", "Menu button (hold)", nullptr, nullptr },
        { "EJoy__BUTTON_DPADUP", "D-pad up (head holster)", nullptr, nullptr },
        { "EJoy__BUTTON_DPADDOWN", "D-pad down (left hip holster)", nullptr, nullptr },
        { "EJoy__BUTTON_DPADLEFT", "D-pad left (right hip holster)", nullptr, nullptr },
        { "EJoy__BUTTON_DPADRIGHT", "D-pad right (shoulder holster)", nullptr, nullptr },
        { "EJoy__BUTTON_16", "Physical crouch", nullptr, nullptr },
        { "EJoy__AXIS_LEFTTRIGGER", "Left trigger", nullptr, nullptr },
        { "EJoy__AXIS_RIGHTTRIGGER", "Right trigger", nullptr, nullptr },
        { "EJoy__AXIS_X", "Left stick right", "Left stick left", "Left stick left/right" },
        { "EJoy__AXIS_Y", "Left stick up", "Left stick down", "Left stick up/down" },
        { "EJoy__AXIS_RX", "Right stick right", "Right stick left", "Right stick left/right" },
        { "EJoy__AXIS_RY", "Right stick up", "Right stick down", "Right stick up/down" },
    };
    for (const N& n : kNames) {
        if (std::strcmp(n.input, in) != 0) { continue; }
        if (n.both && !s.half) {
            std::snprintf(out, cch, "%s", n.both);
            return;
        }
        std::snprintf(out, cch, "%s", (n.minus && s.reverse) ? n.minus : n.plain);
        return;
    }
    std::snprintf(out, cch, "%s", in);   // an input without a plain name (the test checks there is none)
}

}  // namespace dlvr::bindcapture
