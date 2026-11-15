// Standalone unit test for src/src/ovr2xr/magrail_logic.h - NOT part of the DLL build. The held mag on the well's line (Jay
// 2026-11-02 (4)). Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\magrail_logic_test.cpp
#include "magrail_logic.h"

#include <cmath>
#include <cstdio>

using namespace dlvr::reload::rail;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

namespace {

// Seat at (0, 0, 0) with the identity rotation; the out axis = -Y (down, out of the well).
const float kSeat[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
const float kOut[3] = { 0, -1, 0 };

Params P()
{
    Params p;
    p.clearM = 0.10f;
    p.easeM = 0.03f;
    p.captureM = 0.03f;
    p.alignDeg = 45.0f;
    return p;
}

// A mag pose: rotated by `deg` about Z, at (x, y, z).
void Mag(float deg, float x, float y, float z, float m[12])
{
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    const float r[12] = { c, -s, 0, x, s, c, 0, y, 0, 0, 1, z };
    for (int i = 0; i < 12; ++i) { m[i] = r[i]; }
}

bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }
bool Near12(const float a[12], const float b[12], float e = 1e-4f)
{
    for (int i = 0; i < 12; ++i) { if (!Near(a[i], b[i], e)) { return false; } }
    return true;
}

void MeasureReadsTravelLateralAndAngle()
{
    float m[12], s, lat, ang;
    Mag(30.0f, 0.02f, -0.05f, 0.0f, m);
    Measure(kSeat, kOut, m, &s, &lat, &ang);
    CHECK(Near(s, 0.05f));
    CHECK(Near(lat, 0.02f));
    CHECK(Near(ang, 30.0f, 0.05f));
}

// Pulling: the grabbed mag starts on the rail; a SIDEWAYS hand moves nothing - the mag stays on the well's line.
void PullSidewaysStaysOnTheLine()
{
    const Params p = P();
    State st;
    st.onRail = true;   // the driver sets it at the Grab
    float hand[12], w[12];
    Mag(20.0f, 0.15f, 0.0f, 0.0f, hand);   // the hand 15 cm to the side, no travel down the well
    const Kind k = Decide(st, 0.0f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Rail);
    Draw(k, kSeat, kOut, 0.0f, 0.0f, hand, p, w);
    CHECK(Near12(w, kSeat));
    CHECK(st.onRail);
}

// Pulling down: the mag moves along the line only, with the seat's rotation, until it clears.
void PullDownSlidesAlongTheLine()
{
    const Params p = P();
    State st;
    st.onRail = true;
    float hand[12], w[12], want[12];
    Mag(20.0f, 0.15f, -0.06f, 0.0f, hand);
    const Kind k = Decide(st, 0.06f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Rail);
    Draw(k, kSeat, kOut, 0.06f, 0.06f, hand, p, w);
    Mag(0.0f, 0.0f, -0.06f, 0.0f, want);
    CHECK(Near12(w, want));
}

// Past the clear point it eases to the hand - continuous at both ends, then Free (off the rail).
void PullEaseIsContinuousThenFree()
{
    const Params p = P();
    State st;
    st.onRail = true;
    float hand[12], w[12], railEnd[12];
    Mag(40.0f, 0.12f, -0.20f, 0.01f, hand);
    Mag(0.0f, 0.0f, -0.10f, 0.0f, railEnd);
    Kind k = Decide(st, 0.10f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Rail);
    Draw(k, kSeat, kOut, 0.10f, 0.10f, hand, p, w);
    CHECK(Near12(w, railEnd));
    k = Decide(st, 0.1001f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Ease);
    Draw(k, kSeat, kOut, 0.1001f, 0.1001f, hand, p, w);
    CHECK(Near12(w, railEnd, 0.01f));   // just past the clear point: still at the line's end
    k = Decide(st, 0.1299f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Ease);
    Draw(k, kSeat, kOut, 0.1299f, 0.1299f, hand, p, w);
    CHECK(Near12(w, hand, 0.01f));      // just before free: at the hand
    k = Decide(st, 0.13f, 0.0f, 0.0f, p);
    CHECK(k == Kind::Free);
    CHECK(!st.onRail);
    Draw(k, kSeat, kOut, 0.13f, 0.13f, hand, p, w);
    CHECK(Near12(w, hand));
}

// Inserting lined up: locks on in the ease zone, then a sideways hand never takes it out through the side.
void InsertLinedUpLocksOnAndHolds()
{
    const Params p = P();
    State st;
    float hand[12], w[12], s, lat, ang;
    Mag(10.0f, 0.01f, -0.115f, 0.0f, hand);
    Measure(kSeat, kOut, hand, &s, &lat, &ang);
    Kind k = Decide(st, s, lat, ang, p);
    CHECK(k == Kind::Ease);
    CHECK(st.onRail);
    Mag(10.0f, 0.12f, -0.05f, 0.0f, hand);   // now 12 cm to the side, 5 cm out
    Measure(kSeat, kOut, hand, &s, &lat, &ang);
    k = Decide(st, s, lat, ang, p);
    CHECK(k == Kind::Rail);
    Draw(k, kSeat, kOut, s, s, hand, p, w);
    float want[12];
    Mag(0.0f, 0.0f, -0.05f, 0.0f, want);
    CHECK(Near12(w, want));
}

// Inserting at a bad angle: stopped at the lip (moved back out to the clear point, the hand's rotation kept).
void InsertBadAngleStopsAtTheLip()
{
    const Params p = P();
    State st;
    float hand[12], w[12], s, lat, ang;
    Mag(60.0f, 0.01f, -0.04f, 0.0f, hand);
    Measure(kSeat, kOut, hand, &s, &lat, &ang);
    const Kind k = Decide(st, s, lat, ang, p);
    CHECK(k == Kind::Lip);
    CHECK(!st.onRail);
    Draw(k, kSeat, kOut, s, s, hand, p, w);
    float want[12];
    Mag(60.0f, 0.01f, -0.10f, 0.0f, want);
    CHECK(Near12(w, want));
}

// A bad angle above the clear point is free (not yet at the lip); a mag wide of the line is free.
void InsertWideOrNotYetAtTheLipIsFree()
{
    const Params p = P();
    State st;
    CHECK(Decide(st, 0.115f, 0.01f, 60.0f, p) == Kind::Free);
    CHECK(Decide(st, 0.05f, 0.10f, 0.0f, p) == Kind::Free);
    CHECK(!st.onRail);
}

void NonFiniteIsFreeAndDropsTheRail()
{
    const Params p = P();
    State st;
    st.onRail = true;
    CHECK(Decide(st, NAN, 0.0f, 0.0f, p) == Kind::Free);
    CHECK(!st.onRail);
}

}  // namespace

int main()
{
    MeasureReadsTravelLateralAndAngle();
    PullSidewaysStaysOnTheLine();
    PullDownSlidesAlongTheLine();
    PullEaseIsContinuousThenFree();
    InsertLinedUpLocksOnAndHolds();
    InsertBadAngleStopsAtTheLip();
    InsertWideOrNotYetAtTheLipIsFree();
    NonFiniteIsFreeAndDropsTheRail();
    std::printf("%s - %d failure(s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
