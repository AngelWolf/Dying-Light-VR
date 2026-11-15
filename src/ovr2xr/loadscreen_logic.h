// LOADING SCREEN VOID - the decisions, with no engine, OpenXR or Windows in it
// (docs/superpowers/specs/2026-10-20-loading-screen-void-design.md §4 / §5). Tested offline by
// src/src/tests/loadscreen_logic_test.cpp; used by loadscreen.cpp.
//
// While the engine draws its background path (loading screens and the like) the game image is not submitted (a black
// void) and each eye's HUD-box picture goes on a room-fixed quad latched when the screen started. No setting: always on.
#pragma once

#include "uipanel_logic.h"

namespace dlvr {
namespace loadscreen {

// Placement, fixed (spec §4.2: the menu panel's compiled defaults; reads no setting).
constexpr float kDistanceM = 1.8f;
constexpr float kHeightM   = -0.25f;
constexpr float kScale     = 1.0f;

enum class Edge { None, Start, End };

struct State {
    bool loading = false;               // the engine's background-render flag, this submit
    bool latched = false;               // the panel pose was taken for THIS screen
    bool fault = false;                 // a step refused: the game image is submitted again for the rest of the session
    bool havePic[2] = { false, false }; // the eye's panel image holds a picture of THIS screen
    bool shown = false;                 // a panel quad was submitted on THIS screen
    bool gaveUp = false;                // no picture reached the panel in time: the game image for the rest of THIS screen
    bool seq = false;                   // amendment 1: a void sequence the engine flag started is running
};

// Final review I3: a void with no picture this long after ON gives the game image back for the rest of that screen.
constexpr unsigned long long kWatchdogMs = 500;

// One submit's flag. A start forgets the previous screen's latch and pictures; an end drops the latch.
inline Edge Step(State& s, bool loadingNow)
{
    if (loadingNow == s.loading) { return Edge::None; }
    s.loading = loadingNow;
    s.latched = false;
    if (loadingNow) {
        s.havePic[0] = s.havePic[1] = false;
        s.shown = false;
        s.gaveUp = false;
        return Edge::Start;
    }
    return Edge::End;
}

struct Plan {
    bool skipProjection;   // do not submit the game image this submit
    bool quad[2];          // submit eye e's panel quad
};

// calibVoid = the calibration screen's void is up (it already skips the game image and owns the void).
inline Plan Decide(const State& s, bool calibVoid)
{
    Plan p{};
    p.skipProjection = s.loading && !s.fault && !s.gaveUp && !calibVoid;
    for (int e = 0; e < 2; ++e) { p.quad[e] = p.skipProjection && s.latched && s.havePic[e]; }
    return p;
}

// Final review I1: latch only from a LOCATED head pose - a usable projection alone may carry the game's echo (zeros).
inline bool LatchAllowed(bool viewsValid, bool posesValid) { return viewsValid && posesValid; }

// Final review I2: a faulted flag read is not tried again.
inline bool ReadAllowed(const State& s) { return !s.fault; }

// Final review I3: true once, when this screen has had no panel quad for kWatchdogMs.
inline bool WatchdogTrips(const State& s, unsigned long long msSinceStart)
{
    return s.loading && !s.fault && !s.gaveUp && !s.shown && msSinceStart >= kWatchdogMs;
}

// Amendment 1 (the void SEQUENCE): the engine flag starts it; after the flag drops it continues while the loading module shows
// a screen ("Press A to continue", the gap between two background screens), and ends at the HUD, a menu, the module gone or an
// unreadable read. moduleScreens: visible screens of the loading module's level, -1 = unreadable; hud: uipin::HudShowing();
// menu: uipin::MenuState(). Returns whether the void is up this submit (feed it to Step).
inline bool SequenceActive(State& s, bool flag, int moduleScreens, int hud, int menu)
{
    if (flag) {
        s.seq = true;
        return true;
    }
    if (s.seq && (moduleScreens <= 0 || hud == 1 || menu > 0)) { s.seq = false; }
    return s.seq;
}

// The flat-screen HUD box (where the loading picture sits - Jay 2026-10-20) inside one eye's viewport, from that eye's rendered
// frustum (INV-735 - any headset). False when it cannot be known yet (no frustum / a viewport under 64 px).
inline bool EyeBox(int vx, int vy, int vw, int vh, const uipanel::EyeTan& t, uipanel::Rect* out)
{
    uipanel::Rect r{};
    if (!out || !uipanel::UiCrop(vw, vh, t, &r)) { return false; }
    r.x += vx;
    r.y += vy;
    *out = r;
    return true;
}

}  // namespace loadscreen
}  // namespace dlvr
