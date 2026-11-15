// Standalone unit test for src/src/ovr2xr/crouch_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-27-roomscale-physical-crouch.md Task 8. Exit code = number of failures.
#include "crouch_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::crouch;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static const float kDt = 1.0f / 90.0f;
static void Hold(Learner& L, float h, bool crouched, float seconds, float offDrop = 0.25f)
{
    for (float t = 0.0f; t < seconds; t += kDt) { Learn(L, h, crouched, kDt, offDrop); }
}

static void learns_standing_height()
{
    Learner L;
    Learn(L, 1.50f, false, kDt, 0.25f);                    // the first sample seeds it
    CHECK(L.has && Near(L.standM, 1.50f, 1e-6f));
    Hold(L, 1.70f, false, 3.0f);                           // the player stands up straight: the level rises to it
    CHECK(Near(L.standM, 1.70f, 0.01f));
}
static void crouch_does_not_lower_standing()
{
    Learner L;
    Learn(L, 1.70f, false, kDt, 0.25f);
    Hold(L, 1.10f, true, 2.0f);                            // crouched
    CHECK(Near(L.standM, 1.70f, 1e-6f));
    Hold(L, 1.10f, false, 2.0f);                           // that low, not yet flagged crouched: below stand - off drop, still held
    CHECK(Near(L.standM, 1.70f, 1e-6f));
}
static void standing_drifts_down_slowly()
{
    Learner L;
    Learn(L, 1.70f, false, kDt, 0.25f);
    Hold(L, 1.60f, false, 1.0f);                           // slouching a little: falls at most kDownMps a second
    CHECK(L.standM < 1.70f && L.standM >= 1.70f - kDownMps * 1.0f - 1e-3f);
}
static void hysteresis_no_flicker()
{
    Hyst H;
    CHECK(HystStep(H, 1.34f, 1.70f, 0.35f, 0.25f));        // below 1.35 -> on
    CHECK(HystStep(H, 1.40f, 1.70f, 0.35f, 0.25f));        // between the two points -> stays on
    CHECK(!HystStep(H, 1.46f, 1.70f, 0.35f, 0.25f));       // above 1.45 -> off
    CHECK(!HystStep(H, 1.40f, 1.70f, 0.35f, 0.25f));       // between -> stays off
}
static void invalid_drops_refused()
{
    CHECK(!ValidDrops(0.2f, 0.3f));
    CHECK(!ValidDrops(0.3f, 0.3f));
    CHECK(!ValidDrops(0.3f, 0.0f));
    CHECK(ValidDrops(0.35f, 0.25f));
}
static void no_slide_while_sprinting()
{
    Arb A;
    CHECK(Decide(A, true, false, true, false) == Act::None);      // head down while sprinting: never a slide
    CHECK(!A.fromHead);
    CHECK(Decide(A, true, false, false, false) == Act::Crouch);   // the sprint ended: now crouch
    CHECK(A.fromHead);
}
static void slide_allowed_while_sprinting()
{
    Arb A;
    CHECK(Decide(A, true, false, true, true) == Act::Crouch);     // physical_crouch_slide on: the crouch is sent mid-sprint (the slide)
    CHECK(A.fromHead);
    CHECK(Decide(A, true, true, true, true) == Act::None);        // the game is down (sliding / crouched): no second tap
    CHECK(Decide(A, false, true, false, true) == Act::Stand);     // head back up: released, as any head crouch
    CHECK(!A.fromHead);
}
static void pad_crouch_never_stood_up()
{
    Arb A;
    CHECK(Decide(A, false, true, false, false) == Act::None);     // crouched with the button, head up: left alone
    CHECK(Decide(A, true, true, false, false) == Act::None);      // already crouched: nothing to add
    CHECK(!A.fromHead);
    CHECK(Decide(A, false, true, false, false) == Act::None);
}
static void head_crouch_stands_up()
{
    Arb A;
    CHECK(Decide(A, true, false, false, false) == Act::Crouch);
    CHECK(Decide(A, true, true, false, false) == Act::None);      // the game is crouched now, head still down
    CHECK(Decide(A, false, true, false, false) == Act::Stand);    // head back up: stand, once
    CHECK(!A.fromHead);
    CHECK(Decide(A, false, true, false, false) == Act::None);     // still crouched in game this poll: not asked twice
}
static void crouch_not_repeated_while_game_catches_up()
{
    Arb A;                                                 // the pad crouch is a TOGGLE (Data3 inputs_pad.scr:67): a second tap undoes it
    CHECK(Decide(A, true, false, false, false) == Act::Crouch);
    CHECK(Decide(A, true, false, false, false) == Act::None);     // the game has not crouched yet: no second tap
    CHECK(Decide(A, false, false, false, false) == Act::None);    // head back up before it ever crouched: nothing to stand from
    CHECK(!A.fromHead);                                    // ... and the request is forgotten
    CHECK(Decide(A, true, false, false, false) == Act::Crouch);   // so the next real crouch asks again
}
static void pad_stand_wins_over_head()
{
    Arb A;
    CHECK(Decide(A, true, false, false, false) == Act::Crouch);
    CHECK(Decide(A, true, true, false, false) == Act::None);
    CHECK(Decide(A, true, false, false, false) == Act::None);     // the player stood with the button, head still low: not re-crouched
    CHECK(Decide(A, false, false, false, false) == Act::None);    // head up: the head crouch is over
    CHECK(!A.fromHead);
}
static void eye_cancel_only_from_head()
{
    CHECK(Near(EyeCancelY(0.62f, -0.18f, true), 0.80f, 1e-6f));
    CHECK(EyeCancelY(0.62f, -0.18f, false) == 0.0f);       // a button crouch keeps the game's own eye drop
    CHECK(EyeCancelY(0.62f, 0.70f, true) == 0.0f);         // never lifts the eye above standing
}

int main()
{
    learns_standing_height();
    crouch_does_not_lower_standing();
    standing_drifts_down_slowly();
    hysteresis_no_flicker();
    invalid_drops_refused();
    no_slide_while_sprinting();
    slide_allowed_while_sprinting();
    pad_crouch_never_stood_up();
    head_crouch_stands_up();
    crouch_not_repeated_while_game_catches_up();
    pad_stand_wins_over_head();
    eye_cancel_only_from_head();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
