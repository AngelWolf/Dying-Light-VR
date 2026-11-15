// Standalone unit test for src/src/ovr2xr/handcarry_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-22-hand-carried-model.md Task 2. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\handcarry_logic_test.cpp
#include "handcarry_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
#include "bow_nock_hand.inc"   // kNockArrowInHandR / T - the real base the bow arrow slides back from
}

using namespace dlvr::handcarry;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static In Carrying()
{
    In i{};
    i.wantCarry = true;
    i.active = true;
    i.sameObject = true;
    i.stillOn = true;
    i.relChanged = false;
    return i;
}

// Jay 2026-10-22 "Tune in the headset": tilt / turn about the nock in r_hand's frame, then a slide across the hand (+Y).
static void Adjust_zero_is_identity()
{
    const float rel[12] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f };
    float out[12];
    AdjustAboutNock(rel, 0.0f, 0.0f, 0.0f, out);
    CHECK(std::memcmp(out, rel, sizeof(out)) == 0);
}

static void Adjust_turn_about_hand_z_keeps_the_nock()
{
    const float rel[12] = { 1, 0, 0, 0.11f, 0, 1, 0, 0.02f, 0, 0, 1, -0.05f };   // arrow +X, nock at (0.11 0.02 -0.05)
    float out[12];
    AdjustAboutNock(rel, 0.0f, 90.0f, 0.0f, out);   // turn 90: the arrow's +X column -> hand +Y
    CHECK(Near(out[0], 0.0f, 1e-6f));
    CHECK(Near(out[4], 1.0f, 1e-6f));
    CHECK(Near(out[3], 0.11f, 1e-6f));
    CHECK(Near(out[7], 0.02f, 1e-6f));
    CHECK(Near(out[11], -0.05f, 1e-6f));
}

