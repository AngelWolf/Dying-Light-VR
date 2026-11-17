// The left Touch menu button's tap / hold machine (vr_menu_button, ovr2xr.cpp dlvr_FilterScePad). Pure - no game memory, no
// clocks: the caller passes the button, the time and (only when asked) the co-op teleport offer. Unit test:
// tests/menutap_logic_test.cpp.
//
// Today's rules (2026-10-03, Jay; D-175 - Virtual Desktop owns a double press and a ~1 s hold of this button):
//   a release before kHoldMs  -> SELECT: TOUCH_PAD for kPulseMs
//   held to kHoldMs           -> START: OPTIONS for as long as the button stays held
// Co-op waiting-place teleport (2026-11-17, Jay: "Yes, build it that way"; INV-745): when the 500 ms hold lands while the game
// OFFERS the teleport (its hold-progress float on the player reads [0, 1]) and no in-game menu is up, the hold LATCHES a held
// TOUCH_PAD instead of START - the game needs >= 1.0 s of held input (more than VD's ~1 s shortcut), so the player lets go and the
// latch holds it. The latch ends on: the offer ending, the progress dropping (the game's own reset when it fires), an in-game menu
// opening, a new press of the button (cancel), or kLatchCapMs. Then nothing is sent until the button is up.
#pragma once

namespace dlvr {
namespace menutap {

constexpr unsigned long long kHoldMs     = 500;    // a press held this long is a hold (Jay 2026-10-03: "Make the hold .5 seconds")
constexpr unsigned long long kPulseMs    = 100;    // SELECT's pulse - the length INV-299 measured working
constexpr unsigned long long kLatchCapMs = 3000;   // the fill needs 1.0 s (+ 0 or 0.5 s HoldTime, INV-745); a BLOCKED offer pins 0.0
constexpr float              kDropEnd    = 0.25f;  // a progress fall larger than this between two reads = the game reset it (fired)

enum class State { Idle, Down, SelectPulse, StartHeld, TeleportLatch, WaitRelease };

enum class EndWhy { None, OfferGone, ProgressReset, GameMenu, Cancelled, Cap };

struct Machine {
    State              st = State::Idle;
    unsigned long long t = 0;          // time the current state was entered
    float              lastProg = -1.0f;
    bool               releasedInLatch = false;
};

struct Out {
    bool   touchpad = false;   // OR TOUCH_PAD 0x100000 into the pad word
    bool   start = false;      // OR OPTIONS 0x8
    bool   needOffer = false;  // the caller must read the offer for the NEXT step (Down past kHoldMs, or the latch)
    EndWhy ended = EndWhy::None;
};

// True when the machine will consult `offer` / `progress` on this step - lets the caller read game memory only then.
inline bool NeedsOffer(const Machine& m, bool down, unsigned long long now)
{
    if (m.st == State::TeleportLatch) { return true; }
    return m.st == State::Down && down && now - m.t >= kHoldMs;
}

// One poll. offer / progress are only read when NeedsOffer() said so (pass false / -1 otherwise). gameMenu = an in-game menu is up.
inline Out Step(Machine& m, bool down, unsigned long long now, bool offer, float progress, bool gameMenu)
{
    Out o;
    const unsigned long long age = now - m.t;
    switch (m.st) {
    case State::Idle:
        if (down) { m.st = State::Down; m.t = now; }
        break;
    case State::Down:
        if (!down) {
            m.st = State::SelectPulse; m.t = now;
        } else if (age >= kHoldMs) {
            if (offer && !gameMenu) {
                m.st = State::TeleportLatch; m.t = now; m.lastProg = progress; m.releasedInLatch = false;
            } else {
                m.st = State::StartHeld; m.t = now;
            }
        }
        break;
    case State::SelectPulse:
        if (age >= kPulseMs) { m.st = State::Idle; m.t = now; }
        break;
    case State::StartHeld:
        if (!down && age >= kPulseMs) { m.st = State::Idle; m.t = now; }
        break;
    case State::TeleportLatch: {
        if (!down) { m.releasedInLatch = true; }
        EndWhy why = EndWhy::None;
        if (!offer) { why = EndWhy::OfferGone; }
        else if (progress < m.lastProg - kDropEnd) { why = EndWhy::ProgressReset; }
        else if (gameMenu) { why = EndWhy::GameMenu; }
        else if (down && m.releasedInLatch) { why = EndWhy::Cancelled; }
        else if (age >= kLatchCapMs) { why = EndWhy::Cap; }
        if (why != EndWhy::None) {
            o.ended = why;
            m.st = State::WaitRelease; m.t = now;
        } else {
            m.lastProg = progress;
        }
        break;
    }
    case State::WaitRelease:
        if (!down) { m.st = State::Idle; m.t = now; }
        break;
    }
    o.touchpad = m.st == State::SelectPulse || m.st == State::TeleportLatch;
    o.start = m.st == State::StartHeld;
    o.needOffer = NeedsOffer(m, down, now);
    return o;
}

inline const char* StateName(State s)
{
    switch (s) {
    case State::Idle: return "idle";
    case State::Down: return "down";
    case State::SelectPulse: return "SELECT pulse";
    case State::StartHeld: return "START held";
    case State::TeleportLatch: return "TELEPORT latch (touchpad held)";
    case State::WaitRelease: return "wait for release";
    }
    return "?";
}

inline const char* EndName(EndWhy w)
{
    switch (w) {
    case EndWhy::None: return "-";
    case EndWhy::OfferGone: return "the offer ended (progress -1 / unreadable - fired, or the partner left)";
    case EndWhy::ProgressReset: return "the progress dropped (the game reset it - fired)";
    case EndWhy::GameMenu: return "an in-game menu opened";
    case EndWhy::Cancelled: return "a new press of the menu button (cancel)";
    case EndWhy::Cap: return "the 3 s cap (the offer may be blocked - progress pinned)";
    }
    return "?";
}

// The offer from the raw float at PlayerDI+10340 (INV-745): -1.0 = none; [0, 1] = on offer; anything else = unreadable -> none.
inline bool OfferFromProgress(float v)
{
    return v == v && v >= 0.0f && v <= 1.0f;
}

}  // namespace menutap
}  // namespace dlvr
