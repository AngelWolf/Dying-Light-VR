// SteamVR: X + Y together on the left hand = the game-menu input (Jay 2026-11-18: "What about a combo input." -> "Yes, do the X+Y
// trace" -> "60 ms, combo works in menus, keep Options tap" -> "Do it"; trace docs/traces/20261118_1047_steamvr_xy_menu_chord.md).
// SteamVR takes the left Touch menu button for itself (Quest on SteamVR, Jay's observation), and PSVR2's Sense controllers get it
// only as a TAP on the right Options button (Sony's remap, backups/20261118_psvr2_sony_input_files) - so on SteamVR the chord feeds
// the same tap / hold machine the menu button does (menutap_logic.h: tap = SELECT, 0.5 s = START).
// Pure - no clocks, no game memory: the caller passes the raw X / Y and the time. Unit test: tests/menuchord_logic_test.cpp.
//
// The rule (only while `enabled` - the caller passes runtime "SteamVR/OpenXR*" AND the left hand not on the Index profile):
//   Idle    -> the first of X / Y down starts the WINDOW (kWindowMs); neither bit is sent. Both down in the same poll -> CHORD.
//   Window  -> the other one down too -> CHORD. Both up again -> TAP: the first button's bit for kTapPulseMs. kWindowMs reached with
//              one down -> PASS. The first let go while the other is down -> TAP of the first (the other is then a plain press).
//   Chord   -> `chord` = true (the menu machine's `down`) while BOTH stay down; neither bit is sent. One let go -> WaitRelease.
//   WaitRel -> nothing sent until BOTH are up, then Idle - so the chord never leaks an X or a Y to the game.
//   Pass / Tap -> plain pass-through of whatever is down (plus the tap's pulse); both up and the pulse done -> Idle.
// Disabled -> X / Y pass straight through, the machine stays Idle (VDXR, WiVRn, Meta's runtime, Index: unchanged).
#pragma once

namespace dlvr {
namespace menuchord {

constexpr unsigned long long kWindowMs   = 60;    // Jay 2026-11-18: "60 ms"
constexpr unsigned long long kTapPulseMs = 100;   // a tap inside the window is re-sent this long (the pulse length INV-299 measured)

enum class State { Idle, Window, Chord, WaitRelease, Tap, Pass };

struct Machine {
    State              st = State::Idle;
    unsigned long long t = 0;        // time the current state was entered
    bool               firstIsX = false;
};

struct Out {
    bool sendX = false;   // OR Square 0x8000
    bool sendY = false;   // OR Triangle 0x1000
    bool chord = false;   // the menu machine's `down` (OR'd with the menu button by the caller)
};

inline Out Step(Machine& m, bool enabled, bool x, bool y, unsigned long long now)
{
    Out o;
    if (!enabled) {
        m.st = State::Idle;
        o.sendX = x;
        o.sendY = y;
        return o;
    }
    const unsigned long long age = now - m.t;
    switch (m.st) {
    case State::Idle:
        if (x && y) { m.st = State::Chord; m.t = now; }
        else if (x || y) { m.st = State::Window; m.t = now; m.firstIsX = x; }
        break;
    case State::Window: {
        const bool first = m.firstIsX ? x : y;
        const bool other = m.firstIsX ? y : x;
        if (first && other) { m.st = State::Chord; m.t = now; }
        else if (!first) { m.st = State::Tap; m.t = now; }      // a tap of the first (the other, if down, passes from here)
        else if (age >= kWindowMs) { m.st = State::Pass; m.t = now; }
        break;
    }
    case State::Chord:
        if (!(x && y)) { m.st = State::WaitRelease; m.t = now; }
        break;
    case State::WaitRelease:
        if (!x && !y) { m.st = State::Idle; m.t = now; }
        break;
    case State::Tap:
        if (age >= kTapPulseMs && !x && !y) { m.st = State::Idle; m.t = now; }
        else if (age >= kTapPulseMs) { m.st = State::Pass; m.t = now; }
        break;
    case State::Pass:
        if (!x && !y) { m.st = State::Idle; m.t = now; }
        break;
    }
    if (m.st == State::Pass || m.st == State::Tap) {
        o.sendX = x;
        o.sendY = y;
        if (m.st == State::Tap) {
            if (m.firstIsX) { o.sendX = true; } else { o.sendY = true; }
        }
    }
    o.chord = m.st == State::Chord;
    return o;
}

inline const char* StateName(State s)
{
    switch (s) {
    case State::Idle: return "idle";
    case State::Window: return "window (X / Y held back)";
    case State::Chord: return "CHORD (X + Y = menu button down)";
    case State::WaitRelease: return "wait for X and Y up";
    case State::Tap: return "tap (re-sent pulse)";
    case State::Pass: return "pass (single button)";
    }
    return "?";
}

// "SteamVR/OpenXR" and "SteamVR/OpenXR in Meta compatibility mode" (every SteamVR runtime name on record, 2026-11-18).
inline bool RuntimeIsSteamVr(const char* runtimeName)
{
    static const char kName[] = "SteamVR/OpenXR";
    if (!runtimeName) {
        return false;
    }
    for (int i = 0; kName[i]; ++i) {
        if (runtimeName[i] != kName[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace menuchord
}  // namespace dlvr