static void Adjust_tilt_about_hand_y()
{
    const float rel[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float out[12];
    AdjustAboutNock(rel, 90.0f, 0.0f, 0.0f, out);   // tilt 90 about +Y: +X column -> -Z
    CHECK(Near(out[0], 0.0f, 1e-6f));
    CHECK(Near(out[8], -1.0f, 1e-6f));
}

// Jay 2026-10-22 run 2: the hand's Y (the knuckle line) reads VERTICAL in the hold -> "1": the side slide runs HORIZONTALLY, along
// Y x shaft (perpendicular to both the knuckle line and the shaft). Shaft +Z here -> Y x Z = +X.
static void Adjust_side_moves_horizontally_across_the_shaft()
{
    const float rel[12] = { 1, 0, 0, 0.11f, 0, 1, 0, 0.02f, 0, 0, 1, -0.05f };
    float out[12];
    AdjustAboutNock(rel, 0.0f, 0.0f, 0.01f, out);
    CHECK(Near(out[3], 0.12f, 1e-6f));
    CHECK(Near(out[7], 0.02f, 1e-6f));    // never along the knuckle line
    CHECK(Near(out[11], -0.05f, 1e-6f));
    // a shaft along +X (rotated 90 about Y): Y x X = -Z
    const float relX[12] = { 0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0 };
    AdjustAboutNock(relX, 0.0f, 0.0f, 0.01f, out);
    CHECK(Near(out[3], 0.0f, 1e-6f));
    CHECK(Near(out[7], 0.0f, 1e-6f));
    CHECK(Near(out[11], -0.01f, 1e-6f));
    const int rot[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
    for (int k : rot) { CHECK(out[k] == relX[k]); }   // a slide never rotates
}

static void SlideBack_identity()
{
    const float base[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0 };
    float out[12];
    SlideBack(base, 0.05f, out);
    CHECK(Near(out[3], 0.1f, 1e-6f));
    CHECK(Near(out[7], 0.0f, 1e-6f));
    CHECK(Near(out[11], -0.05f, 1e-6f));
}

static void SlideBack_rotated()
{
    const float* R = kNockArrowInHandR;
    const float* T = kNockArrowInHandT;
    const float base[12] = { R[0], R[1], R[2], T[0], R[3], R[4], R[5], T[1], R[6], R[7], R[8], T[2] };
    float out[12];
    SlideBack(base, 0.05f, out);
    CHECK(Near(out[3], +0.113762f - 0.05f * +0.848704f, 1e-6f));
    CHECK(Near(out[7], +0.011991f - 0.05f * -0.104871f, 1e-6f));
    CHECK(Near(out[11], -0.077989f - 0.05f * -0.518366f, 1e-6f));
    const int rot[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
    for (int k : rot) { CHECK(out[k] == base[k]); }   // rotation bit-identical
}

static void SlideBack_zero()
{
    const float base[12] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f };
    float out[12];
    SlideBack(base, 0.0f, out);
    CHECK(std::memcmp(out, base, sizeof(out)) == 0);
}

static void Decide_take()
{
    In i{};
    i.wantCarry = true;
    CHECK(Decide(i) == Act::Take);
}

static void Decide_hold_none()
{
    CHECK(Decide(Carrying()) == Act::None);
    In idle{};
    CHECK(Decide(idle) == Act::None);
}

static void Decide_giveback_on_catch()
{
    In i = Carrying();
    i.wantCarry = false;
    CHECK(Decide(i) == Act::GiveBack);
}

static void Decide_forget_beats_giveback()
{
    In i = Carrying();
    i.wantCarry = false;
    i.sameObject = false;
    CHECK(Decide(i) == Act::Forget);
    i.wantCarry = true;   // still carrying when the bow went away: forget too, never touch the old pointer
    CHECK(Decide(i) == Act::Forget);
}

static void Decide_retake()
{
    In i = Carrying();
    i.stillOn = false;
    CHECK(Decide(i) == Act::Retake);
}

static void Decide_setlocal_on_change()
{
    In i = Carrying();
    i.relChanged = true;   // any of the four sliders moved
    CHECK(Decide(i) == Act::SetLocal);
}

static void Decide_no_double_take()
{
    In i{};
    i.wantCarry = true;
    CHECK(Decide(i) == Act::Take);
    In j = Carrying();
    j.wantCarry = false;   // released the next update
    CHECK(Decide(j) == Act::GiveBack);
    In k{};
    k.wantCarry = true;    // taken again: inactive after the give-back -> exactly one Take
    CHECK(Decide(k) == Act::Take);
    In l = Carrying();     // and held: no second Take
    CHECK(Decide(l) == Act::None);
}

// Final review fix 2: a new player body while carrying -> give it back (never re-take onto the old body), then take again.
static void Decide_body_changed_gives_back()
{
    In i = Carrying();
    i.bodyChanged = true;
    CHECK(Decide(i) == Act::GiveBack);
    i.sameObject = false;   // the bow went too: forget still wins
    CHECK(Decide(i) == Act::Forget);
}

// Final review fix 5: a zero arrow read with the same bow is the same carry, not a changed object.
static void SameObject_rules()
{
    int bow = 0, arrow = 0, other = 0;
    CHECK(SameObject(&bow, &arrow, &bow, &arrow));
    CHECK(SameObject(&bow, nullptr, &bow, &arrow));     // a transient zero read: hold
    CHECK(!SameObject(&other, &arrow, &bow, &arrow));   // a different bow
    CHECK(!SameObject(&bow, &other, &bow, &arrow));     // a different arrow object
    CHECK(!SameObject(&other, nullptr, &bow, &arrow));  // a different bow and no arrow
}

// Final review fix 1: BowNockTick did not run during the last update while a carry is held -> the watch must act.
static void TickMissed_rules()
{
    CHECK(TickMissed(true, 5, 5));    // carrying, no tick since the last watch
    CHECK(!TickMissed(true, 6, 5));   // ticked
    CHECK(!TickMissed(false, 5, 5));  // not carrying
}

// Final review fix 4: the object's own local L composes after the slid relation (drawn = r_hand x slid x L).
static void ComposeLocal_identity_and_offset()
{
    const float slid[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0.2f, 0, 0, 1, 0.3f };
    const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    float out[12];
    ComposeLocal(slid, ident, out);
    CHECK(std::memcmp(out, slid, sizeof(out)) == 0);
    const float rotZ90[12] = { 0, -1, 0, 0.01f, 1, 0, 0, 0, 0, 0, 1, 0 };   // L: 90 deg about Z, t (0.01 0 0)
    const float rotX90[12] = { 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0 };      // slid: 90 deg about X, t 0
    ComposeLocal(rotX90, rotZ90, out);
    CHECK(Near(out[3], 0.01f, 1e-6f));                  // L's t through slid's rotation: X stays X
    CHECK(Near(out[1], -1.0f, 1e-6f));                  // R = Rx(90) . Rz(90): row 0 = (0, -1, 0)
    CHECK(Near(out[8], 1.0f, 1e-6f));                   // row 2 = (1, 0, 0)
}

// Crossbow plan Task 3: bolt_pose's world carries a uniform scale of 3.22526 (INV-676), so the object's own local L is not rigid -
// the inverse is a GENERAL 3x3 one. A rotation (30 deg about X, then 50 deg about Z) scaled by 3.22526 with a translation.
static void ScaledL(float L[12])
{
    const float s = 3.22526f;
    const float kD = 0.01745329252f;
    const float ca = std::cos(30.0f * kD), sa = std::sin(30.0f * kD);
    const float cb = std::cos(50.0f * kD), sb = std::sin(50.0f * kD);
    const float Rx[9] = { 1, 0, 0, 0, ca, -sa, 0, sa, ca };
    const float Rz[9] = { cb, -sb, 0, sb, cb, 0, 0, 0, 1 };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            L[r * 4 + c] = s * (Rz[r * 3] * Rx[c] + Rz[r * 3 + 1] * Rx[3 + c] + Rz[r * 3 + 2] * Rx[6 + c]);
        }
    }
    L[3] = 0.0123f;
    L[7] = -0.0456f;
    L[11] = 0.0789f;
}

static void InvertAffineRoundTrip()
{
    float L[12];
    ScaledL(L);
    float inv[12];
    CHECK(InvertAffine(L, inv));
    float out[12];
    ComposeLocal(L, inv, out);   // m x inv == I (rotation identity, translation zero)
    const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    for (int k = 0; k < 12; ++k) { CHECK(Near(out[k], ident[k], 1e-5f)); }
    ComposeLocal(inv, L, out);   // and inv x m
    for (int k = 0; k < 12; ++k) { CHECK(Near(out[k], ident[k], 1e-5f)); }
    // the scale really is in it: inv's column 0 has length 1 / 3.22526 (a rigid transpose-inverse would give 3.22526)
    CHECK(Near(std::sqrt(inv[0] * inv[0] + inv[4] * inv[4] + inv[8] * inv[8]), 1.0f / 3.22526f, 1e-5f));
}

static void InvertAffineRefusesSingular()
{
    const float flat[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0.2f, 0, 0, 0, 0.3f };   // z collapsed: det 0
    float out[12];
    for (int k = 0; k < 12; ++k) { out[k] = 7.0f; }
    CHECK(!InvertAffine(flat, out));
    for (int k = 0; k < 12; ++k) { CHECK(out[k] == 7.0f); }   // refused = out untouched
    const float zero[12] = {};
    CHECK(!InvertAffine(zero, out));
    const float tiny[12] = { 1e-5f, 0, 0, 0, 0, 1e-5f, 0, 0, 0, 0, 1e-5f, 0 };   // det 1e-15, below 1e-12
    CHECK(!InvertAffine(tiny, out));
}

static void TakeAsComposesToDrawn()
{
    float L[12];
    ScaledL(L);
    // a drawn bolt frame in l_hand (unit scale, rotated, offset) - the object must be DRAWN at onElem x drawn
    const float drawn[12] = { 0, -1, 0, 0.031f, 0, 0, -1, -0.012f, 1, 0, 0, 0.087f };
    float rel[12];
    CHECK(RelForDrawn(drawn, L, rel));
    float local[12];
    ComposeLocal(rel, L, local);   // what HandCarryTake sets on the hand: rel x L
    for (int k = 0; k < 12; ++k) { CHECK(Near(local[k], drawn[k], 1e-5f)); }
    const float flat[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    CHECK(!RelForDrawn(drawn, flat, rel));   // a singular L refuses
}

int main()
{
    InvertAffineRoundTrip();
    InvertAffineRefusesSingular();
    TakeAsComposesToDrawn();
    Adjust_zero_is_identity();
    Adjust_turn_about_hand_z_keeps_the_nock();
    Adjust_tilt_about_hand_y();
    Adjust_side_moves_horizontally_across_the_shaft();
    Decide_body_changed_gives_back();
    SameObject_rules();
    TickMissed_rules();
    ComposeLocal_identity_and_offset();
    SlideBack_identity();
    SlideBack_rotated();
    SlideBack_zero();
    Decide_take();
    Decide_hold_none();
    Decide_giveback_on_catch();
    Decide_forget_beats_giveback();
    Decide_retake();
    Decide_setlocal_on_change();
    Decide_no_double_take();
    std::printf("handcarry_logic_test: %d failure(s)\n", g_fail);
    return g_fail;
}
